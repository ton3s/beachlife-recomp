#include "launcher.h"
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include "build_version.h"
#include "content_service.h"
#include "image.h"
#include "platform/app_icon.h"
#include "platform/game_log.h"
#include "release_notes.h"
#include "ui.h"
#include "update_install.h"
#include "update_service.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_stdlib.h"
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace beachlife::launcher
{
namespace
{
namespace fs = std::filesystem;
namespace color = ui::color;
using ui::col;
using ui::dp;
using ui::Font;
using ui::Icon;

constexpr std::array<const char*, PageCount> pages = {
    "Play", "Game files", "Gameplay", "Display", "Appearance", "Advanced", "About"};
constexpr std::array<Icon, PageCount> page_icons = {
    Icon::Play, Icon::Disc, Icon::Sun, Icon::Monitor, Icon::Sliders, Icon::Gear, Icon::Info};
constexpr const char* times = "\xC3\x97"; // multiplication sign

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message) + ": " + SDL_GetError());
    }
}

// A download's size, "9.3 MB".
std::string megabytes(std::uint64_t bytes)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

// The settings file: --config, or launcher.cfg in the per-user preferences folder.
fs::path settings_file(const Options& options)
{
    if (!options.config.empty())
    {
        return options.config;
    }
    char* preference = SDL_GetPrefPath("BeachLife", "BeachLife");
    require(preference != nullptr, "Cannot locate your settings folder");
    auto path = path_from_utf8(preference) / "launcher.cfg";
    SDL_free(preference);
    return path;
}

ImVec2 add(ImVec2 a, ImVec2 b)
{
    return {a.x + b.x, a.y + b.y};
}
ImTextureID texture_id(SDL_Texture* texture)
{
    return static_cast<ImTextureID>(reinterpret_cast<intptr_t>(texture));
}

struct DialogResult
{
    std::mutex mutex;
    bool pending = false;
    bool completed = false;
    std::string path;
    std::string error;
};

void SDLCALL dialog_result(void* data, const char* const* files, int)
{
    std::unique_ptr<std::shared_ptr<DialogResult>> owner(
        static_cast<std::shared_ptr<DialogResult>*>(data));
    auto& result = **owner;
    const std::lock_guard lock(result.mutex);
    if (!files)
    {
        result.error = SDL_GetError();
    }
    else if (files[0])
    {
        result.path = files[0];
    }
    result.completed = true;
    result.pending = false;
}

// Panel backgrounds are drawn after their content, once its height is known.
class PanelScope
{
  public:
    PanelScope(ImDrawList* draw, ImVec2 min, float width)
        : draw_(draw)
        , min_(min)
        , width_(width)
    {
        draw_->ChannelsSplit(2);
        draw_->ChannelsSetCurrent(1);
    }
    void end(float bottom, float opacity = 0.62f)
    {
        draw_->ChannelsSetCurrent(0);
        ui::panel(draw_, min_, {min_.x + width_, bottom}, opacity);
        draw_->ChannelsMerge();
    }

  private:
    ImDrawList* draw_;
    ImVec2 min_;
    float width_;
};

// How the game source's state is marked: an icon and its tint.
struct Badge
{
    Icon kind;
    ImVec4 tint;
};

