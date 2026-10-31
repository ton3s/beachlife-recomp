#include "platform/path.h"
#include "platform/video.h"

#include "imgui.h"
#include "imgui_impl_sdlgpu3.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace beachlife::platform
{
namespace
{
[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error(message + ": " + SDL_GetError());
}

const char* driver_name(GraphicsApi api)
{
    switch (api)
    {
    case GraphicsApi::Direct3D12:
        return "direct3d12";
    case GraphicsApi::Vulkan:
        return "vulkan";
    case GraphicsApi::Metal:
        return "metal";
    case GraphicsApi::Automatic:
        break;
    }
    return nullptr;
}

const char* api_title(GraphicsApi api)
{
    switch (api)
    {
    case GraphicsApi::Direct3D12:
        return "Direct3D 12";
    case GraphicsApi::Vulkan:
        return "Vulkan";
    case GraphicsApi::Metal:
        return "Metal";
    case GraphicsApi::Automatic:
        break;
    }
    return "A supported graphics API";
}

class SdlGpuVideo : public Video
{
  public:
    explicit SdlGpuVideo(const VideoSettings& settings)
        : settings_(settings)
    {
        // Opened hidden, placed on its display, then shown.
        window_ = SDL_CreateWindow(settings.title.c_str(),
            settings.width,
            settings.height,
            SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!window_)
        {
            fail("Cannot open the game window");
        }
        int count = 0;
        SDL_DisplayID* displays = SDL_GetDisplays(&count);
        const SDL_DisplayID display = displays && settings.display > 0 && settings.display <= count
                                          ? displays[settings.display - 1]
                                          : SDL_GetPrimaryDisplay();
        SDL_free(displays);
        SDL_SetWindowPosition(window_,
            SDL_WINDOWPOS_CENTERED_DISPLAY(display),
            SDL_WINDOWPOS_CENTERED_DISPLAY(display));
        if (settings.fullscreen)
        {
            SDL_SetWindowFullscreenMode(
                window_, nullptr); // the display's own mode, without a mode change
            SDL_SetWindowFullscreen(window_, true);
        }

        // The overlay's shaders come as SPIR-V, DXBC and metallib; every backend takes one of them.
        device_ = SDL_CreateGPUDevice(
            SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXBC | SDL_GPU_SHADERFORMAT_METALLIB |
                SDL_GPU_SHADERFORMAT_MSL, // the iOS simulator's overlay compiles Metal source
            false,
            driver_name(settings.api));
        if (!device_)
        {
            fail(std::string(api_title(settings.api)) +
                 " is not available on this computer. Choose another graphics "
                 "API on the Display page");
        }
        if (!SDL_ClaimWindowForGPUDevice(device_, window_))
        {
            fail("Cannot draw into the game window");
        }
        claimed_ = true;
        info_.api = SDL_GetGPUDeviceDriver(device_);

        // Claiming the window sets up SDR with VSync. Without VSync, the fastest mode the
        // window supports; where that cannot be set, VSync stays (and is reported).
        SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
        if (!settings.vsync)
        {
            if (SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_IMMEDIATE))
            {
                mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
            }
            else if (SDL_WindowSupportsGPUPresentMode(
                         device_, window_, SDL_GPU_PRESENTMODE_MAILBOX))
            {
                mode = SDL_GPU_PRESENTMODE_MAILBOX;
            }
            if (mode != SDL_GPU_PRESENTMODE_VSYNC &&
                !SDL_SetGPUSwapchainParameters(
                    device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode))
            {
                mode = SDL_GPU_PRESENTMODE_VSYNC;
            }
        }
        info_.present_mode = mode == SDL_GPU_PRESENTMODE_VSYNC     ? "vsync"
                             : mode == SDL_GPU_PRESENTMODE_MAILBOX ? "mailbox"
                                                                   : "immediate";
        swapchain_format_ = SDL_GetGPUSwapchainTextureFormat(device_, window_);

        ImGui_ImplSDLGPU3_InitInfo overlay{};
        overlay.Device = device_;
        overlay.ColorTargetFormat = swapchain_format_;
        if (!ImGui_ImplSDLGPU3_Init(&overlay))
        {
            throw std::runtime_error("Cannot set up the overlay renderer");
        }
        overlay_ready_ = true;
        if (!settings.hidden)
        {
            SDL_ShowWindow(window_);
        }
    }

    ~SdlGpuVideo() override
    {
        if (device_)
        {
            SDL_WaitForGPUIdle(device_);
        }
        if (overlay_ready_)
        {
            ImGui_ImplSDLGPU3_Shutdown();
        }
        if (device_)
        {
            for (auto* texture : {frame_texture_, composite_})
            {
                if (texture)
                {
                    SDL_ReleaseGPUTexture(device_, texture);
                }
            }
            for (auto* buffer : {upload_, download_})
            {
                if (buffer)
                {
                    SDL_ReleaseGPUTransferBuffer(device_, buffer);
                }
            }
            if (claimed_)
            {
                SDL_ReleaseWindowFromGPUDevice(device_, window_);
            }
            SDL_DestroyGPUDevice(device_);
        }
        if (window_)
        {
            SDL_DestroyWindow(window_);
        }
    }

    SDL_Window* window() const override
    {
        return window_;
    }
    const VideoInfo& info() const override
    {
        return info_;
    }
    bool stretch() const override
    {
        return settings_.stretch;
    }
    void new_overlay_frame() override
    {
        // Fonts added since the atlas was uploaded: upload it again.
        if (!ImGui::GetIO().Fonts->IsBuilt())
        {
            ImGui_ImplSDLGPU3_DestroyFontsTexture();
        }
        ImGui_ImplSDLGPU3_NewFrame();
    }
    void save_next(const std::filesystem::path& file) override
    {
        screenshot_ = file;
    }

    void present(const Frame& frame, ImDrawData* overlay) override
    {
        if (!overlay)
        {
            overlay = host_overlay();
        }
        const auto width = static_cast<Uint32>(frame.width),
                   height = static_cast<Uint32>(frame.height);
        if (frame.pixels.size() != static_cast<std::size_t>(width) * height)
        {
            throw std::runtime_error("Malformed frame");
        }
        set_frame_size(frame.width, frame.height);
        prepare_frame(width, height);
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
        if (!commands)
        {
            fail("Cannot record GPU commands");
        }

        auto* mapped = SDL_MapGPUTransferBuffer(device_, upload_, true);
        if (!mapped)
        {
            fail("Cannot upload the frame");
        }
        std::memcpy(mapped, frame.pixels.data(), frame.pixels.size() * 4);
        SDL_UnmapGPUTransferBuffer(device_, upload_);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        const SDL_GPUTextureTransferInfo source{upload_, 0, width, height};
        SDL_GPUTextureRegion destination{};
        destination.texture = frame_texture_;
        destination.w = width;
        destination.h = height;
        destination.d = 1;
        SDL_UploadToGPUTexture(copy, &source, &destination, true);
        SDL_EndGPUCopyPass(copy);
        if (overlay)
        {
            Imgui_ImplSDLGPU3_PrepareDrawData(overlay, commands);
        }

        SDL_GPUTexture* swapchain = nullptr;
        Uint32 window_width = 0, window_height = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(
                commands, window_, &swapchain, &window_width, &window_height))
        {
            fail("Cannot present the frame");
        }
        if (!swapchain)
        { // minimized: nothing to show
            SDL_SubmitGPUCommandBuffer(commands);
            return;
        }
        const bool capture = !screenshot_.empty();
        SDL_GPUTexture* target =
            capture ? prepare_composite(window_width, window_height) : swapchain;

        // Black bars, then the frame scaled into place, then the overlay.
        SDL_GPUColorTargetInfo clear{};
        clear.texture = target;
        clear.clear_color = {0, 0, 0, 1};
        clear.load_op = SDL_GPU_LOADOP_CLEAR;
        clear.store_op = SDL_GPU_STOREOP_STORE;
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(commands, &clear, 1, nullptr));
        const SDL_Rect rect =
            presentation_transform(static_cast<int>(window_width), static_cast<int>(window_height))
                .destination;
        SDL_GPUBlitInfo blit{};
        blit.source = {frame_texture_, 0, 0, 0, 0, width, height};
        blit.destination = {target,
            0,
            0,
            static_cast<Uint32>(rect.x),
            static_cast<Uint32>(rect.y),
            static_cast<Uint32>(rect.w),
            static_cast<Uint32>(rect.h)};
        blit.load_op = SDL_GPU_LOADOP_LOAD;
        blit.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(commands, &blit);
        if (overlay)
        {
            SDL_GPUColorTargetInfo layer{};
            layer.texture = target;
            layer.load_op = SDL_GPU_LOADOP_LOAD;
            layer.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &layer, 1, nullptr);
            ImGui_ImplSDLGPU3_RenderDrawData(overlay, commands, pass);
            SDL_EndGPURenderPass(pass);
        }
        if (!capture)
        {
            if (!SDL_SubmitGPUCommandBuffer(commands))
            {
                fail("Cannot submit the frame");
            }
            return;
        }
        save_composite(commands, swapchain, window_width, window_height);
    }

  private:
    void prepare_frame(Uint32 width, Uint32 height)
    {
        if (frame_texture_ && frame_width_ == width && frame_height_ == height)
        {
            return;
        }
        SDL_WaitForGPUIdle(device_);
        if (frame_texture_)
        {
            SDL_ReleaseGPUTexture(device_, frame_texture_);
        }
        if (upload_)
        {
            SDL_ReleaseGPUTransferBuffer(device_, upload_);
        }
        SDL_GPUTextureCreateInfo texture{};
        texture.type = SDL_GPU_TEXTURETYPE_2D;
        texture.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        texture.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        texture.width = width;
        texture.height = height;
        texture.layer_count_or_depth = 1;
        texture.num_levels = 1;
        frame_texture_ = SDL_CreateGPUTexture(device_, &texture);
        const SDL_GPUTransferBufferCreateInfo buffer{
            SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, width * height * 4, 0};
        upload_ = SDL_CreateGPUTransferBuffer(device_, &buffer);
        if (!frame_texture_ || !upload_)
        {
            fail("Cannot create the frame texture");
        }
        frame_width_ = width;
        frame_height_ = height;
    }

    SDL_GPUTexture* prepare_composite(Uint32 width, Uint32 height)
    {
        if (composite_ && composite_width_ == width && composite_height_ == height)
        {
            return composite_;
        }
        SDL_WaitForGPUIdle(device_);
        if (composite_)
        {
            SDL_ReleaseGPUTexture(device_, composite_);
        }
        if (download_)
        {
            SDL_ReleaseGPUTransferBuffer(device_, download_);
        }
        SDL_GPUTextureCreateInfo texture{};
        texture.type = SDL_GPU_TEXTURETYPE_2D;
        texture.format = swapchain_format_;
        texture.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        texture.width = width;
        texture.height = height;
        texture.layer_count_or_depth = 1;
        texture.num_levels = 1;
        composite_ = SDL_CreateGPUTexture(device_, &texture);
        const SDL_GPUTransferBufferCreateInfo buffer{
            SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, width * height * 4, 0};
        download_ = SDL_CreateGPUTransferBuffer(device_, &buffer);
        if (!composite_ || !download_)
        {
            fail("Cannot prepare the screenshot");
        }
        composite_width_ = width;
        composite_height_ = height;
        return composite_;
    }

    // The picture was composed off screen: show it, and keep a copy as PNG.
    void save_composite(
        SDL_GPUCommandBuffer* commands, SDL_GPUTexture* swapchain, Uint32 width, Uint32 height)
    {
        SDL_GPUBlitInfo show{};
        show.source = {composite_, 0, 0, 0, 0, width, height};
        show.destination = {swapchain, 0, 0, 0, 0, width, height};
        show.load_op = SDL_GPU_LOADOP_DONT_CARE;
        show.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(commands, &show);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTextureRegion source{};
        source.texture = composite_;
        source.w = width;
        source.h = height;
        source.d = 1;
        const SDL_GPUTextureTransferInfo destination{download_, 0, width, height};
        SDL_DownloadFromGPUTexture(copy, &source, &destination);
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        if (!fence)
        {
            fail("Cannot submit the frame");
        }
        SDL_WaitForGPUFences(device_, true, &fence, 1);
        SDL_ReleaseGPUFence(device_, fence);
        const auto file = screenshot_;
        screenshot_.clear();
        SDL_PixelFormat format = SDL_PIXELFORMAT_UNKNOWN;
        if (swapchain_format_ == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM)
        {
            format = SDL_PIXELFORMAT_ARGB8888;
        }
        else if (swapchain_format_ == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)
        {
            format = SDL_PIXELFORMAT_ABGR8888;
        }
        else
        {
            throw std::runtime_error(
                "Screenshots of this display's pixel format are not supported");
        }
        auto* pixels = SDL_MapGPUTransferBuffer(device_, download_, false);
        if (!pixels)
        {
            fail("Cannot read the screenshot");
        }
        SDL_Surface* surface = SDL_CreateSurfaceFrom(static_cast<int>(width),
            static_cast<int>(height),
            format,
            pixels,
            static_cast<int>(width * 4));
        const bool saved = surface && SDL_SavePNG(surface, utf8(file).c_str());
        SDL_DestroySurface(surface);
        SDL_UnmapGPUTransferBuffer(device_, download_);
        if (!saved)
        {
            fail("Cannot save the screenshot");
        }
    }

    VideoSettings settings_;
    VideoInfo info_;
    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTextureFormat swapchain_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTexture* frame_texture_ = nullptr;
    SDL_GPUTexture* composite_ = nullptr;
    SDL_GPUTransferBuffer* upload_ = nullptr;
    SDL_GPUTransferBuffer* download_ = nullptr;
    Uint32 frame_width_ = 0, frame_height_ = 0, composite_width_ = 0, composite_height_ = 0;
    bool claimed_ = false, overlay_ready_ = false;
    std::filesystem::path screenshot_;
};
} // namespace

std::unique_ptr<Video> create_video(const VideoSettings& settings)
{
    if (settings.headless)
    {
        return create_headless_video(settings);
    }
    return std::make_unique<SdlGpuVideo>(settings);
}

} // namespace beachlife::platform
