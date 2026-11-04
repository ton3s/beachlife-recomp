#include "renderer_word_surface.h"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace beachlife::runtime::renderer_display
{
std::size_t WordSurface16::checked_word_count(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        throw std::invalid_argument("Word surface dimensions must be positive");
    }
    if (width > std::numeric_limits<int>::max() / 2)
    {
        throw std::overflow_error("Word surface pitch exceeds a signed pitch");
    }
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    if (w > std::numeric_limits<std::size_t>::max() / h / sizeof(std::uint16_t))
    {
        throw std::overflow_error("Word surface storage overflows");
    }
    if (w * h > std::numeric_limits<std::uint32_t>::max() / 4u)
    {
        throw std::overflow_error("Word surface exceeds the Video GPU transfer limit");
    }
    return w * h;
}

WordMasks word_masks(WordFormat format)
{
    switch (format)
    {
    case WordFormat::Rgb555:
        return {0x7c00, 0x03e0, 0x001f};
    case WordFormat::Rgb565:
        return {0xf800, 0x07e0, 0x001f};
    }
    throw std::invalid_argument("Unknown fullscreen word format");
}

platform::Frame convert_words(
    std::span<const std::uint16_t> pixels, int width, int height, WordFormat format)
{
    const auto count = WordSurface16::checked_word_count(width, height);
    if (pixels.size() != count)
    {
        throw std::invalid_argument("Fullscreen surface storage is incomplete");
    }
    (void)word_masks(format);
    platform::Frame result;
    result.width = width;
    result.height = height;
    result.pixels.resize(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        const std::uint32_t word = pixels[index];
        const std::uint32_t red = (word >> (format == WordFormat::Rgb565 ? 11 : 10)) & 31;
        const std::uint32_t blue = word & 31;
        const std::uint32_t green = (word >> 5) & (format == WordFormat::Rgb565 ? 63u : 31u);
        const std::uint32_t r8 = (red << 3) | (red >> 2);
        const std::uint32_t b8 = (blue << 3) | (blue >> 2);
        const std::uint32_t g8 = format == WordFormat::Rgb565 ? (green << 2) | (green >> 4)
                                                              : (green << 3) | (green >> 2);
        result.pixels[index] = r8 | (g8 << 8) | (b8 << 16) | 0xff000000u;
    }
    return result;
}

WordSurface16::WordSurface16(int width, int height, WordFormat format)
    : width_(width)
    , height_(height)
    , words_(checked_word_count(width, height))
    , format_(format)
    , thread_(std::this_thread::get_id())
{
    (void)word_masks(format);
    pixels_.reset(new std::uint16_t[words_]);
}

WordSurfaceDescription WordSurface16::description() const noexcept
{
    return {width_, height_, static_cast<std::size_t>(width_) * 2, format_};
}

void WordSurface16::validate_storage() const
{
    require_thread();
    if (!pixels_ || words_ != checked_word_count(width_, height_))
    {
        throw std::logic_error("Word surface owned storage is incomplete");
    }
    (void)word_masks(format_);
}

void WordSurface16::require_thread() const
{
    if (thread_ != std::this_thread::get_id())
    {
        throw std::logic_error("Word surface operation is on the wrong thread");
    }
}

void WordSurface16::require_unlocked() const
{
    require_thread();
    if (locked_)
    {
        throw std::logic_error("Word surface is locked");
    }
}

bool WordSurface16::locked() const
{
    require_thread();
    return locked_;
}

std::span<std::uint16_t> WordSurface16::lock()
{
    require_unlocked();
    locked_ = true;
    return {pixels_.get(), words_};
}

void WordSurface16::unlock()
{
    require_thread();
    if (!locked_)
    {
        throw std::logic_error("Word surface is not locked");
    }
    locked_ = false;
}

std::span<const std::uint16_t> WordSurface16::pixels() const
{
    require_unlocked();
    return {pixels_.get(), words_};
}

void WordSurface16::set_source_key(std::uint16_t key)
{
    require_unlocked();
    source_key_ = key;
}

namespace
{
bool source_inside(const WordSurface16& source, Rectangle rectangle)
{
    const auto description = source.description();
    return rectangle.left >= 0 && rectangle.top >= 0 && rectangle.right >= rectangle.left &&
           rectangle.bottom >= rectangle.top && rectangle.right <= description.width &&
           rectangle.bottom <= description.height;
}

bool nonempty_inside(Rectangle rectangle, int width, int height)
{
    return rectangle.left >= 0 && rectangle.top >= 0 && rectangle.right > rectangle.left &&
           rectangle.bottom > rectangle.top && rectangle.right <= width && rectangle.bottom <= height;
}
} // namespace

bool WordSurface16::copy_fits(
    const WordSurface16& source, Rectangle source_rectangle, int left, int top) const
{
    if (!source_inside(source, source_rectangle))
    {
        return false;
    }
    const int columns = source_rectangle.right - source_rectangle.left;
    const int rows = source_rectangle.bottom - source_rectangle.top;
    return columns <= width_ && rows <= height_ && left >= 0 && top >= 0 &&
           left <= width_ - columns && top <= height_ - rows;
}

