#pragma once

#include <cstddef>
#include <cstdint>

#include "p4_nano_live_display_session/session_contract.hpp"

namespace p4_nano_live_display {

/* Compatibility aliases for the bounded fixture wrapper.  The canonical
 * production contract lives in the reusable session component. */
inline constexpr std::uint32_t kSourceWidth =
    p4_nano_live_display_session::kSourceWidth;
inline constexpr std::uint32_t kSourceHeight =
    p4_nano_live_display_session::kSourceHeight;
inline constexpr std::size_t kSourceBytes =
    p4_nano_live_display_session::kSourceBytes;
inline constexpr std::size_t kPresentationSlotCount =
    p4_nano_live_display_session::kPresentationSlotCount;
inline constexpr std::size_t kPresentationSlotBytes =
    p4_nano_live_display_session::kPresentationSlotBytes;
inline constexpr std::uint32_t kNativeWidth =
    p4_nano_live_display_session::kNativeWidth;
inline constexpr std::uint32_t kNativeHeight =
    p4_nano_live_display_session::kNativeHeight;
inline constexpr std::size_t kNativeFramebufferBytes =
    p4_nano_live_display_session::kNativeFramebufferBytes;
inline constexpr auto &kCanonicalRotation =
    p4_nano_live_display_session::kCanonicalRotation;

} // namespace p4_nano_live_display
