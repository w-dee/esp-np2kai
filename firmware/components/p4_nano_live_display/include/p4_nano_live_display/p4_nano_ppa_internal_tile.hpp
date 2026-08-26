/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace p4_nano_ppa_internal_tile {

constexpr std::uint32_t kSourceWidth = 640U;
constexpr std::uint32_t kSourceHeight = 400U;
constexpr std::uint32_t kOutputWidth = 400U;
constexpr std::uint32_t kOutputHeight = 640U;
constexpr std::size_t kSourceBytes =
    static_cast<std::size_t>(kSourceWidth) * kSourceHeight * sizeof(std::uint16_t);
constexpr std::size_t kOutputBytes =
    static_cast<std::size_t>(kOutputWidth) * kOutputHeight * sizeof(std::uint16_t);
constexpr std::size_t kLargestTileWidth = 128U;
constexpr std::size_t kLargestTileBytes =
    static_cast<std::size_t>(kOutputWidth) * kLargestTileWidth *
    sizeof(std::uint16_t);
constexpr std::size_t kRequiredAlignmentBytes = 64U;
constexpr std::uint32_t kWarmupSamples = 8U;
constexpr std::uint32_t kMeasuredSamples = 128U;
constexpr std::uint32_t kFinalValidationSamples = 1U;
constexpr std::uint32_t kTileWidths[] = {32U, 64U, 128U};
constexpr std::uint32_t kExpectedSourceCrc = 0x8dadbf82U;
constexpr std::uint32_t kExpectedOutputCrc = 0x379511d7U;

constexpr std::size_t tile_bytes(std::uint32_t tile_width) noexcept
{
    return static_cast<std::size_t>(kOutputWidth) * tile_width *
           sizeof(std::uint16_t);
}

constexpr std::uint32_t tile_count(std::uint32_t tile_width) noexcept
{
    return kSourceWidth / tile_width;
}

struct Input final {
    const std::uint8_t *source = nullptr;
    std::size_t source_bytes = 0U;
    std::uint32_t source_width = 0U;
    std::uint32_t source_height = 0U;
    std::size_t source_pitch_bytes = 0U;
    std::uint32_t source_bpp = 0U;
    const void *presentation_slot0 = nullptr;
    const void *presentation_slot1 = nullptr;
    std::size_t presentation_slot_bytes = 0U;
    const void *native_framebuffer = nullptr;
    std::size_t native_framebuffer_bytes = 0U;
};

/* Runs the P10E-B benchmark-only tiled PPA service measurement.  The caller
 * must retain the isolated source lease and producer pause while this runs. */
esp_err_t run(const Input &input);

} // namespace p4_nano_ppa_internal_tile
