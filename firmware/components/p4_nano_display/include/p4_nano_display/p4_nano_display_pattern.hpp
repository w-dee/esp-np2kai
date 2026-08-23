#pragma once

#include <cstddef>
#include <cstdint>

namespace p4_nano_display {

inline constexpr std::size_t kNativeWidth = 800;
inline constexpr std::size_t kNativeHeight = 1280;
inline constexpr std::size_t kNativePixelCount = kNativeWidth * kNativeHeight;
inline constexpr std::size_t kNativeStrideBytes = kNativeWidth * sizeof(std::uint16_t);
inline constexpr std::size_t kNativeFramebufferBytes = kNativeStrideBytes * kNativeHeight;
inline constexpr std::uint8_t kDisplayControlRegister = 0x95;

bool fill_static_pattern(std::uint16_t *pixels, std::size_t pixel_count);
std::uint32_t crc32(const std::uint8_t *bytes, std::size_t length);
std::uint32_t static_pattern_crc32(const std::uint16_t *pixels,
                                   std::size_t pixel_count);

} // namespace p4_nano_display
