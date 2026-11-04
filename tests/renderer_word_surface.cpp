#include "runtime/renderer_word_surface.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
using namespace beachlife::runtime::renderer_display;
unsigned checks = 0;

void require(bool value, const char* message)
{
    ++checks;
    if (!value)
    {
        throw std::runtime_error(message);
    }
}

template <class Exception, class Operation> void rejects(Operation operation)
{
    try
    {
        operation();
    }
    catch (const Exception&)
    {
        ++checks;
        return;
    }
    throw std::runtime_error("Invalid surface operation was accepted");
}

void assign(WordSurface16& surface, std::initializer_list<std::uint16_t> values)
{
    auto pixels = surface.lock();
    require(pixels.size() == values.size(), "Fixture extent differs");
    std::copy(values.begin(), values.end(), pixels.begin());
    surface.unlock();
}

void equal(
    const WordSurface16& surface, std::span<const std::uint16_t> expected, const char* message)
{
    const auto pixels = surface.pixels();
    require(pixels.size() == expected.size(), "Surface extent differs");
    for (std::size_t index = 0; index < pixels.size(); ++index)
    {
        require(pixels[index] == expected[index], message);
    }
}

void copies()
{
    WordSurface16 source(4, 3, WordFormat::Rgb565);
    WordSurface16 destination(6, 4, WordFormat::Rgb565);
    assign(source,
        {0, 0x07e0, 0xf800, 0x001f, 0xffff, 1, 0x07e0, 0x8000, 0x0400, 0x001f, 0x07e0, 0xf800});
    auto pixels = destination.lock();
    std::fill(pixels.begin(), pixels.end(), 0x1234);
    destination.unlock();
    require(source.description().pitch == 8 && destination.description().pitch == 12,
        "Word surface pitch differs");
    source.set_source_key(0x07e0);
    destination.copy_from(source, {1, 0, 4, 2}, 2, 1, true);
    std::array<std::uint16_t, 24> expected = {0x1234,
        0x1234,
        0x1234,
        0x1234,
        0x1234,
        0x1234,
        0x1234,
        0x1234,
        0x1234,
        0xf800,
        0x001f,
        0x1234,
        0x1234,
        0x1234,
        0x0001,
        0x1234,
        0x8000,
        0x1234,
        0x1234,
        0x1234,
        0x1234,
        0x1234,
        0x1234,
        0x1234};
    equal(destination, expected, "Source color key overwrote a destination word");
    destination.copy_from(source, {1, 0, 4, 2}, 2, 1);
    expected[8] = expected[15] = 0x07e0;
    equal(destination, expected, "Plain copy unexpectedly used the source key");

    WordSurface16 horizontal(5, 1, WordFormat::Rgb565);
    assign(horizontal, {1, 0, 9, 3, 4});
    horizontal.set_source_key(0);
    horizontal.copy_from(horizontal, {0, 0, 4, 1}, 1, 0, true);
    equal(horizontal,
        std::array<std::uint16_t, 5>{1, 1, 9, 9, 3},
        "Overlapping keyed horizontal copy differs");
    WordSurface16 vertical(2, 3, WordFormat::Rgb565);
    assign(vertical, {1, 0, 3, 4, 5, 6});
    vertical.set_source_key(0);
    vertical.copy_from(vertical, {0, 0, 2, 2}, 0, 1, true);
    equal(vertical,
        std::array<std::uint16_t, 6>{1, 0, 1, 4, 3, 4},
        "Overlapping keyed vertical copy differs");

    WordSurface16 high_bit(2, 1, WordFormat::Rgb555);
    WordSurface16 target555(2, 1, WordFormat::Rgb555);
    assign(high_bit, {0, 0x8000});
    assign(target555, {0x1234, 0x1234});
    high_bit.set_source_key(0);
    target555.copy_from(high_bit, {0, 0, 2, 1}, 0, 0, true);
    equal(target555,
        std::array<std::uint16_t, 2>{0x1234, 0x8000},
        "RGB555 key comparison discarded bit15");
}

