#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "p4_nano_display/p4_nano_display_transform.hpp"

namespace {

using p4_nano_display::QuarterTurn;

constexpr std::uint16_t kGuard = 0xa55a;
constexpr std::size_t kGuardPixels = 17;

std::uint16_t source_pixel(std::size_t x, std::size_t y)
{
    const std::uint32_t red =
        ((x * 31U) / (p4_nano_display::kTransformSourceWidth - 1U) +
         (x * 7U + y * 13U)) &
        0x1fU;
    const std::uint32_t green =
        ((y * 63U) / (p4_nano_display::kTransformSourceHeight - 1U) +
         (x * 11U + y * 5U)) &
        0x3fU;
    const std::uint32_t blue =
        (((x * 31U) / (p4_nano_display::kTransformSourceWidth - 1U)) ^
         ((y * 31U) / (p4_nano_display::kTransformSourceHeight - 1U)) ^
         (x * 17U + y * 23U)) &
        0x1fU;
    return static_cast<std::uint16_t>((red << 11U) | (green << 5U) | blue);
}

struct Coordinate {
    std::size_t x;
    std::size_t y;
};

Coordinate destination_for(std::size_t source_x, std::size_t source_y,
                           std::size_t offset_x, std::size_t offset_y,
                           QuarterTurn rotation)
{
    const std::size_t logical_x = source_x * 2U + offset_x;
    const std::size_t logical_y = source_y * 2U + offset_y;
    if (rotation == QuarterTurn::Clockwise) {
        return {p4_nano_display::kTransformDestinationWidth - 1U - logical_y,
                logical_x};
    }
    return {logical_y,
            p4_nano_display::kTransformDestinationHeight - 1U - logical_x};
}

Coordinate source_for_destination(std::size_t destination_x,
                                  std::size_t destination_y,
                                  QuarterTurn rotation)
{
    if (rotation == QuarterTurn::Clockwise) {
        return {destination_y / 2U, (p4_nano_display::kTransformDestinationWidth -
                                     1U - destination_x) /
                                        2U};
    }
    return {(p4_nano_display::kTransformDestinationHeight - 1U -
             destination_y) /
                2U,
            destination_x / 2U};
}

std::uint32_t crc32_bytes(std::span<const std::uint8_t> bytes)
{
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return crc ^ 0xffffffffU;
}

void assert_edge_and_interior_points(
    std::span<const std::uint16_t> source,
    std::span<const std::uint16_t> destination, QuarterTurn rotation)
{
    const Coordinate points[] = {
        {0, 0},
        {p4_nano_display::kTransformDestinationWidth - 1U, 0},
        {0, p4_nano_display::kTransformDestinationHeight - 1U},
        {p4_nano_display::kTransformDestinationWidth - 1U,
         p4_nano_display::kTransformDestinationHeight - 1U},
        {p4_nano_display::kTransformDestinationWidth / 2U, 0},
        {p4_nano_display::kTransformDestinationWidth - 1U,
         p4_nano_display::kTransformDestinationHeight / 2U},
        {p4_nano_display::kTransformDestinationWidth / 2U,
         p4_nano_display::kTransformDestinationHeight - 1U},
        {0, p4_nano_display::kTransformDestinationHeight / 2U},
        {123, 456},
        {512, 777},
        {701, 1111},
    };

    for (const Coordinate point : points) {
        const Coordinate source_point =
            source_for_destination(point.x, point.y, rotation);
        assert(source_point.x < p4_nano_display::kTransformSourceWidth);
        assert(source_point.y < p4_nano_display::kTransformSourceHeight);
        assert(destination[point.y * p4_nano_display::kTransformDestinationWidth +
                           point.x] ==
               source[source_point.y * p4_nano_display::kTransformSourceWidth +
                      source_point.x]);
    }
}

std::uint32_t run_transform(std::span<const std::uint16_t> source,
                            QuarterTurn rotation)
{
    std::vector<std::uint16_t> guarded_destination(
        p4_nano_display::kTransformDestinationPixelCount + 2U * kGuardPixels,
        kGuard);
    const std::span<std::uint16_t> destination(
        guarded_destination.data() + kGuardPixels,
        p4_nano_display::kTransformDestinationPixelCount);
    assert(p4_nano_display::transform_to_native(source, destination, rotation));

    for (std::size_t index = 0; index < kGuardPixels; ++index) {
        assert(guarded_destination[index] == kGuard);
        assert(guarded_destination[guarded_destination.size() - 1U - index] ==
               kGuard);
    }

    std::vector<std::uint8_t> ownership(
        p4_nano_display::kTransformDestinationPixelCount, 0U);
    for (std::size_t source_y = 0;
         source_y < p4_nano_display::kTransformSourceHeight; ++source_y) {
        for (std::size_t source_x = 0;
             source_x < p4_nano_display::kTransformSourceWidth; ++source_x) {
            const std::uint16_t expected =
                source[source_y * p4_nano_display::kTransformSourceWidth +
                       source_x];
            for (std::size_t offset_y = 0; offset_y < 2U; ++offset_y) {
                for (std::size_t offset_x = 0; offset_x < 2U; ++offset_x) {
                    const Coordinate destination_point = destination_for(
                        source_x, source_y, offset_x, offset_y, rotation);
                    assert(destination_point.x <
                           p4_nano_display::kTransformDestinationWidth);
                    assert(destination_point.y <
                           p4_nano_display::kTransformDestinationHeight);
                    const std::size_t destination_index =
                        destination_point.y *
                            p4_nano_display::kTransformDestinationWidth +
                        destination_point.x;
                    assert(++ownership[destination_index] == 1U);
                    assert(destination[destination_index] == expected);
                }
            }
        }
    }

    for (const std::uint8_t count : ownership) {
        assert(count == 1U);
    }

    for (std::size_t destination_y = 0;
         destination_y < p4_nano_display::kTransformDestinationHeight;
         ++destination_y) {
        for (std::size_t destination_x = 0;
             destination_x < p4_nano_display::kTransformDestinationWidth;
             ++destination_x) {
            const Coordinate source_point = source_for_destination(
                destination_x, destination_y, rotation);
            const std::uint16_t expected =
                source[source_point.y * p4_nano_display::kTransformSourceWidth +
                       source_point.x];
            assert(destination[destination_y *
                                  p4_nano_display::kTransformDestinationWidth +
                              destination_x] == expected);
        }
    }

    assert_edge_and_interior_points(source, destination, rotation);
    return crc32_bytes(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(destination.data()),
        p4_nano_display::kTransformDestinationBytes));
}

