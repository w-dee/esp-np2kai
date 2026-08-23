#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "p4_nano_display/p4_nano_display_pattern.hpp"
#include "p4_nano_display/p4_nano_display_transform.hpp"
#include "p4_nano_display/p4_nano_display_transform_pattern.hpp"

int main()
{
    using namespace p4_nano_display;
    static_assert(kTransformSourceWidth == 640);
    static_assert(kTransformSourceHeight == 400);
    static_assert(kTransformSourcePixelCount == 256000);
    static_assert(kTransformSourceBytes == 512000);
    static_assert(kTransformDestinationWidth == 800);
    static_assert(kTransformDestinationHeight == 1280);
    static_assert(kTransformDestinationBytes == 2048000);

    std::vector<std::uint16_t> source(kTransformSourcePixelCount);
    assert(fill_transform_source_pattern(std::span<std::uint16_t>(source)));
    assert(!fill_transform_source_pattern(
        std::span<std::uint16_t>(source.data(), source.size() - 1U)));

    const auto source_at = [&](std::size_t x, std::size_t y) {
        return source[y * kTransformSourceWidth + x];
    };
    assert(source_at(0, 0) == 0xffe0U);
    assert(source_at(kTransformSourceWidth - 1U, 0) == 0xf81fU);
    assert(source_at(0, kTransformSourceHeight - 1U) == 0x07ffU);
    assert(source_at(kTransformSourceWidth - 1U,
                     kTransformSourceHeight - 1U) == 0xfd20U);
    assert(source_at(kTransformSourceWidth / 2U, 0) == 0xf800U);
    assert(source_at(kTransformSourceWidth - 1U,
                     kTransformSourceHeight / 2U) == 0x07e0U);
    assert(source_at(kTransformSourceWidth / 2U,
                     kTransformSourceHeight - 1U) == 0x001fU);
    assert(source_at(0, kTransformSourceHeight / 2U) == 0xffffU);
    assert(source_at(176, 124) == 0x05b8U);
    assert(source_at(176 + 12, 124 + 9) == 0xd81fU);
    assert(source_at(320, 200) == 0x0000U);

    const std::uint32_t source_crc = transform_source_pattern_crc32(source);
    assert(transform_source_pattern_crc32(
               std::span<const std::uint16_t>(source.data(),
                                               source.size() - 1U)) == 0U);

    std::vector<std::uint16_t> destination(kTransformDestinationPixelCount);
    assert(transform_to_native(source, destination, QuarterTurn::Clockwise));
    const std::uint32_t clockwise_crc = crc32(
        reinterpret_cast<const std::uint8_t *>(destination.data()),
        kTransformDestinationBytes);
    assert(transform_to_native(source, destination,
                               QuarterTurn::CounterClockwise));
    const std::uint32_t counter_clockwise_crc = crc32(
        reinterpret_cast<const std::uint8_t *>(destination.data()),
        kTransformDestinationBytes);

    std::printf("P4_NANO_TRANSFORM_SOURCE_PATTERN_CRC32=0x%08x\n",
                source_crc);
    std::printf("P4_NANO_TRANSFORM_DIAGNOSTIC_CW_CRC32=0x%08x\n",
                clockwise_crc);
    std::printf("P4_NANO_TRANSFORM_DIAGNOSTIC_CCW_CRC32=0x%08x\n",
                counter_clockwise_crc);
    std::fflush(stdout);

    assert(source_crc == 0x4291f7e5U);
    assert(clockwise_crc == 0x37fd7262U);
    assert(counter_clockwise_crc == 0xd98ce5d4U);
    return 0;
}
