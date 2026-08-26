#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "p4_nano_display/p4_nano_display_timing_profiles.hpp"

namespace p4_nano_display {

using RefreshVisualConfig = DisplayTimingProfile;

/* The visual image is selected at configure time; there is no runtime clock
 * retuning or target selection. */
#if defined(P4_NANO_REFRESH_VISUAL_BASELINE_PROFILE)
inline constexpr const RefreshVisualConfig &kRefreshVisualConfig =
    kDisplayTimingBaseline;
#elif defined(P4_NANO_REFRESH_VISUAL_LOWER1_PROFILE)
inline constexpr const RefreshVisualConfig &kRefreshVisualConfig =
    kDisplayTimingLower1;
#elif defined(P4_NANO_REFRESH_VISUAL_LOWER2_PROFILE)
inline constexpr const RefreshVisualConfig &kRefreshVisualConfig =
    kDisplayTimingLower2;
#else
#error "P4_NANO_REFRESH_VISUAL_PROFILE requires exactly one visual target"
#endif

static_assert(kRefreshVisualConfig.dpi_clock_source ==
                  MIPI_DSI_DPI_CLK_SRC_PLL_F240M,
              "visual profiles must use explicit PLL_F240M");
static_assert(kRefreshVisualConfig.htotal == 880U &&
                  kRefreshVisualConfig.vtotal == 1324U,
              "visual profiles must preserve native timing totals");
#if defined(P4_NANO_REFRESH_VISUAL_LOWER1_PROFILE)
static_assert(kRefreshVisualConfig.predicted_divider == 5U,
              "LOWER-1 must select PLL240 divider 5");
#elif defined(P4_NANO_REFRESH_VISUAL_LOWER2_PROFILE)
static_assert(kRefreshVisualConfig.predicted_divider == 7U,
              "LOWER-2 must select PLL240 divider 7");
#elif defined(P4_NANO_REFRESH_VISUAL_BASELINE_PROFILE)
static_assert(kRefreshVisualConfig.predicted_divider == 3U,
              "BASELINE must select PLL240 divider 3");
#endif

bool fill_refresh_visual_pattern(std::uint16_t *pixels,
                                 std::size_t pixel_count) noexcept;
std::uint32_t refresh_visual_pattern_crc32(const std::uint16_t *pixels,
                                           std::size_t pixel_count) noexcept;
bool validate_refresh_visual_pattern(const std::uint16_t *pixels,
                                     std::size_t pixel_count) noexcept;
bool update_refresh_visual_marker(std::uint16_t *pixels,
                                  std::size_t pixel_count,
                                  bool highlighted) noexcept;

/* Runs the diagnostic-only 60-second human inspection profile. */
esp_err_t run_refresh_visual();

} // namespace p4_nano_display
