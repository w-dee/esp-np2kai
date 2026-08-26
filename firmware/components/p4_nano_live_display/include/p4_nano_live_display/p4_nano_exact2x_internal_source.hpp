/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace p4_nano_exact2x_internal_source {

inline constexpr std::size_t kOriginalSourceBytes = 640U * 400U * 2U;
inline constexpr std::size_t kRotatedSourceBytes = 400U * 640U * 2U;
inline constexpr std::size_t kDestinationBytes = 800U * 1280U * 2U;
inline constexpr std::size_t kTileBytes = 400U * 128U * 2U;
inline constexpr std::size_t kTileDestinationBytes = 800U * 256U * 2U;
inline constexpr std::size_t kTileCount = 5U;
inline constexpr std::size_t kRequiredAlignmentBytes = 64U;
inline constexpr std::size_t kWarmupSamples = 8U;
inline constexpr std::size_t kMeasuredSamples = 128U;
inline constexpr std::size_t kFinalValidationSamples = 1U;
inline constexpr std::uint32_t kExpectedOriginalCrc = 0x8dadbf82U;
inline constexpr std::uint32_t kExpectedRotatedCrc = 0x379511d7U;
inline constexpr std::uint32_t kExpectedDestinationCrc = 0xc8a10b55U;

struct Input final {
    const std::uint8_t *original_source = nullptr;
    std::size_t original_source_bytes = 0U;
    const void *presentation_slot0 = nullptr;
    const void *presentation_slot1 = nullptr;
    std::size_t presentation_slot_bytes = 0U;
    const void *active_framebuffer = nullptr;
    std::size_t active_framebuffer_bytes = 0U;
};

/* Runs the P10F-B same-binary PSRAM-source versus PPA/internal-source PIE
 * experiment. PPA work is deliberately excluded from the reported PIE
 * intervals. */
esp_err_t run(const Input &input);

} // namespace p4_nano_exact2x_internal_source