bool WordSurface16::blit_fits(const WordSurface16& source,
    Rectangle source_rectangle,
    Rectangle destination_rectangle) const
{
    return nonempty_inside(source_rectangle, source.width_, source.height_) &&
           nonempty_inside(destination_rectangle, width_, height_);
}

void WordSurface16::copy_from(
    const WordSurface16& source, Rectangle source_rectangle, int left, int top, bool use_source_key)
{
    require_unlocked();
    source.require_unlocked();
    if (format_ != source.format_)
    {
        throw std::invalid_argument("Word surface copy formats differ");
    }
    if (!source_inside(source, source_rectangle))
    {
        throw std::out_of_range("Word surface source rectangle is outside storage");
    }
    const int columns = source_rectangle.right - source_rectangle.left;
    const int rows = source_rectangle.bottom - source_rectangle.top;
    if (!copy_fits(source, source_rectangle, left, top))
    {
        throw std::out_of_range("Word surface destination rectangle is outside storage");
    }
    if (use_source_key && !source.source_key_)
    {
        throw std::logic_error("Word surface source key has not been set");
    }
    if (columns == 0 || rows == 0)
    {
        return;
    }

    const bool bottom_first = this == &source && top > source_rectangle.top;
    const bool right_first = this == &source && left > source_rectangle.left;
    for (int step = 0; step < rows; ++step)
    {
        const int row = bottom_first ? rows - 1 - step : step;
        const auto source_offset =
            static_cast<std::size_t>(source_rectangle.top + row) * source.width_ +
            source_rectangle.left;
        const auto destination_offset = static_cast<std::size_t>(top + row) * width_ + left;
        const auto* input = source.pixels_.get() + source_offset;
        auto* output = pixels_.get() + destination_offset;
        if (!use_source_key)
        {
            std::memmove(output, input, static_cast<std::size_t>(columns) * sizeof(*output));
            continue;
        }
        for (int index = 0; index < columns; ++index)
        {
            const int column = right_first ? columns - 1 - index : index;
            const auto value = input[column];
            if (value != *source.source_key_)
            {
                output[column] = value;
            }
        }
    }
}

void WordSurface16::blit_from(const WordSurface16& source,
    Rectangle source_rectangle,
    Rectangle destination_rectangle,
    bool use_source_key)
{
    require_unlocked();
    source.require_unlocked();
    if (format_ != source.format_)
    {
        throw std::invalid_argument("Word surface blit formats differ");
    }
    if (!blit_fits(source, source_rectangle, destination_rectangle))
    {
        throw std::out_of_range("Word surface blit rectangles must be nonempty and contained");
    }
    if (use_source_key && !source.source_key_)
    {
        throw std::logic_error("Word surface source key has not been set");
    }
    const int source_width = source_rectangle.right - source_rectangle.left;
    const int source_height = source_rectangle.bottom - source_rectangle.top;
    const int destination_width = destination_rectangle.right - destination_rectangle.left;
    const int destination_height = destination_rectangle.bottom - destination_rectangle.top;
    if (source_width == destination_width && source_height == destination_height)
    {
        copy_from(source,
            source_rectangle,
            destination_rectangle.left,
            destination_rectangle.top,
            use_source_key);
        return;
    }

    // Directional memmove ordering cannot protect every stretched self-blit.
    // Snapshot just the input rectangle before any destination word is written.
    std::vector<std::uint16_t> snapshot;
    const auto* input = source.pixels_.get() +
                        static_cast<std::size_t>(source_rectangle.top) * source.width_ +
                        source_rectangle.left;
    int pitch = source.width_;
    if (this == &source)
    {
        snapshot.resize(static_cast<std::size_t>(source_width) * source_height);
        for (int y = 0; y < source_height; ++y)
        {
            std::memcpy(snapshot.data() + static_cast<std::size_t>(y) * source_width,
                input + static_cast<std::size_t>(y) * pitch,
                static_cast<std::size_t>(source_width) * sizeof(*input));
        }
        input = snapshot.data();
        pitch = source_width;
    }
    for (int y = 0; y < destination_height; ++y)
    {
        const auto source_y = static_cast<std::int64_t>(y) * source_height / destination_height;
        auto* output = pixels_.get() +
                       static_cast<std::size_t>(y + destination_rectangle.top) * width_ +
                       destination_rectangle.left;
        for (int x = 0; x < destination_width; ++x)
        {
            const auto source_x = static_cast<std::int64_t>(x) * source_width / destination_width;
            const auto value = input[source_y * pitch + source_x];
            if (!use_source_key || value != *source.source_key_)
            {
                output[x] = value;
            }
        }
    }
}

void WordSurface16::exchange_storage(WordSurface16& other)
{
    require_unlocked();
    other.require_unlocked();
    if (width_ != other.width_ || height_ != other.height_ || format_ != other.format_)
    {
        throw std::invalid_argument("Word surface exchange geometry or format differs");
    }
    pixels_.swap(other.pixels_);
}
} // namespace beachlife::runtime::renderer_display
