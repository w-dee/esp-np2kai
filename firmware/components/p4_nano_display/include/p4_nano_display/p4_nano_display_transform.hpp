/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "p4_nano_display/p4_nano_display_pattern.hpp"

namespace p4_nano_display {

inline constexpr std::size_t kTransformSourceWidth = 640;
inline constexpr std::size_t kTransformSourceHeight = 400;
inline constexpr std::size_t kTransformSourcePixelCount =
    kTransformSourceWidth * kTransformSourceHeight;
inline constexpr std::size_t kTransformDestinationWidth = kNativeWidth;
inline constexpr std::size_t kTransformDestinationHeight = kNativeHeight;
inline constexpr std::size_t kTransformDestinationPixelCount =
    kTransformDestinationWidth * kTransformDestinationHeight;
inline constexpr std::size_t kTransformDestinationStrideBytes =
    kNativeStrideBytes;
inline constexpr std::size_t kTransformDestinationBytes =
    kNativeFramebufferBytes;

enum class QuarterTurn : std::uint8_t {
    Clockwise,
    CounterClockwise,
};

/*
 * Transform one immutable 640x400 RGB565 frame directly into one native
 * 800x1280 RGB565 framebuffer. The two spans must have exactly the sizes
 * declared above and must not overlap. No intermediate 1280x800 image is
 * created; the logical landscape coordinates exist only in the mapping.
 */
bool transform_to_native(std::span<const std::uint16_t> source,
                         std::span<std::uint16_t> destination,
                         QuarterTurn rotation);

} // namespace p4_nano_display