void assert_invalid_sizes(std::span<const std::uint16_t> source,
                          QuarterTurn rotation)
{
    std::vector<std::uint16_t> destination(
        p4_nano_display::kTransformDestinationPixelCount, kGuard);
    std::vector<std::uint16_t> short_source(
        p4_nano_display::kTransformSourcePixelCount - 1U, 0U);
    std::vector<std::uint16_t> short_destination(
        p4_nano_display::kTransformDestinationPixelCount - 1U, kGuard);

    assert(!p4_nano_display::transform_to_native(
        std::span<const std::uint16_t>(short_source),
        std::span<std::uint16_t>(destination), rotation));
    assert(!p4_nano_display::transform_to_native(
        source, std::span<std::uint16_t>(short_destination), rotation));
    assert(!p4_nano_display::transform_to_native(
        std::span<const std::uint16_t>(), std::span<std::uint16_t>(destination),
        rotation));
}

} // namespace

int main()
{
    using namespace p4_nano_display;
    static_assert(kTransformSourceWidth == 640);
    static_assert(kTransformSourceHeight == 400);
    static_assert(kTransformSourcePixelCount == 256000);
    static_assert(kTransformDestinationWidth == 800);
    static_assert(kTransformDestinationHeight == 1280);
    static_assert(kTransformDestinationPixelCount == 1024000);
    static_assert(kTransformDestinationBytes == 2048000);

    std::vector<std::uint16_t> source(kTransformSourcePixelCount);
    for (std::size_t y = 0; y < kTransformSourceHeight; ++y) {
        for (std::size_t x = 0; x < kTransformSourceWidth; ++x) {
            source[y * kTransformSourceWidth + x] = source_pixel(x, y);
        }
    }

    const std::uint16_t corners[] = {
        source_pixel(0, 0),
        source_pixel(kTransformSourceWidth - 1U, 0),
        source_pixel(0, kTransformSourceHeight - 1U),
        source_pixel(kTransformSourceWidth - 1U,
                     kTransformSourceHeight - 1U),
    };
    for (std::size_t first = 0; first < 4U; ++first) {
        for (std::size_t second = first + 1U; second < 4U; ++second) {
            assert(corners[first] != corners[second]);
        }
    }

    const std::span<const std::uint16_t> source_view(source);
    assert_invalid_sizes(source_view, QuarterTurn::Clockwise);

    const std::uint32_t clockwise_crc =
        run_transform(source_view, QuarterTurn::Clockwise);
    const std::uint32_t counter_clockwise_crc =
        run_transform(source_view, QuarterTurn::CounterClockwise);
    std::printf("P4_NANO_TRANSFORM_CW_CRC32=0x%08x\n", clockwise_crc);
    std::printf("P4_NANO_TRANSFORM_CCW_CRC32=0x%08x\n",
                counter_clockwise_crc);
    std::fflush(stdout);

    assert(clockwise_crc == 0xdb938d53U);
    assert(counter_clockwise_crc == 0x164584cfU);
    return 0;
}
