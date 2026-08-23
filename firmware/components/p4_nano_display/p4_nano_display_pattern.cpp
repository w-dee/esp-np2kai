/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_display/p4_nano_display_pattern.hpp"

namespace {

constexpr std::uint16_t kBlack = 0x0000;
constexpr std::uint16_t kRed = 0xf800;
constexpr std::uint16_t kGreen = 0x07e0;
constexpr std::uint16_t kBlue = 0x001f;
constexpr std::uint16_t kWhite = 0xffff;
constexpr std::uint16_t kYellow = 0xffe0;
constexpr std::uint16_t kCyan = 0x07ff;
constexpr std::uint16_t kMagenta = 0xf81f;
constexpr std::uint16_t kOrange = 0xfd20;
constexpr std::size_t kBorder = 8;
constexpr std::size_t kCorner = 64;

std::uint16_t pixel_for(std::size_t x, std::size_t y)
{
    std::uint16_t pixel = kBlack;
    if (y < kBorder) {
        pixel = kRed;
    } else if (x >= p4_nano_display::kNativeWidth - kBorder) {
        pixel = kGreen;
    } else if (y >= p4_nano_display::kNativeHeight - kBorder) {
        pixel = kBlue;
    } else if (x < kBorder) {
        pixel = kWhite;
    }

    if (x < kCorner && y < kCorner) {
        pixel = kYellow;
    } else if (x >= p4_nano_display::kNativeWidth - kCorner && y < kCorner) {
        pixel = kMagenta;
    } else if (x < kCorner && y >= p4_nano_display::kNativeHeight - kCorner) {
        pixel = kCyan;
    } else if (x >= p4_nano_display::kNativeWidth - kCorner &&
               y >= p4_nano_display::kNativeHeight - kCorner) {
        pixel = kOrange;
    }
    return pixel;
}

} // namespace

namespace p4_nano_display {

bool fill_static_pattern(std::uint16_t *pixels, std::size_t pixel_count)
{
    if (pixels == nullptr || pixel_count < kNativePixelCount) {
        return false;
    }
    for (std::size_t y = 0; y < kNativeHeight; ++y) {
        for (std::size_t x = 0; x < kNativeWidth; ++x) {
            pixels[y * kNativeWidth + x] = pixel_for(x, y);
        }
    }
    return true;
}

std::uint32_t crc32(const std::uint8_t *bytes, std::size_t length)
{
    if (bytes == nullptr && length != 0) {
        return 0;
    }
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return crc ^ 0xffffffffU;
}

std::uint32_t static_pattern_crc32(const std::uint16_t *pixels,
                                   std::size_t pixel_count)
{
    if (pixels == nullptr || pixel_count < kNativePixelCount) {
        return 0;
    }
    return crc32(reinterpret_cast<const std::uint8_t *>(pixels),
                 kNativeFramebufferBytes);
}

} // namespace p4_nano_display
