/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace p4_nano_ppa_rotation {

constexpr std::uint32_t kSourceWidth = 640U;
constexpr std::uint32_t kSourceHeight = 400U;
constexpr std::uint32_t kOutputWidth = 400U;
constexpr std::uint32_t kOutputHeight = 640U;
constexpr std::size_t kSourceBytes =
    static_cast<std::size_t>(kSourceWidth) * kSourceHeight * sizeof(std::uint16_t);
constexpr std::size_t kOutputBytes =
    static_cast<std::size_t>(kOutputWidth) * kOutputHeight * sizeof(std::uint16_t);
constexpr std::size_t kSourcePitchBytes =
    static_cast<std::size_t>(kSourceWidth) * sizeof(std::uint16_t);
constexpr std::size_t kOutputPitchBytes =
    static_cast<std::size_t>(kOutputWidth) * sizeof(std::uint16_t);
constexpr std::size_t kRequiredAlignmentBytes = 64U;
constexpr std::uint32_t kWarmupSamples = 8U;
constexpr std::uint32_t kMeasuredSamples = 128U;
constexpr std::uint32_t kFinalValidationSamples = 1U;

/* Derived from the existing np2video-7b2d-live-vram scene-2 fixture. */
constexpr std::uint32_t kExpectedSourceCrc = 0x8dadbf82U;
constexpr std::uint32_t kExpectedOutputCrc = 0x379511d7U;

constexpr std::size_t source_index(std::uint32_t sx, std::uint32_t sy) noexcept
{
    return static_cast<std::size_t>(sy) * kSourceWidth + sx;
}

constexpr std::size_t output_index(std::uint32_t dx, std::uint32_t dy) noexcept
{
    return static_cast<std::size_t>(dy) * kOutputWidth + dx;
}

constexpr void map_source_to_output(std::uint32_t sx, std::uint32_t sy,
                                     std::uint32_t *dx,
                                     std::uint32_t *dy) noexcept
{
    *dx = sy;
    *dy = (kSourceWidth - 1U) - sx;
}

inline bool reference_matches(const std::uint16_t *source,
                              const std::uint16_t *output) noexcept
{
    if (source == nullptr || output == nullptr) {
        return false;
    }
    for (std::uint32_t sy = 0U; sy < kSourceHeight; ++sy) {
        for (std::uint32_t sx = 0U; sx < kSourceWidth; ++sx) {
            std::uint32_t dx = 0U;
            std::uint32_t dy = 0U;
            map_source_to_output(sx, sy, &dx, &dy);
            if (output[output_index(dx, dy)] !=
                source[source_index(sx, sy)]) {
                return false;
            }
        }
    }
    return true;
}

} // namespace p4_nano_ppa_rotation