void stretches()
{
    WordSurface16 source(4, 4, WordFormat::Rgb565);
    WordSurface16 destination(7, 5, WordFormat::Rgb565);
    assign(source, {99, 99, 99, 99, 99, 10, 0, 12, 99, 20, 21, 22, 99, 99, 99, 99});
    auto pixels = destination.lock();
    std::fill(pixels.begin(), pixels.end(), 7);
    destination.unlock();
    source.set_source_key(0);
    destination.blit_from(source, {1, 1, 4, 3}, {1, 1, 6, 4}, true);
    // 3x2 -> 5x3, with nonzero rectangle origins and keyed holes. No blending
    // or colour conversion is permitted, and words outside the target survive.
    equal(destination,
        std::array<std::uint16_t, 35>{7,
            7,
            7,
            7,
            7,
            7,
            7,
            7,
            10,
            10,
            7,
            7,
            12,
            7,
            7,
            10,
            10,
            7,
            7,
            12,
            7,
            7,
            20,
            20,
            21,
            21,
            22,
            7,
            7,
            7,
            7,
            7,
            7,
            7,
            7},
        "Noninteger keyed stretch sampled the wrong source word");

    WordSurface16 large(5, 3, WordFormat::Rgb565);
    WordSurface16 small(3, 2, WordFormat::Rgb565);
    assign(large, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
    large.set_source_key(1);
    small.blit_from(large, {0, 0, 5, 3}, {0, 0, 3, 2});
    equal(small,
        std::array<std::uint16_t, 6>{1, 2, 4, 6, 7, 9},
        "Shrinking must use point samples and ignore an unrequested key");

    WordSurface16 overlap(6, 2, WordFormat::Rgb565);
    assign(overlap, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
    overlap.blit_from(overlap, {1, 0, 4, 1}, {0, 0, 6, 2});
    equal(overlap,
        std::array<std::uint16_t, 12>{2, 2, 3, 3, 4, 4, 2, 2, 3, 3, 4, 4},
        "Overlapping stretch must read the original source rectangle");
    overlap.blit_from(overlap, {0, 0, 6, 2}, {1, 1, 4, 2});
    equal(overlap,
        std::array<std::uint16_t, 12>{2, 2, 3, 3, 4, 4, 2, 2, 3, 4, 4, 4},
        "Overlapping shrink must read the original source rectangle");

    WordSurface16 high_bit(2, 1, WordFormat::Rgb555);
    WordSurface16 target555(4, 1, WordFormat::Rgb555);
    assign(high_bit, {0, 0x8000});
    assign(target555, {0x1234, 0x1234, 0x1234, 0x1234});
    high_bit.set_source_key(0);
    target555.blit_from(high_bit, {0, 0, 2, 1}, {0, 0, 4, 1}, true);
    equal(target555,
        std::array<std::uint16_t, 4>{0x1234, 0x1234, 0x8000, 0x8000},
        "RGB555 stretched key comparison must retain bit15");

    // The pointer at the last column: Beach Life asks for an empty rectangle there.
    require(!small.blit_fits(large, {0, 0, 0, 2}, {2, 0, 2, 2}), "An empty blit must not fit");
    require(!small.blit_fits(large, {0, 0, 2, 2}, {2, 0, 4, 2}), "A blit past the edge must not fit");
    require(!small.blit_fits(large, {0, 0, 2, 2}, {-1, 0, 1, 2}), "A blit before the edge must not fit");
    require(small.blit_fits(large, {0, 0, 5, 3}, {0, 0, 3, 2}), "A contained blit must fit");
    require(!small.copy_fits(large, {0, 0, 2, 2}, 2, 0), "A copy past the edge must not fit");
    require(!small.copy_fits(large, {0, 0, 2, 2}, 0, -1), "A copy above the edge must not fit");
    require(!small.copy_fits(large, {0, 0, 6, 2}, 0, 0), "A copy from outside the source must not fit");
    require(small.copy_fits(large, {1, 1, 3, 3}, 1, 0), "A contained copy must fit");
    rejects<std::out_of_range>(
        [&]
        {
            small.blit_from(large, {0, 0, 0, 2}, {2, 0, 2, 2});
        });

    small.lock();
    rejects<std::logic_error>(
        [&]
        {
            small.blit_from(large, {0, 0, 5, 3}, {0, 0, 3, 2});
        });
    small.unlock();
    large.lock();
    rejects<std::logic_error>(
        [&]
        {
            small.blit_from(large, {0, 0, 5, 3}, {0, 0, 3, 2});
        });
    large.unlock();
    rejects<std::logic_error>(
        [&]
        {
            small.blit_from(overlap, {0, 0, 6, 2}, {0, 0, 3, 2}, true);
        });
    rejects<std::invalid_argument>(
        [&]
        {
            small.blit_from(high_bit, {0, 0, 2, 1}, {0, 0, 3, 2});
        });
    rejects<std::out_of_range>(
        [&]
        {
            small.blit_from(large, {-1, 0, 5, 3}, {0, 0, 3, 2});
        });
    rejects<std::out_of_range>(
        [&]
        {
            small.blit_from(large, {0, 0, 6, 3}, {0, 0, 3, 2});
        });
    rejects<std::out_of_range>(
        [&]
        {
            small.blit_from(large, {0, 0, 5, 3}, {-1, 0, 3, 2});
        });
    rejects<std::out_of_range>(
        [&]
        {
            small.blit_from(large, {0, 0, 5, 3}, {0, 0, 4, 2});
        });
    rejects<std::out_of_range>(
        [&]
        {
            small.blit_from(large, {0, 0, 0, 3}, {0, 0, 3, 2});
        });
    rejects<std::out_of_range>(
        [&]
        {
            small.blit_from(large, {0, 0, 5, 3}, {3, 0, 0, 2});
        });
    equal(small,
        std::array<std::uint16_t, 6>{1, 2, 4, 6, 7, 9},
        "Rejected stretch modified destination storage");
}

void boundaries()
{
    rejects<std::invalid_argument>(
        []
        {
            WordSurface16 surface(0, 1, WordFormat::Rgb565);
        });
    rejects<std::invalid_argument>(
        []
        {
            WordSurface16 surface(2, -1, WordFormat::Rgb565);
        });
    rejects<std::overflow_error>(
        []
        {
            WordSurface16::checked_word_count(INT_MAX, 1);
        });
    rejects<std::overflow_error>(
        []
        {
            WordSurface16::checked_word_count(65536, 65536);
        });
    rejects<std::invalid_argument>(
        []
        {
            WordSurface16 surface(2, 1, static_cast<WordFormat>(9));
        });
    WordSurface16 source(2, 1, WordFormat::Rgb565);
    WordSurface16 destination(2, 1, WordFormat::Rgb565);
    assign(source, {7, 8});
    assign(destination, {9, 10});
    auto borrowed = source.lock();
    rejects<std::logic_error>(
        [&]
        {
            source.lock();
        });
    rejects<std::logic_error>(
        [&]
        {
            destination.copy_from(source, {0, 0, 2, 1}, 0, 0);
        });
    source.unlock();
    destination.lock();
    require(borrowed[0] == 7 && borrowed[1] == 8, "Post-unlock borrow was invalidated");
    rejects<std::logic_error>(
        [&]
        {
            destination.copy_from(source, {0, 0, 2, 1}, 0, 0);
        });
    destination.unlock();
    rejects<std::logic_error>(
        [&]
        {
            destination.unlock();
        });
    rejects<std::logic_error>(
        [&]
        {
            destination.copy_from(source, {0, 0, 2, 1}, 0, 0, true);
        });
    rejects<std::out_of_range>(
        [&]
        {
            destination.copy_from(source, {-1, 0, 1, 1}, 0, 0);
        });
    rejects<std::out_of_range>(
        [&]
        {
            destination.copy_from(source, {0, 0, 3, 1}, 0, 0);
        });
    rejects<std::out_of_range>(
        [&]
        {
            destination.copy_from(source, {0, 0, 2, 1}, 1, 0);
        });
    WordSurface16 other_format(2, 1, WordFormat::Rgb555);
    assign(other_format, {0, 0});
    rejects<std::invalid_argument>(
        [&]
        {
            destination.copy_from(other_format, {0, 0, 2, 1}, 0, 0);
        });
    destination.copy_from(source, {2, 0, 2, 1}, 2, 0);
    equal(
        destination, std::array<std::uint16_t, 2>{9, 10}, "Rejected or empty copy changed pixels");
    bool wrong_thread_rejected = false;
    std::thread wrong_thread(
        [&]
        {
            try
            {
                source.lock();
            }
            catch (const std::logic_error&)
            {
                wrong_thread_rejected = true;
            }
        });
    wrong_thread.join();
    require(wrong_thread_rejected && !source.locked(), "Wrong-thread access changed lock state");
    {
        WordSurface16 locked_cleanup(2, 1, WordFormat::Rgb565);
        locked_cleanup.lock(); // The discarded borrow ends before real destruction.
    }
}

void conversion()
{
    const auto mask555 = word_masks(WordFormat::Rgb555);
    const auto mask565 = word_masks(WordFormat::Rgb565);
    require(mask555.red == 0x7c00 && mask555.green == 0x03e0 && mask555.blue == 0x1f &&
                mask565.red == 0xf800 && mask565.green == 0x07e0 && mask565.blue == 0x1f,
        "Logical word masks differ");
    const std::array<std::uint16_t, 5> pixels565{0xf800, 0x07e0, 0x001f, 0xffff, 0x0400};
    const auto frame565 = convert_words(pixels565, 5, 1, WordFormat::Rgb565);
    require(frame565.width == 5 && frame565.height == 1 &&
                frame565.pixels ==
                    std::vector<std::uint32_t>{
                        0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff, 0xff008200},
        "RGB565 transport conversion differs");
    const std::array<std::uint16_t, 5> pixels555{0x7c00, 0x03e0, 0x001f, 0xffff, 0x8000};
    require(
        convert_words(pixels555, 5, 1, WordFormat::Rgb555).pixels ==
            std::vector<std::uint32_t>{0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff, 0xff000000},
        "RGB555 transport conversion differs");
    rejects<std::invalid_argument>(
        [&]
        {
            convert_words(pixels565, 4, 1, WordFormat::Rgb565);
        });
}
} // namespace

int main()
{
    try
    {
        copies();
        stretches();
        boundaries();
        conversion();
        std::printf("{\"groups\":4,\"checks\":%u}\n", checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "word-surface: %s\n", error.what());
        return 2;
    }
}
