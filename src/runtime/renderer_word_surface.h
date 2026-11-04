#pragma once

#include "runtime/renderer_software_display.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <thread>

namespace beachlife::runtime::renderer_display
{
// Logical game pixels, independent of the physical SDL GPU swapchain format.
enum class WordFormat
{
    Rgb555,
    Rgb565
};

struct WordMasks
{
    std::uint16_t red, green, blue;
};

WordMasks word_masks(WordFormat format);
platform::Frame convert_words(
    std::span<const std::uint16_t> pixels, int width, int height, WordFormat format);

struct WordSurfaceDescription
{
    int width, height;
    std::size_t pitch;
    WordFormat format;
};

class FlipChain16;

// Owns tightly packed, uninitialized 16-bit pixels. Each instance is confined
// to its construction thread and must be destroyed there after its borrows end.
// This is CPU storage, not a COM object or a video-memory allocation claim.
class WordSurface16
{
  public:
    WordSurface16(int width, int height, WordFormat format);
    ~WordSurface16() = default;
    WordSurface16(const WordSurface16&) = delete;
    WordSurface16& operator=(const WordSurface16&) = delete;

    // Also validates the signed pitch and the existing Video transfer limit,
    // before an allocation or a display-mode operation is attempted.
    static std::size_t checked_word_count(int width, int height);
    WordSurfaceDescription description() const noexcept;
    // Checks the owned allocation and its descriptor without reading pixels.
    // A valid lock is not a loss of storage. No borrow is ended by this query.
    void validate_storage() const;
    bool locked() const;
    std::span<std::uint16_t> lock();
    void unlock();

    // Unlock does not free or relocate pixels: the real rectangle-copy caller
    // reads its saved pointer after unlocking. Borrows expire at destruction or
    // a flip's storage exchange. No access may race or reenter another operation.
    // Callers may read only pixels they have initialized, including keyed holes.
    std::span<const std::uint16_t> pixels() const;
    void set_source_key(std::uint16_t key);

    // Unscaled, fully contained rectangles only. Copies preserve all 16 bits;
    // optional source-key comparison is exact, including RGB555's unused bit15.
    // Both surfaces must be unlocked and use the same logical format. A copy
    // within one surface has memmove ordering, including the keyed form.
    void copy_from(const WordSurface16& source,
        Rectangle source_rectangle,
        int left,
        int top,
        bool use_source_key = false);

    // DirectDraw Blt's point-sampled stretch between fully contained, nonempty
    // rectangles. Left/top are inclusive and right/bottom exclusive. Source
    // colour keys are compared before writing, with no filtering of word values.
    // Overlap reads the original source rectangle, including when scaling down.
    void blit_from(const WordSurface16& source,
        Rectangle source_rectangle,
        Rectangle destination_rectangle,
        bool use_source_key = false);

    // Whether copy_from or blit_from would accept these rectangles. DirectDraw answered a
    // rectangle outside the surface with DDERR_INVALIDRECT and drew nothing; callers that
    // stand in for it test first and skip the draw.
    bool copy_fits(const WordSurface16& source, Rectangle source_rectangle, int left, int top) const;
    bool blit_fits(const WordSurface16& source,
        Rectangle source_rectangle,
        Rectangle destination_rectangle) const;

  private:
    friend class FlipChain16;
    void require_thread() const;
    void require_unlocked() const;
    void exchange_storage(WordSurface16& other);

    int width_, height_;
    std::size_t words_;
    WordFormat format_;
    std::thread::id thread_;
    std::unique_ptr<std::uint16_t[]> pixels_;
    std::optional<std::uint16_t> source_key_;
    bool locked_ = false;
};
} // namespace beachlife::runtime::renderer_display
