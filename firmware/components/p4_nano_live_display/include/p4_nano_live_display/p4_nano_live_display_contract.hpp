#pragma once

#include <cstddef>
#include <cstdint>

namespace p4_nano_live_display {

/* The immutable source/slot/native geometry is deliberately shared with the
 * host contract test so a live build cannot silently drift from the tested
 * presentation boundary. */
inline constexpr std::uint32_t kSourceWidth = 640U;
inline constexpr std::uint32_t kSourceHeight = 400U;
inline constexpr std::size_t kSourceBytes =
    static_cast<std::size_t>(kSourceWidth) * kSourceHeight * sizeof(std::uint16_t);
inline constexpr std::size_t kPresentationSlotCount = 2U;
inline constexpr std::size_t kPresentationSlotBytes = kSourceBytes;
inline constexpr std::uint32_t kNativeWidth = 800U;
inline constexpr std::uint32_t kNativeHeight = 1280U;
inline constexpr std::size_t kNativeFramebufferBytes =
    static_cast<std::size_t>(kNativeWidth) * kNativeHeight * sizeof(std::uint16_t);
inline constexpr char kCanonicalRotation[] = "CCW";

} // namespace p4_nano_live_display
