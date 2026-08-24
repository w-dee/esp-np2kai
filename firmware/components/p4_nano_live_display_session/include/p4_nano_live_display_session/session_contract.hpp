#pragma once

#include <cstddef>
#include <cstdint>

namespace p4_nano_live_display_session {

/* This is the production-compatible source contract.  It is intentionally
 * independent of np2video fixtures, golden metadata, and validation CRCs. */
inline constexpr std::uint32_t kSourceWidth = 640U;
inline constexpr std::uint32_t kSourceHeight = 400U;
inline constexpr std::uint32_t kSourceBpp = 16U;
inline constexpr std::uint32_t kPixelFormatRgb565Le = 1U;
inline constexpr std::size_t kSourcePitch =
    static_cast<std::size_t>(kSourceWidth) * sizeof(std::uint16_t);
inline constexpr std::size_t kSourceBytes =
    kSourcePitch * static_cast<std::size_t>(kSourceHeight);
inline constexpr std::size_t kPresentationSlotCount = 2U;
inline constexpr std::size_t kPresentationSlotBytes = kSourceBytes;
inline constexpr std::uint32_t kNativeWidth = 800U;
inline constexpr std::uint32_t kNativeHeight = 1280U;
inline constexpr std::size_t kNativeFramebufferBytes =
    static_cast<std::size_t>(kNativeWidth) * kNativeHeight *
    sizeof(std::uint16_t);
inline constexpr char kCanonicalRotation[] = "CCW";

constexpr bool source_geometry_valid(std::uint32_t width,
                                     std::uint32_t height,
                                     std::size_t pitch,
                                     std::uint32_t bpp,
                                     std::uint32_t pixel_format) noexcept
{
    return width == kSourceWidth && height == kSourceHeight &&
           pitch == kSourcePitch && bpp == kSourceBpp &&
           pixel_format == kPixelFormatRgb565Le;
}

/* Platform-neutral state helper: only the first completed frame transitions
 * visibility.  Hardware backlight I/O remains owned by Session. */
struct FirstFrameGate final {
    bool visible = false;

    constexpr bool mark_valid_frame() noexcept
    {
        if (visible) {
            return false;
        }
        visible = true;
        return true;
    }

    constexpr void reset() noexcept { visible = false; }
};

} // namespace p4_nano_live_display_session
