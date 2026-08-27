/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace p4_nano_display {

inline constexpr std::size_t kExact2xSourceWidth = 400U;
inline constexpr std::size_t kExact2xSourceHeight = 640U;
inline constexpr std::size_t kExact2xSourceStrideBytes =
    kExact2xSourceWidth * sizeof(std::uint16_t);
inline constexpr std::size_t kExact2xSourceBytes =
    kExact2xSourceStrideBytes * kExact2xSourceHeight;
inline constexpr std::size_t kExact2xGroupsPerSourceRow =
    kExact2xSourceWidth / 8U;
inline constexpr std::size_t kExact2xDestinationWidth = 800U;
inline constexpr std::size_t kExact2xDestinationHeight = 1280U;
inline constexpr std::size_t kExact2xDestinationStrideBytes =
    kExact2xDestinationWidth * sizeof(std::uint16_t);
inline constexpr std::size_t kExact2xDestinationBytes =
    kExact2xDestinationStrideBytes * kExact2xDestinationHeight;
inline constexpr std::size_t kExact2xTileSourceHeight = 128U;
inline constexpr std::size_t kExact2xTileSourceBytes =
    kExact2xSourceStrideBytes * kExact2xTileSourceHeight;
inline constexpr std::size_t kExact2xTileDestinationHeight = 256U;
inline constexpr std::size_t kExact2xTileDestinationBytes =
    kExact2xDestinationStrideBytes * kExact2xTileDestinationHeight;
inline constexpr std::size_t kExact2xTileCount =
    kExact2xSourceHeight / kExact2xTileSourceHeight;
inline constexpr std::size_t kExact2xM2CAlignmentBytes = 64U;
inline constexpr std::size_t kExact2xRequiredAlignmentBytes = 16U;
inline constexpr std::uint32_t kExact2xExpectedSourceCrc = 0x379511d7U;
inline constexpr std::uint32_t kExact2xExpectedDestinationCrc = 0xc8a10b55U;
inline constexpr std::size_t kExact2xWarmupSamples = 8U;
inline constexpr std::size_t kExact2xMeasuredSamples = 128U;
inline constexpr std::size_t kExact2xFinalValidationSamples = 1U;

/* Scalar packed baseline.  The source and destination are contiguous with
 * either the full-frame or fixed 128-row tile geometry above; no allocation
 * or cache maintenance is performed by this kernel. */
bool exact2x_scalar(const std::uint16_t *source, std::size_t source_bytes,
                   std::uint16_t *destination,
                   std::size_t destination_bytes) noexcept;

/* Fixed-geometry P4 PIE helper. Its caller owns all pointer, external-RAM,
 * alignment, cache, timing, and correctness checks. */
extern "C" void exact2x_pie_aligned(const std::uint16_t *source,
                                    std::uint16_t *destination);

/* Fixed-geometry P10F-B helper. The caller supplies one 400x128 rotated
 * tile and its corresponding 800x256 destination tile. */
extern "C" void exact2x_pie_tile128_aligned(
    const std::uint16_t *source, std::uint16_t *destination);

/* P10M-C0 fixed-geometry horizontal-only helper. */
extern "C" void exact2x_pie_horizontal64_aligned(
    const std::uint16_t *source, std::uint16_t *destination);

/* Dedicated P10K-B0 fixed-geometry T128 correctness candidate.  Its caller
 * owns pointer, cache, and correctness handling; this benchmark/test fixture
 * currently requires 64-byte-aligned buffers while the generic 16-byte
 * kExact2xRequiredAlignmentBytes contract remains unchanged.  It is not
 * selected by any production or performance profile.  q3 is intentionally
 * avoided because ESP-IDF v5.5.4 omits q3 from its FreeRTOS PIE context
 * save/restore path. */
extern "C" void exact2x_pie_tile128_grouped64_aligned(
    const std::uint16_t *source, std::uint16_t *destination);

/* Correctness-harness-only owner-transfer helper. */
extern "C" void exact2x_pie_preemption_clobber_q0_q1(
    const std::uint8_t *source, std::uint8_t *destination);

inline constexpr bool exact2x_pie_available() noexcept { return true; }

} // namespace p4_nano_display