class Launcher
{
  public:
    explicit Launcher(const Options& options)
        : options_(options)
        , page_(options.page)
    {
    }
    ~Launcher()
    {
        content_.reset(); // cancel and finish file work before tearing down the interface
        if (renderer_backend_)
        {
            ImGui_ImplSDLRenderer3_Shutdown();
        }
        if (platform_backend_)
        {
            ImGui_ImplSDL3_Shutdown();
        }
        if (ImGui::GetCurrentContext())
        {
            ImGui::DestroyContext();
        }
        SDL_DestroyTexture(logo_.texture);
        SDL_DestroyTexture(artwork_.sharp);
        SDL_DestroyTexture(artwork_.soft);
        SDL_DestroyTexture(ramp_);
        SDL_DestroyTexture(spot_);
        SDL_DestroyRenderer(renderer_);
        SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    int run()
    {
        initialize();
        int frame = 0;
        bool running = true;
        while (running)
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                {
                    close_requested_ = true; // file work stops when the launcher actually closes
                }
                if (event.type == SDL_EVENT_DROP_FILE && event.drop.data)
                {
                    settings_.source = event.drop.data;
                    select_source();
                    page_ = PageFiles;
                }
            }
            consume_dialog();
            poll_content();
            update_ = updates_->snapshot();
            prompt_update();
            // The new files are in place: restart into them, after any unsaved settings.
            if (update_.state == UpdateService::State::Installed && !restart_)
            {
                restart_ = true;
                close_requested_ = true;
            }
            if (options_.smoke && options_.screenshot.empty())
            {
                page_ = (frame / 3) % PageCount;
            }
            ImGui_ImplSDL3_NewFrame();
            const auto display = ImGui::GetIO().DisplaySize;
            scale_ = std::min({settings_.scale / 100.0f, display.x / 960.0f, display.y / 620.0f});
            ui::set_scale(scale_);
            update_fonts();
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui::GetStyle() = base_style_;
            ImGui::GetStyle().ScaleAllSizes(scale_);
            ImGui::NewFrame();
            draw(display);
            if (play_)
            {
                running = false;
            }
            if (close_requested_)
            {
                if (settings_ == saved_)
                {
                    running = false;
                }
                else
                {
                    ImGui::OpenPopup("Save changes?");
                }
                close_requested_ = false;
            }
            draw_update_dialog();
            if (!draw_close_prompt())
            {
                running = false;
            }
            ImGui::Render();
            // Everything is laid out in window points. On high-density screens (Retina) SDL
            // scales points to pixels; the ImGui backend leaves geometry to this scale.
            const auto framebuffer = ImGui::GetIO().DisplayFramebufferScale;
            require(SDL_SetRenderScale(renderer_, framebuffer.x, framebuffer.y),
                "Cannot scale the launcher");
            require(SDL_SetRenderDrawColor(renderer_, 7, 18, 31, 255), "Cannot set the background");
            require(SDL_RenderClear(renderer_), "Cannot clear the launcher");
            render_backdrop();
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
            if (!options_.screenshot.empty() && frame == 4)
            {
                auto* surface = SDL_RenderReadPixels(renderer_, nullptr);
                require(surface != nullptr, "Cannot capture the launcher");
                const bool saved = SDL_SavePNG(surface, path_utf8(options_.screenshot).c_str());
                SDL_DestroySurface(surface);
                require(saved, "Cannot save the screenshot");
                running = false;
            }
            require(SDL_RenderPresent(renderer_), "Cannot present the launcher");
            ++frame;
            if (options_.smoke && frame >= 3 * PageCount)
            {
                running = false;
            }
            SDL_Delay(8);
        }
        if (options_.smoke)
        {
            std::puts("Launcher window, artwork, fonts and pages verified.");
        }
        if (options_.smoke || !options_.screenshot.empty())
        {
            std::printf("Launcher renderer: %s\n", SDL_GetRendererName(renderer_));
        }
        return exit_code_;
    }

    // The updated launcher to start once this one is gone, or empty.
    fs::path restart_program() const
    {
        return restart_ ? installed_launcher(package_) : fs::path();
    }
    // Play was pressed: the game starts once the launcher is gone.
    bool play_requested() const
    {
        return play_;
    }
    bool prepare_only() const
    {
        return prepare_only_;
    }
    const std::string& preview_screen() const
    {
        return preview_screen_;
    }
    const Settings& settings() const
    {
        return settings_;
    }
    const fs::path& config() const
    {
        return config_;
    }

  private:
    void initialize()
    {
        SDL_SetAppMetadata("Beach Life", build::version, "org.beachlife.recomp");
        require(SDL_Init(SDL_INIT_VIDEO), "Cannot initialize the launcher");
        config_ = settings_file(options_);
        log_folder_ = game_log_folder(config_);
        try
        {
            saved_ = settings_ = load_settings(config_);
        }
        catch (const std::exception& error)
        {
            notice_ = error.what();
        }
        if (!options_.source.empty())
        {
            settings_.source = options_.source;
            settings_.content = {}; // an explicit selection always checks the image again
        }
        // Without a folder for prepared files the launcher still opens; the
        // Game files page reports why nothing can be prepared.
        auto cache = options_.cache;
        if (cache.empty())
        {
            try
            {
                cache = data::default_content_directory();
            }
            catch (const std::exception& error)
            {
                notice_ = error.what();
            }
        }
        content_ = std::make_unique<ContentService>(cache);
        content_->select(settings_);
        content_status_ = content_->snapshot();
        source_ = content_status_.source;
        const auto flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                           (options_.smoke ? SDL_WINDOW_HIDDEN : 0);
        window_ = SDL_CreateWindow(
            (std::string("Beach Life ") + build::version).c_str(), 1180, 760, flags);
        require(window_ != nullptr, "Cannot create the launcher window");
        SDL_SetWindowMinimumSize(window_, 900, 600);
#ifdef _WIN32
        const char* preferred = "direct3d12";
#elif defined(__APPLE__)
        const char* preferred = "metal";
#else
        const char* preferred = "vulkan";
#endif
        renderer_ = SDL_CreateRenderer(window_, options_.smoke ? "software" : preferred);
        // The launcher remains usable without a working GPU driver. This does
        // not choose or qualify the future native game's rendering backend.
        if (!renderer_ && !options_.smoke)
        {
            renderer_ = SDL_CreateRenderer(window_, "software");
        }
        require(renderer_ != nullptr, "Cannot create the launcher renderer");
        SDL_SetRenderVSync(renderer_, 1);
        const char* base = SDL_GetBasePath();
        require(base != nullptr, "Cannot locate launcher resources");
        const auto resources = path_from_utf8(base) / "assets" / "launcher";
        platform::set_app_icon(window_, resources.parent_path() / "app" / "icon.png");
        require(load_artwork(renderer_, path_utf8(resources / "header.jpg").c_str(), artwork_),
            "Cannot load header.jpg; keep the package resources with the application");
        require(load_picture(path_utf8(resources / "logo.png").c_str(), logo_),
            "Cannot load logo.png; keep the package resources with the application");
        // Updates: the package this launcher was unpacked from (none for a local build).
        package_ = package_folder(path_from_utf8(base));
        updates_ = std::make_unique<UpdateService>(parse_build_identity(build::json), package_);
        const bool offline = options_.smoke || !options_.screenshot.empty();
        if (!offline)
        {
            try
            {
                const auto replaced = finish_updates(package_);
                if (!replaced.empty())
                {
                    show_notice(describe_replacement(replaced, updates_->build()), true);
                }
            }
            catch (const std::exception& error)
            {
                show_notice(error.what());
            }
            if (!platform::unreported_crash(log_folder_).empty())
            {
                show_notice("The game stopped unexpectedly last time. Advanced > Game log has "
                            "its log, with what stopped it.");
            }
        }
        if (!options_.update_preview.empty())
        {
            // "<state>-dialog" also opens the update dialog.
            std::string state = options_.update_preview;
            constexpr std::string_view dialog = "-dialog";
            update_dialog_ = state.size() > dialog.size() && state.ends_with(dialog);
            if (update_dialog_)
            {
                state.resize(state.size() - dialog.size());
            }
            if (!updates_->preview(state))
            {
                throw std::runtime_error("Unknown update preview state.");
            }
        }
#if !(defined(TARGET_OS_IOS) && TARGET_OS_IOS)
        // The updater replaces a desktop package in place, which an iOS app cannot do.
        else if (settings_.updates.check && !offline)
        {
            check_updates(UpdatePrompt::Newer, update_channel());
        }
#endif
        ramp_ = create_ramp(renderer_);
        spot_ = create_spot(renderer_);
        require(ramp_ && spot_, "Cannot create the launcher shading");
        ui::set_textures(ramp_, spot_);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        if (!fs::is_regular_file(resources / "Roboto-Medium.ttf"))
        {
            throw std::runtime_error("The launcher font is missing. Restore the complete package.");
        }
        font_path_ = path_utf8(resources / "Roboto-Medium.ttf");
        ui::apply_theme(base_style_);
        platform_backend_ = ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
        require(platform_backend_, "Cannot initialize launcher input");
        renderer_backend_ = ImGui_ImplSDLRenderer3_Init(renderer_);
        require(renderer_backend_, "Cannot initialize launcher drawing");
    }

    // Glyphs are rasterized for the current scale and pixel density, so text stays crisp at every
    // size.
    void update_fonts()
    {
        const float density = std::max(1.0f, SDL_GetWindowPixelDensity(window_));
        const float raster = std::round(scale_ * density * 8) / 8;
        if (raster == raster_ && density == density_)
        {
            return;
        }
        if (!ui::load_fonts(font_path_, raster, density))
        {
            throw std::runtime_error(
                "Cannot load the launcher font. Restore the complete package.");
        }
        ImGui_ImplSDLRenderer3_DestroyFontsTexture();
        raster_ = raster;
        density_ = density;
    }

    // The artwork covers the window, top-aligned so the face stays whole; narrow windows crop the
    // palm first.
    void place_art(ImVec2 display)
    {
        const float fit = std::max(display.x / artwork_.width, display.y / artwork_.height);
        art_size_ = {artwork_.width * fit, artwork_.height * fit};
        art_min_ = {(display.x - art_size_.x) * 0.7f, 0};
    }

    // The artwork is copied by SDL beneath the interface: the software
    // renderer rejects textured triangles this large, texture copies work everywhere.
    void render_backdrop() const
    {
        const auto& io = ImGui::GetIO();
        auto copy = [&](SDL_Texture* texture, ImVec2 a, ImVec2 b)
        {
            float width = 0, height = 0;
            SDL_GetTextureSize(texture, &width, &height);
            const SDL_FRect source{(a.x - art_min_.x) / art_size_.x * width,
                (a.y - art_min_.y) / art_size_.y * height,
                (b.x - a.x) / art_size_.x * width,
                (b.y - a.y) / art_size_.y * height};
            const SDL_FRect target{a.x, a.y, b.x - a.x, b.y - a.y};
            require(
                SDL_RenderTexture(renderer_, texture, &source, &target), "Cannot draw the artwork");
        };
        copy(backdrop_page_ == PagePlay ? artwork_.sharp : artwork_.soft, {0, 0}, io.DisplaySize);
        // Frosted sidebar glass over the sharp artwork.
        if (backdrop_page_ == PagePlay)
        {
            copy(artwork_.soft, {0, 0}, {backdrop_side_, io.DisplaySize.y});
        }
    }

    void draw(ImVec2 display)
    {
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(display);
        ImGui::Begin("Beach Life",
            nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                ImGuiWindowFlags_NoBackground);
        auto* draw = ImGui::GetWindowDrawList();
        const float side = dp(232), footer = display.y - dp(68);
        place_art(display);
        backdrop_page_ = page_;
        backdrop_side_ = side;
        draw->AddRectFilled({0, 0}, display, col(color::ocean, page_ == PagePlay ? 0.08f : 0.8f));
        draw_sidebar(draw, display, side);
        if (page_ == PagePlay)
        {
            draw_play(draw, display, side, footer);
        }
        else
        {
            ImGui::SetCursorScreenPos({side, 0});
            ImGui::BeginChild("page",
                {display.x - side, footer},
                ImGuiChildFlags_None,
                ImGuiWindowFlags_NoBackground);
            draw_page(side, display.x - side);
            ImGui::EndChild();
        }
        draw_footer(draw, display, side, footer);
        draw_notice(draw, display, side, footer);
        ImGui::End();
    }

    void draw_sidebar(ImDrawList* draw, ImVec2 display, float side)
    {
        // Frosted glass: the blurred artwork under a deep tint, lit warmly from the top.
        draw->AddRectFilled(
            {0, 0}, {side, display.y}, col(color::deep, page_ == PagePlay ? 0.74f : 0.55f));
        // The game's logo heads every page, centred and as large as the menu below leaves room for.
        const float scale = ImGui::GetIO().DisplayFramebufferScale.x, logo_top = dp(30);
        const float nav_height = dp((PageCount - 1) * 50 + 44), chip_top = display.y - dp(78);
        const float room = chip_top - dp(24) - nav_height - dp(40) - ui::font_size(Font::Caption) -
                           dp(16) - logo_top;
        const float logo_height = std::clamp(room, dp(96), dp(150));
        SDL_Texture* logo = picture_texture(renderer_,
            logo_,
            static_cast<int>(std::lround(logo_height * logo_.width / logo_.height * scale)));
        float texture_width = 0, texture_height = 0;
        SDL_GetTextureSize(logo, &texture_width, &texture_height);
        // Drawn 1:1 on whole pixels, so the resampled edges stay crisp.
        const ImVec2 logo_size{texture_width / scale, texture_height / scale};
        const ImVec2 logo_at{std::round((side - logo_size.x) * 0.5f * scale) / scale,
            std::round(logo_top * scale) / scale};
        draw->PushClipRect({0, 0}, {side, display.y});
        ui::glow(draw,
            {side * 0.5f - dp(210), logo_at.y + logo_size.y * 0.5f - dp(190)},
            {side * 0.5f + dp(210), logo_at.y + logo_size.y * 0.5f + dp(190)},
            col(color::sun, 0.1f));
        draw->PopClipRect();
        draw->AddRectFilled({side - 1, 0}, {side, display.y}, col(color::white, 0.08f));

        draw->AddImage(texture_id(logo), logo_at, add(logo_at, logo_size));
        const char* caption = "NATIVE PC PORT";
        const float caption_y = logo_at.y + logo_size.y + dp(16);
        ui::tracked(draw,
            Font::Caption,
            {(side - ui::tracked_width(Font::Caption, caption, dp(2.2f))) * 0.5f, caption_y},
            col(color::muted),
            caption,
            dp(2.2f));
        const float x = dp(24), nav_top = caption_y + ui::font_size(Font::Caption) + dp(40);
        for (int i = 0; i < PageCount; ++i)
        {
            if (ui::nav_item(pages[i],
                    page_icons[i],
                    page_ == i,
                    {dp(12), nav_top + dp(i * 50)},
                    side - dp(24)))
            {
                page_ = i;
            }
        }

        const float bottom = display.y - dp(28);
        ui::chip(draw, {x, bottom - dp(50)}, "LAUNCHER PREVIEW", color::coral);
        ui::text(draw,
            Font::Caption,
            {x, bottom - dp(16)},
            col(color::dim),
            ui::fit(Font::Caption, build::version, side - dp(48)).c_str());
    }

    Badge source_badge() const
    {
        if (content_status_.state == ContentService::State::Ready)
        {
            return {Icon::Check, color::palm};
        }
        if (content_status_.busy())
        {
            return {Icon::Refresh, color::lagoon};
        }
        if (!source_.selected)
        {
            return {Icon::Disc, color::lagoon};
        }
        if (!source_.readable)
        {
            return {Icon::Warning, color::amber};
        }
        return {Icon::Check, color::palm};
    }

    void draw_play(ImDrawList* draw, ImVec2 display, float side, float footer)
    {
        // The artwork carries the page; its summary cards sit on a deep fade at the bottom.
        const float left = side + dp(40), right = display.x - dp(40), gap = dp(16);
        const float card_height = dp(116), card_y = footer - dp(26) - card_height;
        const float fade_end = card_y + card_height * 0.5f;
        ui::fade(draw,
            {side, fade_end - dp(240)},
            {display.x, fade_end},
            col(color::ocean, 0.94f),
            ui::Side::Bottom);
        draw->AddRectFilled({side, fade_end}, {display.x, footer}, col(color::ocean, 0.94f));
        const Badge status = source_badge();
        const float card_width = (right - left - 2 * gap) / 3;
        const auto& view = settings_.display;
        summary_card("files",
            {left, card_y},
            {card_width, card_height},
            Icon::Disc,
            status.tint,
            "Game files",
            content_status_.state == ContentService::State::Ready ? "Ready to play"
            : source_.readable                                    ? source_name()
            : source_.selected                                    ? source_.title
                                                                  : "Not selected yet",
            source_detail(),
            PageFiles);
        summary_card("language",
            {left + card_width + gap, card_y},
            {card_width, card_height},
            Icon::Globe,
            color::lagoon,
            "Game language",
            content_status_.language == "en" ? "English" : "Not detected yet",
            "Offered from your game data",
            PageFiles);
        summary_card("display",
            {left + 2 * (card_width + gap), card_y},
            {card_width, card_height},
            Icon::Monitor,
            color::sun,
            "Display",
            view.mode == WindowMode::Fullscreen
                ? "Fullscreen"
                : size_label(view.width, view.height, false) + " window",
            std::string(view.scaling == Scaling::Fit ? "Fit" : "Stretch") + " \xC2\xB7 VSync " +
                (view.vsync ? "on" : "off"),
            PageDisplay);
    }

    static std::string size_label(int width, int height, bool aspect)
    {
        std::string label = std::to_string(width) + " " + times + " " + std::to_string(height);
        if (!aspect)
        {
            return label;
        }
        for (const auto& [w, h, name] :
            {std::tuple{4, 3, "4:3"}, {16, 9, "16:9"}, {16, 10, "16:10"}, {5, 4, "5:4"}})
        {
            if (width * h == height * w)
            {
                return label + "  (" + name + ")";
            }
        }
        return label;
    }

    std::string source_name() const
    {
        auto path = path_from_utf8(settings_.source);
        if (!path.has_filename())
        {
            path = path.parent_path();
        }
        return path_utf8(path.filename());
    }

    std::string source_detail() const
    {
        if (content_status_.state == ContentService::State::Ready)
        {
            return std::to_string(content_status_.files) + " local files ready";
        }
        if (content_status_.busy())
        {
            return "Checking local game files";
        }
        if (!source_.selected)
        {
            return "Your disc image (.iso)";
        }
        if (!source_.readable)
        {
            return "Needs your attention";
        }
        char size[48];
        const double megabytes = static_cast<double>(source_.bytes) / (1024.0 * 1024.0);
        if (megabytes >= 1024)
        {
            std::snprintf(size, sizeof size, "ISO image \xC2\xB7 %.1f GB", megabytes / 1024);
        }
        else if (megabytes >= 1)
        {
            std::snprintf(size, sizeof size, "ISO image \xC2\xB7 %.0f MB", megabytes);
        }
        else
        {
            std::snprintf(
                size, sizeof size, "ISO image \xC2\xB7 %.0f KB", std::ceil(megabytes * 1024));
        }
        return size;
    }

    void summary_card(const char* id,
        ImVec2 pos,
        ImVec2 size,
        Icon kind,
        const ImVec4& tint,
        const char* title,
        const std::string& value,
        const std::string& detail,
        int target)
    {
        ImGui::SetCursorScreenPos(pos);
        if (ImGui::InvisibleButton(id, size))
        {
            page_ = target;
        }
        const bool hovered = ImGui::IsItemHovered();
        if (hovered)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        const float hover = ui::animate(ImGui::GetItemID(), hovered);
        auto* draw = ImGui::GetWindowDrawList();
        const bool file_name = std::string_view(id) == "files" && source_.readable;
        const ImVec2 a{pos.x, pos.y - dp(3) * hover}, b{pos.x + size.x, a.y + size.y};
        const float rounding = dp(16);
        ui::glow(draw,
            {a.x - dp(24), a.y - dp(6)},
            {b.x + dp(24), b.y + dp(34)},
            col({0, 0.01f, 0.03f, 1}, 0.5f));
        draw->AddRectFilled(
            a, b, col(ui::mix(color::deep, color::card, 0.35f * hover), 0.82f), rounding);
        ui::gradient(draw,
            a,
            {b.x, a.y + size.y * 0.5f},
            col(color::white, 0.05f),
            col(color::white, 0),
            rounding,
            false);
        draw->AddRect(a,
            b,
            col(ui::mix(
                {1, 1, 1, 0.08f}, {color::coral.x, color::coral.y, color::coral.z, 0.6f}, hover)),
            rounding,
            0,
            1);
        const ImVec2 chip{a.x + dp(18), a.y + dp(18)};
        draw->AddRectFilled(chip, {chip.x + dp(32), chip.y + dp(32)}, col(tint, 0.16f), dp(10));
        ui::icon(draw, kind, {chip.x + dp(16), chip.y + dp(16)}, dp(17), col(tint));
        ui::icon(draw,
            Icon::Chevron,
            {b.x - dp(24) + dp(3) * hover, chip.y + dp(16)},
            dp(16),
            col(ui::mix(color::dim, color::coral, hover)));
        ui::text(draw, Font::Caption, {chip.x + dp(44), chip.y + dp(8)}, col(color::muted), title);
        const float inner = size.x - dp(36);
        ui::text(draw,
            Font::Label,
            {a.x + dp(18), a.y + dp(62)},
            col(color::text),
            ui::fit(Font::Label, value, inner, file_name).c_str());
        ui::text(draw,
            Font::Caption,
            {a.x + dp(18), a.y + dp(87)},
            col(color::muted),
            ui::fit(Font::Caption, detail, inner).c_str());
    }

    // Section heading inside a panel: tinted icon tile, title and a short explanation.
    float section(ImDrawList* draw,
        ImVec2 pos,
        float width,
        Icon kind,
        const ImVec4& tint,
        const char* title,
        const char* description) const
    {
        draw->AddRectFilled(pos, {pos.x + dp(38), pos.y + dp(38)}, col(tint, 0.15f), dp(11));
        ui::icon(draw, kind, {pos.x + dp(19), pos.y + dp(19)}, dp(19), col(tint));
        const float x = pos.x + dp(54);
        ui::text_bold(draw, Font::Heading, {x, pos.y - dp(2)}, col(color::text), title);
        ui::text(draw,
            Font::Caption,
            {x, pos.y + dp(23)},
            col(color::muted),
            description,
            width - dp(54));
        return std::max(
                   dp(38), dp(23) + ui::text_size(Font::Caption, description, width - dp(54)).y) +
               dp(22);
    }

    // A tinted note with an icon, a title and wrapped detail; returns its height.
    float callout(ImDrawList* draw,
        ImVec2 pos,
        float width,
        Icon kind,
        const ImVec4& tint,
        const char* title,
        const char* detail) const
    {
        const float text_x = pos.x + dp(52), text_width = width - dp(68);
        const float height =
            std::max(dp(66), dp(40) + ui::text_size(Font::Caption, detail, text_width).y + dp(16));
        draw->AddRectFilled(pos, {pos.x + width, pos.y + height}, col(tint, 0.08f), dp(12));
        draw->AddRect(pos, {pos.x + width, pos.y + height}, col(tint, 0.28f), dp(12), 0, 1);
        ui::icon(draw, kind, {pos.x + dp(26), pos.y + dp(26)}, dp(19), col(tint));
        ui::text(draw, Font::Label, {text_x, pos.y + dp(14)}, col(color::text), title);
        ui::text(
            draw, Font::Caption, {text_x, pos.y + dp(38)}, col(color::muted), detail, text_width);
        return height;
    }

    void field_label(ImDrawList* draw, ImVec2 pos, const char* name) const
    {
        ui::tracked(draw, Font::Caption, pos, col(color::dim), name, dp(1.6f));
    }

    void draw_page(float side, float width)
    {
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float left = origin.x + dp(44), available = std::min(width - dp(88), dp(820));
        float y = origin.y + dp(38);
        if (page_ != PageAbout)
        {
            const char* description =
                page_ == PageFiles
                    ? "Choose the original files that belong to your copy of Beach Life."
                : page_ == PageGameplay ? "Optional changes that are not in the original game. "
                                          "Each one is off by default."
                : page_ == PageDisplay  ? "How the game window opens and how its picture is drawn."
                : page_ == PageAdvanced ? "Troubleshooting options. The defaults suit most players."
                                        : "Make the launcher comfortable on your screen.";
            ui::text_bold(draw, Font::Title, {left, y}, col(color::text), pages[page_]);
            y += ui::font_size(Font::Title) + dp(8);
            ui::text(draw, Font::Body, {left, y}, col(color::muted), description, available);
            y += ui::text_size(Font::Body, description, available).y + dp(26);
        }
        if (page_ == PageFiles)
        {
            y = draw_files(draw, left, y, available);
        }
        else if (page_ == PageGameplay)
        {
            y = draw_gameplay(draw, left, y, available);
        }
        else if (page_ == PageDisplay)
        {
            y = draw_display(draw, left, y, available);
        }
        else if (page_ == PageAppearance)
        {
            y = draw_appearance(draw, left, y, available);
        }
        else if (page_ == PageAdvanced)
        {
            y = draw_advanced(draw, left, y, available);
        }
        else
        {
            y = draw_about(draw, left, y, available);
        }
        ImGui::SetCursorScreenPos({side, y + dp(16)});
        ImGui::Dummy({1, 1});
        if (ImGui::GetScrollY() < ImGui::GetScrollMaxY())
        {
            // More below: fade the cut edge instead of clipping text mid-line.
            const ImVec2 at = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
            ui::fade(draw,
                {at.x, at.y + size.y - dp(48)},
                add(at, size),
                col(color::ocean, 0.92f),
                ui::Side::Bottom);
        }
    }

    float draw_files(ImDrawList* draw, float left, float y, float available)
    {
        const float pad = dp(24), ix = left + pad, iw = available - 2 * pad;
        PanelScope source_panel(draw, {left, y}, available);
        float iy = y + pad;
        iy += section(draw,
            {ix, iy},
            iw,
            Icon::Disc,
            color::lagoon,
            "Disc image",
            "An ISO image of your original Beach Life CD.");
        field_label(draw, {ix, iy}, "LOCATION");
        iy += dp(22);
        const float browse_width = dp(124), field_height = dp(44);
        ImGui::SetCursorScreenPos({ix, iy});
        ImGui::SetNextItemWidth(iw - browse_width - dp(12));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
            {dp(14), (field_height - ui::font_size(Font::Body)) * 0.5f});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, dp(12));
        if (ImGui::InputTextWithHint("##source",
                "Select an .iso file, paste its path, or drag it into this window",
                &settings_.source))
        {
            select_source();
        }
        ImGui::PopStyleVar(2);
        bool pending;
        {
            const std::lock_guard lock(dialog_->mutex);
            pending = dialog_->pending;
        }
        if (ui::secondary_button("browse",
                "Browse\xE2\x80\xA6",
                Icon::Folder,
                {ix + iw - browse_width, iy},
                {browse_width, field_height},
                !pending))
        {
            browse();
        }
        iy += field_height + dp(16);

        const Badge status = source_badge();
        iy += callout(draw,
            {ix, iy},
            iw,
            status.kind,
            status.tint,
            source_.title.c_str(),
            source_.selected ? source_.detail.c_str()
                             : "Choose the ISO image of your original Beach Life CD.");
        iy += pad;
        source_panel.end(iy);
        y = iy + dp(18);

        PanelScope content_panel(draw, {left, y}, available);
        iy = y + pad;
        const auto state = content_status_.state;
        const bool ready = state == ContentService::State::Ready;
        iy += section(draw,
            {ix, iy},
            iw,
            Icon::Folder,
            ready ? color::palm : color::lagoon,
            "Local game data",
            ready ? "Your prepared files are ready. The ISO can be disconnected."
                  : "Prepare your files once, then play from this computer. Your ISO stays "
                    "unchanged.");
        if (content_status_.busy())
        {
            ui::text(draw, Font::Body, {ix, iy}, col(color::text), "Checking local game files...");
            iy += dp(28);
        }
        else
        {
            std::string detail =
                ready ? std::to_string(content_status_.files) +
                            " files prepared. Verify and repair if files are damaged."
                : state == ContentService::State::Cancelled
                    ? "Preparation cancelled. You can resume when ready."
                    : "Play prepares the files in the game's loading screen. You can also prepare "
                      "them now.";
            if (!content_status_.error.empty())
            {
                detail = content_status_.error;
            }
            ui::text(draw,
                Font::Caption,
                {ix, iy},
                col(content_status_.error.empty() ? color::muted : color::amber),
                detail.c_str(),
                iw);
            iy += ui::text_size(Font::Caption, detail.c_str(), iw).y + dp(18);
            if (ui::secondary_button("prepare",
                    ready ? "Verify and repair" : "Prepare files",
                    Icon::Refresh,
                    {ix, iy},
                    {dp(190), dp(42)},
                    source_.readable && !update_busy()))
            {
                play(true);
            }
            iy += dp(54);
            if (ui::secondary_button("preview_screen",
                    "Preview title screen",
                    Icon::Monitor,
                    {ix, iy},
                    {dp(220), dp(42)},
                    (ready || source_.readable) && !update_busy()))
            {
                play(false, "Data/Screens/FrontScreen.tga");
            }
        }
        iy += dp(42) + pad;
        content_panel.end(iy);
        y = iy + dp(18);

        PanelScope language_panel(draw, {left, y}, available);
        iy = y + pad;
        iy += section(draw,
            {ix, iy},
            iw,
            Icon::Globe,
            color::lagoon,
            "Game language",
            "Only languages present in your game data will be offered. Installer languages are "
            "separate.");
        const ImVec2 box{ix, iy}, box_end{ix + std::min(iw, dp(380)), iy + dp(44)};
        draw->AddRectFilled(box, box_end, col({0.02f, 0.05f, 0.09f, 1}, 0.45f), dp(12));
        draw->AddRect(box, box_end, col(color::white, 0.06f), dp(12), 0, 1);
        ui::text(draw,
            Font::Body,
            {box.x + dp(14), box.y + (dp(44) - ui::font_size(Font::Body)) * 0.5f},
            col(ready ? color::text : color::dim),
            content_status_.language == "en" ? "English" : "Waiting for content detection");
        const ImVec2 caret{box_end.x - dp(22), box.y + dp(22)};
        const ImVec2 points[] = {{caret.x - dp(5), caret.y - dp(2.5f)},
            {caret.x, caret.y + dp(2.5f)},
            {caret.x + dp(5), caret.y - dp(2.5f)}};
        draw->AddPolyline(points, 3, col(color::dim), 0, dp(1.6f));
        iy += dp(44) + pad;
        language_panel.end(iy);
        y = iy + dp(20);

        ui::icon(draw, Icon::Shield, {left + dp(9), y + dp(8)}, dp(17), col(color::palm, 0.85f));
        ui::text(draw,
            Font::Caption,
            {left + dp(28), y + dp(1)},
            col(color::muted),
            "Selecting a source does not install, extract or modify your game files.",
            available - dp(28));
        return y + dp(20);
    }

    float row_spacing() const
    {
        return dp(26);
    }
    float control_width(float width) const
    {
        return std::clamp(width * 0.46f, dp(240), dp(380));
    }

    // One setting in a panel: its name and explanation on the left, with a hairline above all
    // but the first. Advances `y` and returns where its control (`height` tall) goes on the right.
    ImVec2 setting_row(ImDrawList* draw,
        float& y,
        float x,
        float width,
        bool first,
        const char* name,
        const char* description,
        float height) const
    {
        const float spacing = row_spacing(), control = control_width(width),
                    text_width = width - control - dp(28);
        if (!first)
        {
            draw->AddRectFilled({x, y - spacing * 0.5f},
                {x + width, y - spacing * 0.5f + 1},
                col(color::white, 0.06f));
        }
        const float text_height = ui::font_size(Font::Label) + dp(5) +
                                  ui::text_size(Font::Caption, description, text_width).y;
        const float line = std::max(text_height, height), text_y = y + (line - text_height) * 0.5f;
        ui::text(draw, Font::Label, {x, text_y}, col(color::text), name);
        ui::text(draw,
            Font::Caption,
            {x, text_y + ui::font_size(Font::Label) + dp(5)},
            col(color::muted),
            description,
            text_width);
        const ImVec2 at{x + width - control, y + (line - height) * 0.5f};
        y += line + spacing;
        return at;
    }

    // A switch with its state written beside it.
    void switch_control(ImDrawList* draw, const char* id, bool* value, ImVec2 at)
    {
        ui::toggle(id, value, at);
        const float height = dp(ui::toggle_size.y);
        ui::text(draw,
            Font::Body,
            {at.x + dp(ui::toggle_size.x + 12), at.y + (height - ui::font_size(Font::Body)) * 0.5f},
            col(*value ? color::text : color::muted),
            *value ? "On" : "Off");
    }

    // "Primary display", then each connected display (`connected` choices in all); `selected`
    // is the saved number, or an extra last choice for a saved display that is not connected.
    std::vector<std::string> display_choices(
        int& selected, int& connected, int& max_width, int& max_height) const
    {
        std::vector<std::string> names = {"Primary display"};
        int count = 0;
        SDL_DisplayID* ids = SDL_GetDisplays(&count);
        SDL_DisplayID target = SDL_GetPrimaryDisplay();
        for (int i = 0; ids && i < count; ++i)
        {
            const char* name = SDL_GetDisplayName(ids[i]);
            std::string label = std::to_string(i + 1) + ". " + (name && *name ? name : "Display");
            if (const auto* mode = SDL_GetDesktopDisplayMode(ids[i]))
            {
                label += "  (" + size_label(mode->w, mode->h, false) + ")";
            }
            names.push_back(label);
            if (settings_.display.monitor == i + 1)
            {
                target = ids[i];
            }
        }
        SDL_free(ids);
        connected = static_cast<int>(names.size());
        selected = settings_.display.monitor;
        if (selected > count)
        {
            names.push_back("Display " + std::to_string(selected) + " (not connected)");
            selected = count + 1;
        }
        const auto* mode = target ? SDL_GetDesktopDisplayMode(target) : nullptr;
        max_width = mode ? mode->w : 0;
        max_height = mode ? mode->h : 0;
        return names;
    }

    float draw_display(ImDrawList* draw, float left, float y, float available)
    {
        auto& display = settings_.display;
        y += callout(draw,
                 {left, y},
                 available,
                 Icon::Info,
                 color::lagoon,
                 "Used by Play's game window",
                 "Play opens the game window with these settings.") +
             dp(18);
        const float pad = dp(24), ix = left + pad, iw = available - 2 * pad,
                    spacing = row_spacing(), field = dp(44);
        const float control = control_width(iw);
        auto row =
            [&](float& iy, bool first, const char* name, const char* description, float height)
        {
            return setting_row(draw, iy, ix, iw, first, name, description, height);
        };

        PanelScope window_panel(draw, {left, y}, available);
        float iy = y + pad;
        iy += section(draw,
            {ix, iy},
            iw,
            Icon::Monitor,
            color::lagoon,
            "Window",
            "Where and how the game window opens.");
        ImVec2 at = row(
            iy, true, "Mode", "Fullscreen covers the whole display at its own resolution.", field);
        int mode = display.mode == WindowMode::Fullscreen ? 1 : 0;
        if (ui::segmented("mode",
                &mode,
                {{"Windowed", Icon::None}, {"Fullscreen", Icon::None}},
                at,
                {control, field}))
        {
            display.mode = mode == 1 ? WindowMode::Fullscreen : WindowMode::Windowed;
        }

        int monitor = 0, connected = 0, max_width = 0, max_height = 0;
        const auto monitors = display_choices(monitor, connected, max_width, max_height);
        // Common sizes that fit the chosen display, plus the saved size whatever it is.
        static constexpr std::pair<int, int> presets[] = {{1024, 768},
            {1280, 720},
            {1280, 960},
            {1366, 768},
            {1600, 900},
            {1600, 1200},
            {1920, 1080},
            {2560, 1440},
            {3840, 2160}};
        std::vector<std::pair<int, int>> sizes;
        for (const auto& size : presets)
        {
            if (!max_width || (size.first <= max_width && size.second <= max_height))
            {
                sizes.push_back(size);
            }
        }
        const std::pair<int, int> current{display.width, display.height};
        if (std::find(sizes.begin(), sizes.end(), current) == sizes.end())
        {
            sizes.insert(std::upper_bound(sizes.begin(), sizes.end(), current), current);
        }
        std::vector<std::string> size_names;
        int size_index = 0;
        for (size_t i = 0; i < sizes.size(); ++i)
        {
            size_names.push_back(size_label(sizes[i].first, sizes[i].second, true));
            if (sizes[i] == current)
            {
                size_index = static_cast<int>(i);
            }
        }
        at = row(iy,
            false,
            "Window size",
            display.mode == WindowMode::Fullscreen ? "Used when the game runs in a window."
                                                   : "The game window's size on your desktop.",
            field);
        if (ui::dropdown("size", size_names, &size_index, at, {control, field}))
        {
            std::tie(display.width, display.height) = sizes[static_cast<size_t>(size_index)];
        }

        at = row(iy,
            false,
            "Display",
            connected > 2 ? "Where the game window opens." : "Only one display is connected.",
            field);
        // Choosing the "not connected" entry keeps the saved display.
        if (ui::dropdown("monitor", monitors, &monitor, at, {control, field}) &&
            monitor < connected)
        {
            display.monitor = monitor;
        }
        iy += pad - spacing;
        window_panel.end(iy);
        y = iy + dp(18);

        PanelScope picture_panel(draw, {left, y}, available);
        iy = y + pad;
        iy += section(draw,
            {ix, iy},
            iw,
            Icon::Sun,
            color::sun,
            "Picture",
            "How the game's picture fills the window.");
        at = row(iy,
            true,
            "Scaling",
            "Fit keeps the game's proportions, with bars where the window's shape differs. "
            "Stretch fills the whole window.",
            field);
        int scaling = display.scaling == Scaling::Stretch ? 1 : 0;
        if (ui::segmented("scaling",
                &scaling,
                {{"Fit", Icon::None}, {"Stretch", Icon::None}},
                at,
                {control, field}))
        {
            display.scaling = scaling == 1 ? Scaling::Stretch : Scaling::Fit;
        }

        const float toggle_height = dp(ui::toggle_size.y);
        at = row(iy,
            false,
            "VSync",
            "Waits for your display's refresh to prevent tearing.",
            toggle_height);
        switch_control(draw, "vsync", &display.vsync, at);

#ifdef _WIN32
        at = row(iy,
            false,
            "Graphics API",
            "Automatic lets the game choose for your graphics card.",
            field);
        int api = display.api == GraphicsApi::Automatic    ? 0
                  : display.api == GraphicsApi::Direct3D12 ? 1
                  : display.api == GraphicsApi::Vulkan     ? 2
                                                           : -1;
        if (ui::segmented("api",
                &api,
                {{"Automatic", Icon::None}, {"DirectX 12", Icon::None}, {"Vulkan", Icon::None}},
                at,
                {control, field}))
        {
            display.api = api == 1   ? GraphicsApi::Direct3D12
                          : api == 2 ? GraphicsApi::Vulkan
                                     : GraphicsApi::Automatic;
        }
#else
#ifdef __APPLE__
        const char* api_name = "Metal";
        at = row(iy, false, "Graphics API", "The game draws with Metal on macOS.", field);
#else
        const char* api_name = "Vulkan";
        at = row(iy, false, "Graphics API", "The game draws with Vulkan on Linux.", field);
#endif
        const ImVec2 box_end{at.x + control, at.y + field};
        draw->AddRectFilled(at, box_end, col({0.02f, 0.05f, 0.09f, 1}, 0.45f), dp(12));
        draw->AddRect(at, box_end, col(color::white, 0.06f), dp(12), 0, 1);
        ui::icon(draw, Icon::Check, {at.x + dp(22), at.y + field * 0.5f}, dp(14), col(color::palm));
        ui::text(draw,
            Font::Body,
            {at.x + dp(40), at.y + (field - ui::font_size(Font::Body)) * 0.5f},
            col(color::text),
            api_name);
#endif
        iy += pad - spacing;
        picture_panel.end(iy);
        y = iy + dp(18);

        PanelScope overlay_panel(draw, {left, y}, available);
        iy = y + pad;
        iy += section(draw,
            {ix, iy},
            iw,
            Icon::Info,
            color::palm,
            "Performance overlay",
            "FPS, system GPU and CPU usage.");
        at = row(
            iy, true, "On screen", "A compact row in a corner, also in fullscreen.", toggle_height);
        switch_control(draw, "fps_overlay", &display.fps_overlay, at);
        at = row(iy, false, "Corner", "Where the performance overlay sits.", field);
        static const std::vector<std::string> corners{
            "Top left", "Top right", "Bottom left", "Bottom right"};
        int corner = static_cast<int>(display.fps_overlay_corner);
        if (ui::dropdown("fps_corner", corners, &corner, at, {control, field}, display.fps_overlay))
        {
            display.fps_overlay_corner = static_cast<OverlayCorner>(corner);
        }
        iy += pad - spacing;
        overlay_panel.end(iy);
        return iy;
    }

    float draw_gameplay(ImDrawList* draw, float left, float y, float available)
    {
        const float pad = dp(24), ix = left + pad, iw = available - 2 * pad;
        PanelScope tweaks(draw, {left, y}, available);
        float iy = y + pad;
        iy += section(draw,
            {ix, iy},
            iw,
            Icon::Sliders,
            color::lagoon,
            "Gameplay options",
            "Each option changes the game only after you switch it on.");
        ImVec2 at = setting_row(draw,
            iy,
            ix,
            iw,
            true,
            "Skip intro",
            "Starts directly at the main menu, without the startup movie, the logo and the "
            "title screen.",
            dp(ui::toggle_size.y));
        switch_control(draw, "skip_intro", &settings_.tweaks.skip_intro, at);
        at = setting_row(draw,
            iy,
            ix,
            iw,
            false,
            "Mouse wheel zoom",
            "Rolling the wheel up zooms in and down zooms out, like Page Up and Page Down. While "
            "placing a building the wheel still rotates it.",
            dp(ui::toggle_size.y));
        switch_control(draw, "wheel_zoom", &settings_.tweaks.wheel_zoom, at);
        iy += pad - row_spacing();
        tweaks.end(iy);
        return iy;
    }

    float draw_advanced(ImDrawList* draw, float left, float y, float available)
    {
        const float pad = dp(24), ix = left + pad, iw = available - 2 * pad;
        PanelScope diagnostics(draw, {left, y}, available);
        float iy = y + pad;
        iy += section(draw,
            {ix, iy},
            iw,
            Icon::Terminal,
            color::lagoon,
            "Diagnostics & analytics",
            "Local tools for investigating problems. No usage data is collected or sent.");
        ImVec2 at = setting_row(draw,
            iy,
            ix,
            iw,
            true,
            "Log window",
            "Opens a separate window that follows the game's log while it runs. Not available in "
            "the runtime preview yet.",
            dp(ui::toggle_size.y));
        switch_control(draw, "log_window", &settings_.diagnostics.log_window, at);

        const std::string folder =
            path_utf8(config_.parent_path().empty() ? fs::current_path() : config_.parent_path());
        const std::string location = path_utf8(config_);
        at = setting_row(draw, iy, ix, iw, false, "Launcher settings", location.c_str(), dp(42));
        if (ui::secondary_button(
                "settings_folder", "Open folder", Icon::Folder, at, {control_width(iw), dp(42)}))
        {
            std::error_code ignored;
            fs::create_directories(path_from_utf8(folder), ignored);
            if (!SDL_OpenURL(file_url(folder).c_str()))
            {
                notice_ = std::string("Cannot open the folder: ") + SDL_GetError();
            }
        }
        const std::string logs = path_utf8(log_folder_);
        at = setting_row(draw, iy, ix, iw, false, "Game log", logs.c_str(), dp(42));
        if (ui::secondary_button(
                "log_folder", "Open folder", Icon::Folder, at, {control_width(iw), dp(42)}))
        {
            std::error_code ignored;
            fs::create_directories(path_from_utf8(logs), ignored);
            if (!SDL_OpenURL(file_url(logs).c_str()))
            {
                notice_ = std::string("Cannot open the folder: ") + SDL_GetError();
            }
        }
        iy += pad - row_spacing();
        diagnostics.end(iy);
        y = iy + dp(18);

        PanelScope updates(draw, {left, y}, available);
        iy = y + pad;
        iy += section(draw,
            {ix, iy},
            iw,
            Icon::Refresh,
            color::sun,
            "Updates",
            "Official releases and test builds from the project's GitHub page.");
        at = setting_row(draw,
            iy,
            ix,
            iw,
            true,
            "Check at start",
            "Looks for a newer build each time the launcher starts. Checks contact GitHub.",
            dp(ui::toggle_size.y));
        switch_control(draw, "check_updates", &settings_.updates.check, at);
        const std::string identity = describe_build(updates_->build()) + ".";
        at = setting_row(draw, iy, ix, iw, false, "This build", identity.c_str(), dp(42));
        if (ui::secondary_button(
                "build_page", "Open on GitHub", Icon::Link, at, {control_width(iw), dp(42)}))
        {
            const auto& build = updates_->build();
            const std::string page =
                build.origin == BuildOrigin::Test && build.pull_request
                    ? pull_request_page_url(build.repository, build.pull_request)
                    : releases_page_url(build.repository);
            if (!SDL_OpenURL(page.c_str()))
            {
                show_notice(std::string("Cannot open the page: ") + SDL_GetError());
            }
        }
        iy += pad - row_spacing();
        updates.end(iy);
        return iy;
    }

    // A file:// URL with every byte outside the unreserved set and '/' percent-encoded.
    static std::string file_url(const std::string& path)
    {
        std::string generic = path_utf8(path_from_utf8(path).generic_u8string());
        std::string url = generic.starts_with('/') ? "file://" : "file:///";
        for (const unsigned char c : generic)
        {
            if (std::isalnum(c) ||
                std::string_view("-._~/:").find(static_cast<char>(c)) != std::string_view::npos)
            {
                url += static_cast<char>(c);
            }
            else
            {
                char escaped[4];
                std::snprintf(escaped, sizeof escaped, "%%%02X", c);
                url += escaped;
            }
        }
        return url;
    }

    float draw_appearance(ImDrawList* draw, float left, float y, float available)
    {
        const float pad = dp(24), ix = left + pad, iw = available - 2 * pad;
        PanelScope size_panel(draw, {left, y}, available);
        float iy = y + pad;
        iy += section(draw,
            {ix, iy},
            iw,
            Icon::Sliders,
            color::sun,
            "Interface size",
            "Adjusts text and controls. The layout adapts to smaller windows.");
        int index = std::clamp((settings_.scale - 100) / 25, 0, 2);
        if (ui::segmented("scale",
                &index,
                {{"100%", Icon::None}, {"125%", Icon::None}, {"150%", Icon::None}},
                {ix, iy},
                {std::min(iw, dp(390)), dp(44)}))
        {
            settings_.scale = 100 + index * 25;
        }
        iy += dp(44) + pad;
        size_panel.end(iy);
        y = iy + dp(18);

        PanelScope later_panel(draw, {left, y}, available);
        iy = y + pad;
        iy += section(draw,
            {ix, iy},
            iw,
            Icon::Speaker,
            color::lagoon,
            "Audio",
            "Volume and sound settings will appear here once the game plays sound.");
        ui::chip(draw, {ix + dp(54), iy - dp(6)}, "COMING LATER", color::lagoon);
        iy += dp(23) + pad;
        later_panel.end(iy);
        return iy;
    }

    float draw_about(ImDrawList* draw, float left, float y, float available)
    {
        // The whole artwork as a postcard beside the title, uncropped.
        const float card_height = dp(168),
                    card_width = card_height * artwork_.width / artwork_.height;
        const ImVec2 card{left, y}, card_end{left + card_width, y + card_height};
        ui::glow(draw,
            {card.x - dp(28), card.y - dp(8)},
            {card_end.x + dp(28), card_end.y + dp(40)},
            col({0, 0.01f, 0.03f, 1}, 0.6f));
        draw->AddImageRounded(
            texture_id(artwork_.sharp), card, card_end, {0, 0}, {1, 1}, IM_COL32_WHITE, dp(14));
        draw->AddRect(card, card_end, col(color::white, 0.16f), dp(14), 0, 1);

        const float x = card_end.x + dp(30), width = left + available - x;
        float text_y = y + dp(4);
        ui::tracked(draw, Font::Caption, {x, text_y}, col(color::sun), "NATIVE PC PORT", dp(2.4f));
        text_y += ui::font_size(Font::Caption) + dp(10);
        ui::text_gradient(
            draw, Font::Title, {x, text_y}, col(color::sun), col(color::coral), "BEACH LIFE");
        const float title_width = ui::text_size(Font::Title, "BEACH LIFE").x + dp(16);
        ui::chip(draw,
            {x + title_width, text_y + dp(6)},
            ui::fit(Font::Caption, build::version, width - title_width - dp(18)).c_str(),
            color::sun);
        text_y += ui::font_size(Font::Title) + dp(10);
        const char* summary =
            "This first build lets you choose and remember your game source. Gameplay and "
            "game-language detection are still in development.";
        ui::text(draw, Font::Body, {x, text_y}, col(color::muted), summary, width);
        text_y += ui::text_size(Font::Body, summary, width).y + dp(18);
        if (ui::secondary_button(
                "project", "View project", Icon::Link, {x, text_y}, {dp(156), dp(42)}))
        {
            if (!SDL_OpenURL("https://github.com/yannicksuter/beachlife-recomp"))
            {
                notice_ = SDL_GetError();
            }
        }
        y = std::max(card_end.y, text_y + dp(42)) + dp(32);

        field_label(draw, {left, y}, "DESIGNED FOR");
        y += dp(24);
        const struct
        {
            const char* name;
            const char* api;
        } platforms[] = {
            {"Windows", "DirectX 12 \xC2\xB7 Vulkan"}, {"macOS", "Metal"}, {"Linux", "Vulkan"}};
        const float gap = dp(14), tile = (available - 2 * gap) / 3, tile_height = dp(64);
        for (int i = 0; i < 3; ++i)
        {
            const ImVec2 a{left + i * (tile + gap), y};
            ui::panel(draw, a, {a.x + tile, y + tile_height}, 0.62f);
            draw->AddRectFilled({a.x + dp(14), y + dp(14)},
                {a.x + dp(50), y + dp(50)},
                col(color::lagoon, 0.14f),
                dp(10));
            ui::icon(draw, Icon::Monitor, {a.x + dp(32), y + dp(32)}, dp(18), col(color::lagoon));
            ui::text(
                draw, Font::Label, {a.x + dp(64), y + dp(13)}, col(color::text), platforms[i].name);
            ui::text(draw,
                Font::Caption,
                {a.x + dp(64), y + dp(36)},
                col(color::muted),
                ui::fit(Font::Caption, platforms[i].api, tile - dp(76)).c_str());
        }
        y += tile_height + dp(26);

        field_label(draw, {left, y}, "CREDITS");
        y += dp(24);
        const char* credits[][2] = {{"Original game", "Deep Red Games / Eidos Interactive (2002)"},
            {"Artwork",
                "Remastered Beach Life art; logo from the LaunchBox Games Database. Rights remain "
                "with their "
                "respective owners."},
            {"Interface", "SDL3, Dear ImGui and Roboto"},
            {"Design and updater", "Adapted from Recharged"}};
        PanelScope credit_panel(draw, {left, y}, available);
        float iy = y + dp(8);
        const float value_x = left + dp(170);
        for (int i = 0; i < 4; ++i)
        {
            if (i)
            {
                draw->AddRectFilled({left + dp(20), iy},
                    {left + available - dp(20), iy + 1},
                    col(color::white, 0.06f));
            }
            const float height = std::max(dp(40),
                ui::text_size(Font::Body, credits[i][1], left + available - dp(20) - value_x).y +
                    dp(20));
            ui::text(
                draw, Font::Caption, {left + dp(20), iy + dp(12)}, col(color::dim), credits[i][0]);
            ui::text(draw,
                Font::Body,
                {value_x, iy + dp(10)},
                col(color::text, 0.88f),
                credits[i][1],
                left + available - dp(20) - value_x);
            iy += height;
        }
        iy += dp(8);
        credit_panel.end(iy);
        return iy;
    }

    bool update_busy() const
    {
        return update_.state == UpdateService::State::Downloading ||
               update_.state == UpdateService::State::Installing ||
               update_.state == UpdateService::State::Installed;
    }

    // Play can prepare a readable ISO or use a previously prepared, offline set.
    std::string play_blocker() const
    {
        if (update_busy())
        {
            return "Wait for the application update to finish.";
        }
        if (content_status_.busy())
        {
            return "Checking your game files.";
        }
        if (content_status_.state == ContentService::State::Ready)
        {
            return {};
        }
        if (!source_.selected)
        {
            return "Choose your game files first.";
        }
        if (!source_.readable)
        {
            return "Your game files need attention.";
        }
        return {};
    }

    void select_source(bool clear_binding = true)
    {
        if (clear_binding)
        {
            settings_.content = {};
        }
        failed_binding_.clear();
        content_->select(settings_);
        content_status_ = content_->snapshot();
        source_ = content_status_.source;
    }

    void setup_failed(const std::string& error)
    {
        show_notice(error);
    }

    void poll_content()
    {
        content_status_ = content_->snapshot();
        source_ = content_status_.source;
        using State = ContentService::State;
        if (content_status_.state == State::Ready)
        {
            const auto& binding = content_status_.binding;
            settings_.content = binding;
            // Remember completed setup without also saving display edits made while it ran.
            if (saved_.content != binding && saved_.source == binding.source &&
                failed_binding_ != binding.identity)
            {
                auto stored = saved_;
                stored.content = binding;
                try
                {
                    save_settings(config_, stored);
                    saved_ = std::move(stored);
                }
                catch (const std::exception& error)
                {
                    failed_binding_ = binding.identity;
                    setup_failed(error.what());
                    return;
                }
            }
        }
    }

    // The game window owns setup progress. Its worker rechecks the source stamp,
    // even if the launcher's readiness check succeeded earlier.
    void play(bool prepare_only = false, std::string preview_screen = {})
    {
        if (settings_ != saved_ && !save())
        {
            setup_failed(notice_);
            return;
        }
        content_->cancel(); // release the launcher's mount before setup/repair
        prepare_only_ = prepare_only;
        preview_screen_ = std::move(preview_screen);
        play_ = true;
    }

    UpdateChannel update_channel() const
    {
        switch (settings_.updates.channel)
        {
        case UpdateChoice::Release:
            return UpdateChannel::Release;
        case UpdateChoice::Test:
            return UpdateChannel::Test;
        case UpdateChoice::Automatic:
            break;
        }
        return updates_->build().origin == BuildOrigin::Test ? UpdateChannel::Test
                                                             : UpdateChannel::Release;
    }

    // The update dialog opens by itself when a check finds a build: after the start
    // only for a newer one; after the refresh button or the Release / Test switch also
    // for a switch to a test build or the way back to the release.
    enum class UpdatePrompt
    {
        None,
        Newer,
        Any
    };

    void check_updates(UpdatePrompt prompt, UpdateChannel channel)
    {
        update_prompt_ = prompt;
        updates_->check(channel);
    }

    // Once the check that asked for it is done: the dialog, when it found what was asked.
    void prompt_update()
    {
        if (update_prompt_ == UpdatePrompt::None || update_.state == UpdateService::State::Checking)
        {
            return;
        }
        if (update_.state == UpdateService::State::Available && update_.offer)
        {
            const auto& build = updates_->build();
            const OfferKind kind = classify_offer(build, *update_.offer);
            update_dialog_ = update_prompt_ == UpdatePrompt::Any || kind == OfferKind::Update ||
                             (kind == OfferKind::TestBuild && is_newer(build, *update_.offer));
        }
        // Nothing to install: the switch stands.
        if (!update_dialog_)
        {
            switch_from_.reset();
        }
        update_prompt_ = UpdatePrompt::None;
    }

    // The Release / Test switch is saved at once, without other unsaved changes, and
    // checks again. It holds when something gets installed or nothing is offered; Later
    // in the update dialog brings the previous choice back, since nothing changed.
    void select_channel(UpdateChannel channel)
    {
        if (!switch_from_)
        {
            switch_from_ = settings_.updates.channel;
        }
        save_channel(channel == UpdateChannel::Test ? UpdateChoice::Test : UpdateChoice::Release);
        check_updates(UpdatePrompt::Any, channel);
    }

    void save_channel(UpdateChoice choice)
    {
        settings_.updates.channel = choice;
        Settings stored = saved_;
        stored.updates.channel = choice;
        try
        {
            save_settings(config_, stored);
            saved_ = stored;
        }
        catch (const std::exception& error)
        {
            show_notice(error.what());
        }
    }

    // Later (or Escape) in the update dialog.
    void dismiss_update_dialog()
    {
        ImGui::CloseCurrentPopup();
        if (switch_from_)
        {
            const UpdateChoice previous = *switch_from_;
            switch_from_.reset();
            save_channel(previous);
            check_updates(UpdatePrompt::None, update_channel());
        }
    }

    void install_update()
    {
        switch_from_.reset();
        updates_->install();
    }

    void open_page(const std::string& url)
    {
        if (!SDL_OpenURL(url.c_str()))
        {
            show_notice(std::string("Cannot open the page: ") + SDL_GetError());
        }
    }

    void draw_footer(ImDrawList* draw, ImVec2 display, float side, float footer)
    {
        draw->AddRectFilled({side, footer}, display, col(color::ocean, 0.9f));
        draw->AddRectFilled({side, footer}, {display.x, footer + 1}, col(color::white, 0.07f));
        const bool dirty = settings_ != saved_;
        const float middle = footer + (display.y - footer) * 0.5f, height = dp(44),
                    top = middle - height * 0.5f;

        // Right-aligned actions on every page: Revert, Save, then Play, the main one.
        // Narrow windows keep Revert as an icon, so the update status keeps its room.
        const bool narrow = display.x - side < dp(820);
        float x = display.x - dp(24) - dp(150);
        const std::string blocker = play_blocker();
        if (ui::primary_button("play",
                "Play",
                Icon::Play,
                {x, top},
                {dp(150), height},
                blocker.empty(),
                blocker.empty()))
        {
            play();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s",
                blocker.empty() ? "Opens the game window. The game code is not reconstructed yet."
                                : blocker.c_str());
        }
        const float save_width = dp(narrow ? 108 : 124);
        x -= dp(12) + save_width;
        if (ui::secondary_button("save",
                dirty ? "Save" : "Saved",
                dirty ? Icon::Check : Icon::None,
                {x, top},
                {save_width, height},
                dirty))
        {
            save();
        }
        if (dirty)
        {
            draw->AddCircleFilled(
                {x + save_width - dp(10), top + dp(10)}, dp(4), col(color::amber));
        }
        const float revert_width = narrow ? height : dp(116);
        x -= dp(10) + revert_width;
        const bool reverted =
            narrow ? ui::icon_button("revert", Icon::Revert, "Revert", {x, top}, height, dirty)
                   : ui::secondary_button(
                         "revert", "Revert", Icon::Revert, {x, top}, {revert_width, height}, dirty);
        if (reverted)
        {
            settings_ = saved_;
            select_source(false);
        }

        // Updates on the left: check again, the channel, and what the last check found.
        const bool busy = update_.state == UpdateService::State::Downloading ||
                          update_.state == UpdateService::State::Installing ||
                          update_.state == UpdateService::State::Installed;
        float left = side + dp(24);
        if (ui::icon_button(
                "refresh", Icon::Refresh, "Check for updates", {left, top}, height, !busy))
        {
            check_updates(UpdatePrompt::Any, update_channel());
        }
        left += height + dp(10);
        int channel = update_channel() == UpdateChannel::Test ? 1 : 0;
        if (ui::dropdown(
                "channel", {"Release", "Test"}, &channel, {left, top}, {dp(112), height}) &&
            !busy)
        {
            select_channel(channel == 1 ? UpdateChannel::Test : UpdateChannel::Release);
        }
        left += dp(112) + dp(16);
        draw_update_status(draw, {left, middle}, x - dp(16) - left, height);
    }

    // What the last check found, and the one action it allows.
    void draw_update_status(ImDrawList* draw, ImVec2 at, float room, float height)
    {
        using State = UpdateService::State;
        const auto& build = updates_->build();
        Icon kind = Icon::None;
        ImVec4 tint = color::muted;
        std::string text, tooltip, action;
        bool spin = false;
        switch (update_.state)
        {
        case State::Idle:
            text = "Not checked yet";
            tooltip = describe_build(build) + ".";
            break;
        case State::Checking:
            spin = true;
            tint = color::lagoon;
            text = "Checking for updates\xE2\x80\xA6";
            break;
        case State::UpToDate:
            kind = Icon::Check;
            tint = color::palm;
            text = "Up to date";
            tooltip =
                describe_build(build) + ".\nChecked " + format_utc_time(update_.checked) + ".";
            break;
        case State::Available:
        {
            const auto& offer = *update_.offer;
            const OfferKind offer_kind = classify_offer(build, offer);
            kind = Icon::Download;
            tint = color::sun;
            text = offer_kind == OfferKind::Update
                       ? (offer.test ? "New test build ready"
                                     : "Version " + version_number(offer.version) + " ready")
                   : offer_kind == OfferKind::TestBuild
                       ? "Test build #" + std::to_string(offer.pull_request) + " ready"
                       : "Release " + version_number(offer.version) + " ready";
            action = !update_.install_blocker.empty()  ? "Download"
                     : offer_kind == OfferKind::Update ? "Update"
                                                       : "Install";
            tooltip = (offer.title.empty() ? offer.tag : offer.title) + ", published " +
                      format_utc_time(offer.published) + ".";
            if (!update_.install_blocker.empty())
            {
                tooltip += "\n" + update_.install_blocker;
            }
            break;
        }
        case State::CheckFailed:
            kind = Icon::Warning;
            tint = color::amber;
            text = "Update check failed";
            tooltip = update_.error;
            break;
        case State::Downloading:
        {
            kind = Icon::Download;
            tint = color::lagoon;
            const int percent =
                update_.total ? static_cast<int>(update_.received * 100 / update_.total) : 0;
            text = "Downloading " + std::to_string(percent) + "%";
            action = "Cancel";
            break;
        }
        case State::Installing:
            spin = true;
            tint = color::lagoon;
            text = "Installing\xE2\x80\xA6";
            break;
        case State::Installed:
            spin = true;
            tint = color::palm;
            text = "Restarting\xE2\x80\xA6";
            break;
        case State::InstallFailed:
            kind = Icon::Warning;
            tint = color::amber;
            text = "Update failed";
            tooltip = update_.error;
            action = "Retry";
            break;
        }
        const float action_width =
            action.empty() ? 0
                           : std::max(dp(96), ui::text_size(Font::Body, action.c_str()).x + dp(48));
        const float text_room = room - dp(26) - (action_width ? action_width + dp(12) : 0);
        if (spin)
        {
            ui::spinner(draw, {at.x + dp(9), at.y}, dp(7), col(tint));
        }
        else
        {
            ui::icon(draw, kind, {at.x + dp(9), at.y}, dp(17), col(tint));
        }
        if (text_room >= dp(36))
        {
            const float y = at.y - ui::font_size(Font::Body) * 0.5f;
            ui::text(draw,
                Font::Body,
                {at.x + dp(26), y},
                col(update_.state == State::Idle ? color::muted : color::text, 0.92f),
                ui::fit(Font::Body, text, text_room).c_str());
            if (update_.state == State::Downloading && update_.total)
            {
                const float bar = std::min(text_room, ui::text_size(Font::Body, text.c_str()).x);
                const float fraction =
                    static_cast<float>(update_.received) / static_cast<float>(update_.total);
                draw->AddRectFilled({at.x + dp(26), y + dp(21)},
                    {at.x + dp(26) + bar, y + dp(23)},
                    col(color::white, 0.12f),
                    dp(1));
                ui::gradient(draw,
                    {at.x + dp(26), y + dp(21)},
                    {at.x + dp(26) + bar * fraction, y + dp(23)},
                    col(color::sun),
                    col(color::coral),
                    dp(1),
                    true);
            }
        }
        if (!tooltip.empty() &&
            ImGui::IsMouseHoveringRect({at.x, at.y - height * 0.5f},
                {at.x + std::max(0.0f, text_room) + dp(26), at.y + height * 0.5f}))
        {
            ImGui::SetTooltip("%s", tooltip.c_str());
        }
        if (action.empty())
        {
            return;
        }
        const ImVec2 button{at.x + room - action_width, at.y - height * 0.5f};
        if (update_.state == State::Available)
        {
            // The dialog shows what the build changes before anything is installed.
            if (ui::primary_button("update",
                    action.c_str(),
                    Icon::Download,
                    button,
                    {action_width, height},
                    !play_))
            {
                update_dialog_ = true;
            }
        }
        else if (ui::secondary_button("update",
                     action.c_str(),
                     Icon::None,
                     button,
                     {action_width, height},
                     update_.state != State::InstallFailed || (!content_status_.busy() && !play_)))
        {
            if (update_.state == State::Downloading)
            {
                updates_->cancel();
            }
            else if (update_.state == State::InstallFailed)
            {
                updates_->install();
            }
        }
    }

    void show_notice(std::string text, bool success = false)
    {
        notice_ = std::move(text);
        notice_success_ = success;
        notice_until_ = success ? ImGui::GetTime() + 10 : 0;
    }

    // Messages sit above the footer until dismissed; good news goes by itself.
    void draw_notice(ImDrawList* draw, ImVec2 display, float side, float footer)
    {
        if (notice_.empty())
        {
            return;
        }
        if (notice_success_ && notice_until_ > 0 && ImGui::GetTime() > notice_until_)
        {
            notice_.clear();
            return;
        }
        const ImVec4 tint = notice_success_ ? color::palm : color::amber;
        const float wrap = std::min(dp(560), display.x - side - dp(48) - dp(96));
        const float height =
            std::max(dp(48), ui::text_size(Font::Body, notice_.c_str(), wrap).y + dp(28));
        const float width =
            std::min(ui::text_size(Font::Body, notice_.c_str(), wrap).x, wrap) + dp(96);
        const ImVec2 a{side + dp(24), footer - dp(14) - height}, b{a.x + width, footer - dp(14)};
        ui::glow(draw,
            {a.x - dp(30), a.y - dp(10)},
            {b.x + dp(30), b.y + dp(30)},
            col({0, 0.01f, 0.03f, 1}, 0.5f));
        draw->AddRectFilled(a, b, col(ui::mix(color::deep, color::card, 0.4f), 0.96f), dp(14));
        draw->AddRect(a, b, col(tint, 0.45f), dp(14), 0, 1);
        ui::icon(draw,
            notice_success_ ? Icon::Check : Icon::Warning,
            {a.x + dp(26), a.y + height * 0.5f},
            dp(17),
            col(tint));
        ui::text(draw,
            Font::Body,
            {a.x + dp(48), a.y + dp(14)},
            col(color::text),
            notice_.c_str(),
            wrap);
        if (ui::icon_button("dismiss",
                Icon::None,
                "Dismiss",
                {b.x - dp(38), a.y + (height - dp(28)) * 0.5f},
                dp(28)))
        {
            notice_.clear();
        }
        const ImVec2 cross{b.x - dp(24), a.y + height * 0.5f};
        draw->AddLine({cross.x - dp(5), cross.y - dp(5)},
            {cross.x + dp(5), cross.y + dp(5)},
            col(color::muted),
            dp(1.6f));
        draw->AddLine({cross.x - dp(5), cross.y + dp(5)},
            {cross.x + dp(5), cross.y - dp(5)},
            col(color::muted),
            dp(1.6f));
    }

    // Returns false when the launcher should close.
    bool draw_close_prompt()
    {
        const auto display = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(
            {display.x * 0.5f, display.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {dp(28), dp(24)});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, dp(18));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
        bool open = true;
        if (ImGui::BeginPopupModal("Save changes?",
                nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                    ImGuiWindowFlags_NoMove))
        {
            auto* draw = ImGui::GetWindowDrawList();
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float width = dp(420);
            ui::text_bold(draw,
                Font::Heading,
                pos,
                col(color::text),
                restart_ ? "Restart to finish the update?" : "Save your changes?");
            const char* message =
                restart_
                    ? "The update is installed. Save your launcher settings before it restarts?"
                    : "You changed launcher settings. Save them before closing?";
            ui::text(draw, Font::Body, {pos.x, pos.y + dp(34)}, col(color::muted), message, width);
            float y = pos.y + dp(34) + ui::text_size(Font::Body, message, width).y + dp(24);
            if (!notice_.empty())
            {
                ui::text(
                    draw, Font::Caption, {pos.x, y}, col(color::amber), notice_.c_str(), width);
                y += ui::text_size(Font::Caption, notice_.c_str(), width).y + dp(16);
            }
            const float height = dp(42);
            if (ui::secondary_button(
                    "discard", "Discard", Icon::None, {pos.x, y}, {dp(112), height}))
            {
                open = false;
            }
            if (ui::secondary_button("cancel",
                    "Cancel",
                    Icon::None,
                    {pos.x + width - dp(298), y},
                    {dp(108), height}))
            {
                restart_ = false; // the new files stay for the next start
                ImGui::CloseCurrentPopup();
            }
            if (ui::primary_button("save_close",
                    restart_ ? "Save and restart" : "Save and close",
                    Icon::None,
                    {pos.x + width - dp(178), y},
                    {dp(178), height}) &&
                save())
            {
                open = false;
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(3);
        return open;
    }

    // What the offered build is and changes, then its download and installation, over
    // the dimmed launcher.
    void draw_update_dialog()
    {
        if (update_dialog_ && update_.offer)
        {
            ImGui::OpenPopup("Update");
        }
        update_dialog_ = false;
        const auto display = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(
            {display.x * 0.5f, display.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {dp(28), dp(24)});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, dp(18));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
        if (ImGui::BeginPopupModal("Update",
                nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                    ImGuiWindowFlags_NoMove))
        {
            // A new check, or the installed update restarting, ends the dialog.
            if (!update_.offer || update_.state == UpdateService::State::Installed)
            {
                ImGui::CloseCurrentPopup();
            }
            else
            {
                draw_update_contents(*update_.offer, std::min(dp(560), display.x - dp(104)));
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(3);
    }

    void draw_update_contents(const PublishedBuild& offer, float width)
    {
        using State = UpdateService::State;
        auto* draw = ImGui::GetWindowDrawList();
        const auto& build = updates_->build();
        const OfferKind kind = classify_offer(build, offer);
        const ImVec4 tint = kind == OfferKind::Update      ? color::sun
                            : kind == OfferKind::TestBuild ? color::amber
                                                           : color::lagoon;
        const float caption = ui::font_size(Font::Caption);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        float y = pos.y;

        // What kind of build it is, which one and its title.
        const char* overline = kind == OfferKind::TestBuild       ? "INSTALL TEST BUILD"
                               : kind == OfferKind::BackToRelease ? "INSTALL RELEASE"
                               : offer.test                       ? "NEW TEST BUILD"
                                                                  : "UPDATE AVAILABLE";
        ui::icon(draw, Icon::Download, {pos.x + dp(8), y + caption * 0.5f}, dp(15), col(tint));
        ui::tracked(draw, Font::Caption, {pos.x + dp(24), y}, col(tint), overline, dp(1.4f));
        y += caption + dp(10);
        const std::string headline = offer.test
                                         ? "Pull request #" + std::to_string(offer.pull_request)
                                         : "Version " + version_number(offer.version);
        ui::text_bold(draw, Font::Heading, {pos.x, y}, col(color::text), headline.c_str());
        y += ui::font_size(Font::Heading) + dp(6);
        std::string title = offer.test && update_.pull && !update_.pull->title.empty()
                                ? update_.pull->title
                                : offer.title;
        if (title == "Beach Life " + version_number(offer.version))
        {
            title.clear(); // the headline says it
        }
        if (!title.empty())
        {
            ui::text(draw, Font::Body, {pos.x, y}, col(color::muted), title.c_str(), width);
            y += ui::text_size(Font::Body, title.c_str(), width).y;
        }
        y += dp(12);

        // From the running build to the offered one (equal numbers differ in their
        // commits), then its date and the download's size.
        const PortVersion mine = parse_version(build.version);
        const bool same_number = compare_versions(mine, offer.version) == 0;
        auto describe = [same_number](const PortVersion& version, const std::string& what)
        {
            std::string text = version_number(version);
            if (same_number && !version.commit.empty())
            {
                text += " (" + version.commit.substr(0, 8) + ")";
            }
            return text + "  \xC2\xB7  " + what;
        };
        const std::string from = describe(mine,
            build.origin == BuildOrigin::Release ? "Release"
            : build.origin == BuildOrigin::Test
                ? "Test build #" + std::to_string(build.pull_request)
                : "Local build");
        const std::string to = describe(offer.version,
            offer.test ? "Test build #" + std::to_string(offer.pull_request) : "Release");
        ui::text(draw, Font::Caption, {pos.x, y}, col(color::muted), from.c_str());
        float x = pos.x + ui::text_size(Font::Caption, from.c_str()).x + dp(8);
        ui::icon(draw, Icon::Chevron, {x + dp(7), y + caption * 0.55f}, dp(14), col(color::dim));
        x += dp(22);
        ui::text(draw, Font::Caption, {x, y}, col(ui::mix(color::text, tint, 0.3f)), to.c_str());
        y += caption + dp(6);
        const PackageAsset* asset = offer.package_for(build.package);
        std::string meta = offer.published ? "Published " + format_utc_time(offer.published) : "";
        if (asset && asset->size)
        {
            meta += (meta.empty() ? "" : "  \xC2\xB7  ") + megabytes(asset->size);
        }
        if (!meta.empty())
        {
            ui::text(draw, Font::Caption, {pos.x, y}, col(color::dim), meta.c_str());
            y += caption + dp(6);
        }
        y += dp(14);

        // A box of text with an icon: what a switch means, or what went wrong.
        auto callout = [&](const std::string& text, const ImVec4& ink, Icon kind_icon)
        {
            const float inner = width - dp(56);
            const float height = ui::text_size(Font::Body, text.c_str(), inner).y + dp(24);
            draw->AddRectFilled({pos.x, y}, {pos.x + width, y + height}, col(ink, 0.08f), dp(12));
            draw->AddRect({pos.x, y}, {pos.x + width, y + height}, col(ink, 0.28f), dp(12));
            ui::icon(draw,
                kind_icon,
                {pos.x + dp(22), y + dp(12) + ui::font_size(Font::Body) * 0.55f},
                dp(17),
                col(ink));
            ui::text(draw,
                Font::Body,
                {pos.x + dp(42), y + dp(12)},
                col(color::text, 0.9f),
                text.c_str(),
                inner);
            y += height + dp(18);
        };
        bool explained = false;
        if (kind != OfferKind::Update)
        {
            std::string meaning;
            if (kind == OfferKind::TestBuild && build.origin == BuildOrigin::Test)
            {
                meaning = "This replaces test build #" + std::to_string(build.pull_request) +
                          " by test build #" + std::to_string(offer.pull_request) +
                          ", whose updates you then get. Release in the footer takes you back "
                          "to the official release.";
            }
            else if (kind == OfferKind::TestBuild)
            {
                meaning = "Test builds have changes that are not released yet, to try them "
                          "early; they can have new problems. Release in the footer takes you "
                          "back.";
            }
            else
            {
                meaning = "This replaces your test build by the official release: changes that "
                          "are not released yet go away. Your settings stay.";
            }
            callout(meaning, tint, kind == OfferKind::TestBuild ? Icon::Warning : Icon::Info);
            explained = true;
        }

        // The release notes, or the test build's pull request description; the text
        // fades out where it scrolls.
        ui::tracked(draw,
            Font::Caption,
            {pos.x, y},
            col(color::dim),
            offer.test ? "PULL REQUEST" : "RELEASE NOTES",
            dp(1.2f));
        y += caption + dp(10);
        const std::string& source = offer.test && update_.pull ? update_.pull->body : offer.notes;
        if (source != notes_source_)
        {
            notes_source_ = source;
            notes_ = parse_release_notes(source);
        }
        float notes_room = ImGui::GetIO().DisplaySize.y * 0.36f;
        notes_room -= explained ? dp(64) : 0;
        notes_room -=
            update_.state == State::InstallFailed || !update_.install_blocker.empty() ? dp(56) : 0;
        ImGui::SetCursorScreenPos({pos.x, y});
        ImGui::SetNextWindowSizeConstraints(
            {0, dp(24)}, {FLT_MAX, std::clamp(notes_room, dp(90), dp(320))});
        ImGui::BeginChild("##notes",
            {width, 0},
            ImGuiChildFlags_AutoResizeY,
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
        {
            const float text_width =
                ImGui::GetContentRegionAvail().x - (ImGui::GetScrollMaxY() > 0 ? dp(10) : 0);
            if (notes_.empty())
            {
                const char* none = "No notes were published for this build.";
                ui::text(ImGui::GetWindowDrawList(),
                    Font::Body,
                    ImGui::GetCursorScreenPos(),
                    col(color::muted),
                    none,
                    text_width);
                ImGui::Dummy({text_width, ui::text_size(Font::Body, none, text_width).y});
            }
            else
            {
                draw_release_notes(notes_, text_width);
            }
            const float scroll = ImGui::GetScrollY(), scroll_max = ImGui::GetScrollMaxY();
            if (scroll_max > 0)
            {
                auto* notes_draw = ImGui::GetWindowDrawList();
                const ImVec2 at = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
                const float fade = dp(28), right = at.x + size.x - ImGui::GetStyle().ScrollbarSize;
                const ImVec4 fill = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
                const ImU32 solid = col(fill), clear = col(fill, 0);
                if (scroll < scroll_max - 0.5f)
                {
                    notes_draw->AddRectFilledMultiColor({at.x, at.y + size.y - fade},
                        {right, at.y + size.y},
                        clear,
                        clear,
                        solid,
                        solid);
                }
                if (scroll > 0.5f)
                {
                    notes_draw->AddRectFilledMultiColor(
                        at, {right, at.y + fade}, solid, solid, clear, clear);
                }
            }
        }
        ImGui::EndChild();
        y = ImGui::GetItemRectMax().y + dp(18);

        // Progress or problems, above the actions.
        if (update_.state == State::Downloading)
        {
            const float fraction =
                update_.total
                    ? std::min(1.0f,
                          static_cast<float>(update_.received) / static_cast<float>(update_.total))
                    : 0.0f;
            draw->AddRectFilled(
                {pos.x, y}, {pos.x + width, y + dp(4)}, col(color::white, 0.12f), dp(2));
            if (fraction > 0)
            {
                ui::gradient(draw,
                    {pos.x, y},
                    {pos.x + std::max(dp(4), width * fraction), y + dp(4)},
                    col(color::sun),
                    col(color::coral),
                    dp(2),
                    true);
            }
            y += dp(12);
            const std::string progress =
                "Downloading  \xC2\xB7  " + megabytes(update_.received) +
                (update_.total ? " of " + megabytes(update_.total) : std::string());
            ui::text(draw, Font::Caption, {pos.x, y}, col(color::muted), progress.c_str());
            if (update_.total)
            {
                const std::string percent =
                    std::to_string(update_.received * 100 / update_.total) + "%";
                ui::text(draw,
                    Font::Caption,
                    {pos.x + width - ui::text_size(Font::Caption, percent.c_str()).x, y},
                    col(color::text),
                    percent.c_str());
            }
            y += caption + dp(18);
        }
        else if (update_.state == State::Installing)
        {
            ui::spinner(draw, {pos.x + dp(8), y + caption * 0.5f}, dp(6), col(color::lagoon));
            ui::text(draw,
                Font::Caption,
                {pos.x + dp(24), y},
                col(color::muted),
                "Installing\xE2\x80\xA6");
            y += caption + dp(18);
        }
        else if (update_.state == State::InstallFailed)
        {
            callout(update_.error, color::coral, Icon::Warning);
        }
        else if (!update_.install_blocker.empty())
        {
            callout(update_.install_blocker +
                        " Download the package and unpack it over this folder instead.",
                color::muted,
                Icon::Info);
        }

        // The build's page on GitHub on the left, the actions on the right.
        draw->AddRectFilled({pos.x, y}, {pos.x + width, y + 1}, col(color::white, 0.08f));
        y += dp(18);
        const float height = dp(42), right = pos.x + width;
        if (ui::secondary_button(
                "update_page", "GitHub", Icon::Link, {pos.x, y}, {dp(124), height}))
        {
            open_page(offer.page);
        }
        const bool can_install = !content_status_.busy() && !play_;
        const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (update_.state == State::Downloading)
        {
            if (ui::secondary_button(
                    "update_cancel", "Cancel", Icon::None, {right - dp(108), y}, {dp(108), height}))
            {
                updates_->cancel();
            }
            // The download goes on in the footer.
            if (ui::secondary_button(
                    "update_hide", "Hide", Icon::None, {right - dp(226), y}, {dp(108), height}) ||
                escape)
            {
                ImGui::CloseCurrentPopup();
            }
        }
        else if (update_.state == State::Installing)
        {
            ui::primary_button("update_install",
                "Installing\xE2\x80\xA6",
                Icon::None,
                {right - dp(160), y},
                {dp(160), height},
                false);
        }
        else
        {
            const bool blocked = !update_.install_blocker.empty();
            const char* action = update_.state == State::InstallFailed ? "Retry"
                                 : blocked                             ? "Download"
                                 : kind == OfferKind::Update           ? "Update"
                                                                       : "Install";
            if (ui::primary_button("update_install",
                    action,
                    Icon::Download,
                    {right - dp(160), y},
                    {dp(160), height},
                    blocked || can_install))
            {
                if (blocked)
                {
                    open_page(offer.page);
                }
                else
                {
                    install_update();
                }
            }
            if (!blocked && !can_install &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip("%s",
                    play_ ? "Close the game window first."
                          : "Wait until your game files are checked.");
            }
            if (ui::secondary_button(
                    "update_later", "Later", Icon::None, {right - dp(278), y}, {dp(108), height}) ||
                escape)
            {
                dismiss_update_dialog();
            }
        }
    }

    // Notes blocks at the cursor, wrapped to `width`; words flow across their spans.
    static void draw_release_notes(const std::vector<NoteBlock>& blocks, float width)
    {
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        float y = origin.y;
        bool first = true;
        for (const auto& block : blocks)
        {
            const bool heading = block.kind == NoteBlock::Kind::Heading;
            const Font which =
                heading ? (block.level <= 1 ? Font::Heading : Font::Label) : Font::Body;
            ImFont* font = ui::font(which);
            const float size = ui::font_size(which), line_height = size * 1.35f;
            if (!first)
            {
                y += heading                               ? dp(14)
                     : block.kind == NoteBlock::Kind::Item ? dp(4)
                     : block.kind == NoteBlock::Kind::Code ? 0.0f
                                                           : dp(8);
            }
            first = false;
            if (block.kind == NoteBlock::Kind::Rule)
            {
                draw->AddRectFilled({origin.x, y + dp(6)},
                    {origin.x + width, y + dp(6) + 1},
                    col(color::white, 0.08f));
                y += dp(12);
                continue;
            }
            const float indent = block.kind == NoteBlock::Kind::Item
                                     ? dp(20) * static_cast<float>(block.level + 1)
                                     : 0;
            const float left = origin.x + indent, right = origin.x + width;
            if (block.kind == NoteBlock::Kind::Item)
            {
                if (block.marker.empty())
                {
                    draw->AddCircleFilled(
                        {left - dp(10), y + line_height * 0.5f}, dp(2.6f), col(color::sun, 0.85f));
                }
                else
                {
                    const float mark = ui::text_size(which, block.marker.c_str()).x;
                    ui::text(draw,
                        which,
                        {left - dp(6) - mark, y + (line_height - size) * 0.5f},
                        col(color::muted),
                        block.marker.c_str());
                }
            }
            // A line breaks before a word that does not fit; a single word wider than
            // the column overflows it.
            float x = left;
            for (const auto& span : block.spans)
            {
                const ImVec4 ink = span.style == NoteSpan::Style::Strong ? color::text
                                   : span.style == NoteSpan::Style::Code
                                       ? ui::mix(color::lagoon, color::text, 0.25f)
                                   : span.style == NoteSpan::Style::Link ? color::sun
                                   : heading ? color::text
                                             : ui::mix(color::muted, color::text, 0.5f);
                const std::string& text = span.text;
                std::size_t at = 0;
                while (at < text.size())
                {
                    std::size_t end = text.find(' ', at);
                    end = end == std::string::npos ? text.size() : end + 1;
                    const char* begin = text.c_str() + at;
                    const char* stop = text.c_str() + end;
                    const char* visible = stop;
                    while (visible > begin && visible[-1] == ' ')
                    {
                        --visible;
                    }
                    const float word = font->CalcTextSizeA(size, FLT_MAX, 0, begin, visible).x;
                    const float advance = font->CalcTextSizeA(size, FLT_MAX, 0, begin, stop).x;
                    if (x > left && x + word > right)
                    {
                        x = left;
                        y += line_height;
                        if (begin == visible)
                        {
                            at = end; // spaces at a line start
                            continue;
                        }
                    }
                    if (visible > begin)
                    {
                        draw->AddText(font,
                            size,
                            {x, y + (line_height - size) * 0.5f},
                            col(ink),
                            begin,
                            visible);
                    }
                    x += advance;
                    at = end;
                }
            }
            y += line_height;
        }
        ImGui::Dummy({width, std::max(0.0f, y - origin.y)});
    }

    void browse()
    {
        static const SDL_DialogFileFilter filters[] = {{"ISO disc image", "iso"}};
        {
            const std::lock_guard lock(dialog_->mutex);
            if (dialog_->pending)
            {
                return;
            }
            dialog_->pending = true;
        }
        auto* owner = new std::shared_ptr<DialogResult>(dialog_);
        SDL_ShowOpenFileDialog(dialog_result, owner, window_, filters, 1, nullptr, false);
    }

    void consume_dialog()
    {
        const std::lock_guard lock(dialog_->mutex);
        if (!dialog_->completed)
        {
            return;
        }
        if (!dialog_->error.empty())
        {
            notice_ = "Cannot open the file picker: " + dialog_->error +
                      ". You can paste the path instead.";
        }
        if (!dialog_->path.empty())
        {
            settings_.source = dialog_->path;
            select_source();
            notice_.clear();
        }
        dialog_->path.clear();
        dialog_->error.clear();
        dialog_->completed = false;
    }

    bool save()
    {
        try
        {
            if (!settings_.source.empty())
            {
                settings_.source =
                    path_utf8(fs::absolute(path_from_utf8(settings_.source)).lexically_normal());
            }
            save_settings(config_, settings_);
            saved_ = settings_;
            notice_.clear();
            return true;
        }
        catch (const std::exception& error)
        {
            notice_ = error.what();
            return false;
        }
    }

    Options options_;
    fs::path config_;
    fs::path log_folder_; // game_log_folder(config_)
    Settings settings_, saved_;
    SourceStatus source_;
    std::unique_ptr<ContentService> content_;
    ContentService::Status content_status_;
    std::string failed_binding_;
    bool prepare_only_ = false;
    std::string preview_screen_;
    int exit_code_ = 0;
    std::string notice_;
    std::string font_path_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    Artwork artwork_;
    Picture logo_;
    SDL_Texture* ramp_ = nullptr;
    SDL_Texture* spot_ = nullptr;
    ImGuiStyle base_style_;
    ImVec2 art_min_{}, art_size_{1, 1};
    bool platform_backend_ = false, renderer_backend_ = false, close_requested_ = false;
    int page_ = 0, backdrop_page_ = 0;
    float backdrop_side_ = 0;
    float scale_ = 1, density_ = 0, raster_ = 0;
    std::shared_ptr<DialogResult> dialog_ = std::make_shared<DialogResult>();
    fs::path package_;
    std::unique_ptr<UpdateService> updates_;
    UpdateService::Status update_;
    UpdatePrompt update_prompt_ = UpdatePrompt::None;
    bool update_dialog_ = false;
    std::optional<UpdateChoice> switch_from_;
    std::string notes_source_;
    std::vector<NoteBlock> notes_;
    bool restart_ = false, notice_success_ = false, play_ = false;
    double notice_until_ = 0;
};
} // namespace

fs::path game_log_folder(const fs::path& settings_file)
{
#ifdef __APPLE__
    // A Mac app lives in Applications, which it should not write beside; Console.app
    // lists ~/Library/Logs.
    if (const char* home = std::getenv("HOME"); home && *home)
    {
        return path_from_utf8(home) / "Library" / "Logs" / "Beach Life";
    }
#endif
    fs::path game;
    if (const char* base = SDL_GetBasePath())
    {
        game = package_folder(path_from_utf8(base));
        if (game.empty())
        {
            // A local build: its program folder, or the folder holding its app.
            game = path_from_utf8(base).parent_path();
            for (fs::path folder = game; folder.has_relative_path(); folder = folder.parent_path())
            {
                if (folder.extension() == ".app")
                {
                    game = folder.parent_path();
                    break;
                }
            }
        }
    }
    if (!game.empty())
    {
        const fs::path logs = game / "logs";
        std::error_code error;
        fs::create_directories(logs, error);
        const fs::path probe = logs / ".writable";
        if (std::ofstream(probe).good())
        {
            fs::remove(probe, error);
            return logs;
        }
    }
    return settings_file.parent_path() / "logs";
}

Outcome play_settings(const Options& options)
{
    Outcome outcome;
    try
    {
        SDL_SetAppMetadata("Beach Life", build::version, "org.beachlife.recomp");
        outcome.config = settings_file(options);
        const auto saved =
            load_settings(outcome.config); // unreadable settings stop here, untouched
        outcome.settings = saved;
        if (!options.source.empty())
        {
            outcome.settings.source =
                path_utf8(fs::absolute(path_from_utf8(options.source)).lexically_normal());
            outcome.settings.content = {}; // an explicit selection always checks the image again
        }
        if (outcome.settings != saved)
        {
            save_settings(outcome.config, outcome.settings);
        }
        outcome.play = true;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Beach Life: %s\n", error.what());
        if (!options.smoke)
        {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Beach Life", error.what(), nullptr);
        }
        outcome.exit_code = 1;
    }
    return outcome;
}

Outcome run(const Options& options)
{
    fs::path restart;
    std::string restart_error;
    Outcome outcome;
    try
    {
        Launcher launcher(options);
        outcome.exit_code = launcher.run();
        restart = launcher.restart_program();
        outcome.play = launcher.play_requested() && restart.empty();
        outcome.prepare_only = launcher.prepare_only();
        outcome.preview_screen = launcher.preview_screen();
        outcome.settings = launcher.settings();
        outcome.config = launcher.config();
#ifdef _WIN32
        // Windows lets only the foreground process bring a new window to the front:
        // start the updated launcher while this window still is, not after it closed.
        if (!restart.empty() && options.argv)
        {
            restart_error = restart_launcher(restart, options.argc, options.argv);
        }
#endif
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Beach Life: %s\n", error.what());
        if (!options.smoke)
        {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Beach Life", error.what(), nullptr);
        }
        outcome.exit_code = 1;
        return outcome;
    }
    if (restart.empty() || !options.argv)
    {
        return outcome;
    }
#ifdef _WIN32
    const std::string& error = restart_error;
#else
    // An update replaced the program files; the old window and SDL are gone.
    const std::string error = restart_launcher(restart, options.argc, options.argv);
#endif
    if (error.empty())
    {
        return outcome;
    }
    const std::string message =
        "The update is installed, but the launcher could not start again (" + error +
        "). Please start it again yourself.";
    std::fprintf(stderr, "Beach Life: %s\n", message.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Beach Life", message.c_str(), nullptr);
    outcome.exit_code = 1;
    return outcome;
}
} // namespace beachlife::launcher
