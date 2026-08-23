/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_display/p4_nano_display_transform_pattern.hpp"

#include "p4_nano_display/p4_nano_display_pattern.hpp"

namespace {

using p4_nano_display::kTransformSourceHeight;
using p4_nano_display::kTransformSourceWidth;

constexpr std::uint16_t kBlack = 0x0000;
constexpr std::uint16_t kRed = 0xf800;
constexpr std::uint16_t kGreen = 0x07e0;
constexpr std::uint16_t kBlue = 0x001f;
constexpr std::uint16_t kWhite = 0xffff;
constexpr std::uint16_t kYellow = 0xffe0;
constexpr std::uint16_t kCyan = 0x07ff;
constexpr std::uint16_t kMagenta = 0xf81f;
constexpr std::uint16_t kOrange = 0xfd20;
constexpr std::uint16_t kMarkerPrimary = 0x05b8;
constexpr std::uint16_t kMarkerAccent = 0xd81f;
constexpr std::size_t kBorder = 8;
constexpr std::size_t kCorner = 64;
constexpr std::size_t kMarkerX = 176;
constexpr std::size_t kMarkerY = 124;
constexpr std::size_t kMarkerWidth = 48;
constexpr std::size_t kMarkerHeight = 36;
constexpr std::size_t kMarkerAccentX = 12;
constexpr std::size_t kMarkerAccentY = 9;
constexpr std::size_t kMarkerAccentWidth = 15;
constexpr std::size_t kMarkerAccentHeight = 20;

std::uint16_t pixel_for(std::size_t x, std::size_t y)
{
    std::uint16_t pixel = kBlack;
    if (y < kBorder) {
        pixel = kRed;
    } else if (x >= kTransformSourceWidth - kBorder) {
        pixel = kGreen;
    } else if (y >= kTransformSourceHeight - kBorder) {
        pixel = kBlue;
    } else if (x < kBorder) {
        pixel = kWhite;
    }

    if (x < kCorner && y < kCorner) {
        pixel = kYellow;
    } else if (x >= kTransformSourceWidth - kCorner && y < kCorner) {
        pixel = kMagenta;
    } else if (x < kCorner && y >= kTransformSourceHeight - kCorner) {
        pixel = kCyan;
    } else if (x >= kTransformSourceWidth - kCorner &&
               y >= kTransformSourceHeight - kCorner) {
        pixel = kOrange;
    }

    if (x >= kMarkerX && x < kMarkerX + kMarkerWidth &&
        y >= kMarkerY && y < kMarkerY + kMarkerHeight) {
        pixel = kMarkerPrimary;
        if (x >= kMarkerX + kMarkerAccentX &&
            x < kMarkerX + kMarkerAccentX + kMarkerAccentWidth &&
            y >= kMarkerY + kMarkerAccentY &&
            y < kMarkerY + kMarkerAccentY + kMarkerAccentHeight) {
            pixel = kMarkerAccent;
        }
    }
    return pixel;
}

} // namespace

namespace p4_nano_display {

bool fill_transform_source_pattern(std::span<std::uint16_t> pixels)
{
    if (pixels.size() != kTransformSourcePixelCount) {
        return false;
    }
    for (std::size_t y = 0; y < kTransformSourceHeight; ++y) {
        for (std::size_t x = 0; x < kTransformSourceWidth; ++x) {
            pixels[y * kTransformSourceWidth + x] = pixel_for(x, y);
        }
    }
    return true;
}

std::uint32_t transform_source_pattern_crc32(
    std::span<const std::uint16_t> pixels)
{
    if (pixels.size() != kTransformSourcePixelCount) {
        return 0;
    }
    return crc32(reinterpret_cast<const std::uint8_t *>(pixels.data()),
                 kTransformSourceBytes);
}

} // namespace p4_nano_display
