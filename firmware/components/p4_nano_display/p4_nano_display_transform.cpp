/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_display/p4_nano_display_transform.hpp"

namespace p4_nano_display {

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

} // namespace p4_nano_display
