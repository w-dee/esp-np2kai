/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_display/p4_nano_display_transform.hpp"

#include <bit>
#include <cstring>

namespace p4_nano_display {

namespace {

constexpr std::size_t kTileWidth = 32;
constexpr std::size_t kTileHeight = 16;
constexpr std::size_t kSourceRowStridePixels = kTransformSourceWidth;
constexpr std::size_t kDestinationRowStridePixels = kTransformDestinationWidth;

static_assert(std::endian::native == std::endian::little,
              "RGB565 packed stores require a little-endian target");
static_assert(kTransformSourceWidth % kTileWidth == 0U);
static_assert(kTransformSourceHeight % kTileHeight == 0U);

bool destination_supports_packed_store(
    std::span<std::uint16_t> destination) noexcept
{
    return (reinterpret_cast<std::uintptr_t>(destination.data()) &
            (alignof(std::uint32_t) - 1U)) == 0U;
}

inline void store_packed_pair(std::uint16_t *destination,
                              std::uint32_t packed) noexcept
{
    void *aligned_destination = __builtin_assume_aligned(
        destination, alignof(std::uint32_t));
    std::memcpy(aligned_destination, &packed, sizeof(packed));
}

bool transform_ccw_tiled_32x16(
    std::span<const std::uint16_t> source,
    std::span<std::uint16_t> destination) noexcept
{
    for (std::size_t tile_x = 0; tile_x < kTransformSourceWidth;
         tile_x += kTileWidth) {
        for (std::size_t tile_y = 0; tile_y < kTransformSourceHeight;
             tile_y += kTileHeight) {
            for (std::size_t local_x = 0; local_x < kTileWidth; ++local_x) {
                const std::size_t source_x = tile_x + local_x;
                const std::uint16_t *source_ptr =
                    source.data() + tile_y * kSourceRowStridePixels + source_x;
                std::uint16_t *row0 =
                    destination.data() +
                    (kTransformDestinationHeight - 1U - 2U * source_x) *
                        kDestinationRowStridePixels +
                    2U * tile_y;
                std::uint16_t *row1 =
                    destination.data() +
                    (kTransformDestinationHeight - 2U - 2U * source_x) *
                        kDestinationRowStridePixels +
                    2U * tile_y;

                for (std::size_t local_y = 0; local_y < kTileHeight;
                     ++local_y) {
                    const std::uint16_t pixel = *source_ptr;
                    const std::uint32_t packed =
                        static_cast<std::uint32_t>(pixel) |
                        (static_cast<std::uint32_t>(pixel) << 16U);
                    store_packed_pair(row0, packed);
                    store_packed_pair(row1, packed);
                    source_ptr += kSourceRowStridePixels;
                    row0 += 2U;
                    row1 += 2U;
                }
            }
        }
    }
    return true;
}

bool transform_reference(std::span<const std::uint16_t> source,
                         std::span<std::uint16_t> destination,
                         QuarterTurn rotation)
{
    for (std::size_t source_y = 0; source_y < kTransformSourceHeight;
         ++source_y) {
        for (std::size_t source_x = 0; source_x < kTransformSourceWidth;
             ++source_x) {
            const std::uint16_t pixel =
                source[source_y * kTransformSourceWidth + source_x];
            const std::size_t logical_x = source_x * 2;
            const std::size_t logical_y = source_y * 2;

            for (std::size_t offset_y = 0; offset_y < 2; ++offset_y) {
                for (std::size_t offset_x = 0; offset_x < 2; ++offset_x) {
                    const std::size_t lx = logical_x + offset_x;
                    const std::size_t ly = logical_y + offset_y;
                    std::size_t destination_x;
                    std::size_t destination_y;

                    if (rotation == QuarterTurn::Clockwise) {
                        destination_x = kTransformDestinationWidth - 1 - ly;
                        destination_y = lx;
                    } else {
                        destination_x = ly;
                        destination_y = kTransformDestinationHeight - 1 - lx;
                    }

                    destination[destination_y * kTransformDestinationWidth +
                                destination_x] = pixel;
                }
            }
        }
    }
    return true;
}

} // namespace

bool transform_to_native(std::span<const std::uint16_t> source,
                         std::span<std::uint16_t> destination,
                         QuarterTurn rotation)
{
    if (source.size() != kTransformSourcePixelCount ||
        destination.size() != kTransformDestinationPixelCount ||
        (rotation != QuarterTurn::Clockwise &&
         rotation != QuarterTurn::CounterClockwise)) {
        return false;
    }

    if (rotation == QuarterTurn::CounterClockwise &&
        destination_supports_packed_store(destination)) {
        return transform_ccw_tiled_32x16(source, destination);
    }
    return transform_reference(source, destination, rotation);
}

} // namespace p4_nano_display
