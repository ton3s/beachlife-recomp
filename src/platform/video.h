#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ImDrawData;

// The game's graphics boundary. The game (later its DirectDraw layer) composes
// whole frames at its own resolution; the video layer owns the window and the GPU
// and only ever shows finished frames, scaled into the window, with an optional
// ImGui overlay. One implementation uses SDL's GPU API, which runs on Direct3D 12
// and Vulkan on Windows, Metal on macOS and Vulkan on Linux; another provider
// could replace it behind this interface.
namespace beachlife::platform
{

// A frame in the game's resolution: 32-bit RGBA (red in the lowest byte), row by row.
struct Frame
{
    int width = 0, height = 0;
    std::vector<std::uint32_t> pixels;
};

enum class GraphicsApi
{
    Automatic,
    Direct3D12,
    Vulkan,
    Metal
};

struct VideoSettings
{
    std::string title = "Beach Life";
    bool fullscreen = false;        // borderless, at the display's own resolution
    int width = 1280, height = 960; // the window's size when not fullscreen
    int display = 0;                // 0 is the primary display, otherwise its 1-based position
    bool vsync = true;
    bool stretch = false; // fill the window instead of keeping the frame's proportions
    GraphicsApi api = GraphicsApi::Automatic;
    bool hidden = false;   // checks and screenshots
    bool headless = false; // tests and automation: offscreen window, no GPU
};

// What the video layer actually got.
struct VideoInfo
{
    std::string api;          // SDL's driver name: "vulkan", "direct3d12" or "metal"
    std::string present_mode; // "vsync", "mailbox" or "immediate"
};

// The GPU destination is in drawable pixels; SDL mouse coordinates are in
// window units. Keep both sizes so high-density windows use the same mapping.
struct PresentationTransform
{
    int width = 0, height = 0;
    int window_width = 0, window_height = 0;
    int pixel_width = 0, pixel_height = 0;
    SDL_Rect destination{};
    bool fullscreen = false; // the game's mode, including emulated fullscreen

    struct Point
    {
        double x, y;
    };
    bool valid() const
    {
        return width > 0 && height > 0 && window_width > 0 && window_height > 0 &&
               pixel_width > 0 && pixel_height > 0 && destination.w > 0 && destination.h > 0;
    }
    Point to_client(double x, double y) const
    {
        return {(x * pixel_width / window_width - destination.x) * width / destination.w,
            (y * pixel_height / window_height - destination.y) * height / destination.h};
    }
    Point to_window(double x, double y) const
    {
        return {(destination.x + x * destination.w / width) * window_width / pixel_width,
            (destination.y + y * destination.h / height) * window_height / pixel_height};
    }
};

// Used by presentation and input, including the exact integer rounding of bars.
inline SDL_Rect frame_rect(
    int frame_width, int frame_height, int window_width, int window_height, bool stretch)
{
    if (stretch || frame_width <= 0 || frame_height <= 0)
    {
        return {0, 0, window_width, window_height};
    }
    const double scale = std::min(static_cast<double>(window_width) / frame_width,
        static_cast<double>(window_height) / frame_height);
    const int width = std::min(window_width, static_cast<int>(frame_width * scale + 0.5));
    const int height = std::min(window_height, static_cast<int>(frame_height * scale + 0.5));
    return {(window_width - width) / 2, (window_height - height) / 2, width, height};
}

// Text the game asked for this frame, drawn sharply by the host in game coordinates.
struct HdGlyph
{
    unsigned int codepoint;
    int x, y, width, height;
    unsigned char red, green, blue;
    int clip_left, clip_top, clip_right, clip_bottom;
};
const std::vector<HdGlyph>& hd_glyphs();
void set_hd_text(bool enabled);

class Video
{
  public:
    virtual ~Video() = default;
    virtual SDL_Window* window() const = 0;
    virtual const VideoInfo& info() const = 0;
    // Before ImGui::NewFrame (the caller owns the ImGui context and its input).
    virtual void new_overlay_frame() = 0;
    // Shows `frame` with `overlay` (may be null) on top. Nothing is shown while the
    // window cannot present (minimized).
    virtual void present(const Frame& frame, ImDrawData* overlay) = 0;
    // Saves the next presented picture, overlay included, as a PNG file.
    virtual void save_next(const std::filesystem::path& file) = 0;

    // Set at mode selection, before the first frame/input query. Presentation
    // also supplies its frame size, for transports without an explicit mode.
    void set_game_mode(int width, int height, bool fullscreen)
    {
        set_frame_size(width, height);
        game_fullscreen_ = fullscreen;
    }
    void set_frame_size(int width, int height)
    {
        game_width_ = width;
        game_height_ = height;
    }
    virtual bool stretch() const
    {
        return false;
    }
    PresentationTransform presentation_transform(int pixel_width = 0, int pixel_height = 0) const
    {
        PresentationTransform result;
        if (!SDL_GetWindowSize(window(), &result.window_width, &result.window_height) ||
            ((!pixel_width || !pixel_height) &&
                !SDL_GetWindowSizeInPixels(window(), &pixel_width, &pixel_height)))
        {
            return result;
        }
        result.width = game_width_ > 0 ? game_width_ : result.window_width;
        result.height = game_height_ > 0 ? game_height_ : result.window_height;
        result.pixel_width = pixel_width;
        result.pixel_height = pixel_height;
        result.destination =
            frame_rect(result.width, result.height, pixel_width, pixel_height, stretch());
        result.fullscreen = game_fullscreen_;
        return result;
    }

    // The host's overlay for frames presented without one (the game's own flips),
    // built on demand at each such present; none when unset.
    void set_host_overlay(std::function<ImDrawData*()> overlay)
    {
        host_overlay_ = std::move(overlay);
    }

  protected:
    ImDrawData* host_overlay() const
    {
        return host_overlay_ ? host_overlay_() : nullptr;
    }

  private:
    int game_width_ = 0, game_height_ = 0;
    bool game_fullscreen_ = false;
    std::function<ImDrawData*()> host_overlay_;
};

// Opens the window and the GPU device on the requested API. An API that is not
// available here is an error naming it; there is no silent fallback to another one.
std::unique_ptr<Video> create_video(const VideoSettings& settings);
std::unique_ptr<Video> create_headless_video(const VideoSettings& settings);

} // namespace beachlife::platform
