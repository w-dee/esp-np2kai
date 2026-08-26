/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_display/p4_nano_display_exact2x.hpp"

#include <cstdint>

namespace p4_nano_display {

bool exact2x_scalar(const std::uint16_t *source, std::size_t source_bytes,
                   std::uint16_t *destination,
                   std::size_t destination_bytes) noexcept
{
    if (source == nullptr || destination == nullptr ||
        source_bytes != kExact2xSourceBytes ||
        destination_bytes != kExact2xDestinationBytes ||
        (reinterpret_cast<std::uintptr_t>(destination) &
         (kExact2xRequiredAlignmentBytes - 1U)) != 0U) {
        return false;
    }

    constexpr std::size_t source_stride = kExact2xSourceWidth;
    constexpr std::size_t destination_stride = kExact2xDestinationWidth;
    for (std::size_t y = 0U; y < kExact2xSourceHeight; ++y) {
        const std::uint16_t *source_row = source + y * source_stride;
        std::uint16_t *destination_row0 =
            destination + (2U * y) * destination_stride;
        std::uint16_t *destination_row1 = destination_row0 + destination_stride;
        auto *destination_pairs0 =
            reinterpret_cast<std::uint32_t *>(destination_row0);
        auto *destination_pairs1 =
            reinterpret_cast<std::uint32_t *>(destination_row1);
        for (std::size_t x = 0U; x < kExact2xSourceWidth; ++x) {
            const std::uint32_t pixel = source_row[x];
            const std::uint32_t packed = pixel | (pixel << 16U);
            destination_pairs0[x] = packed;
            destination_pairs1[x] = packed;
        }
    }
    return true;
}

} // namespace p4_nano_display
