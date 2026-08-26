#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_lcd_mipi_dsi.h"

namespace p4_nano_display {

struct DisplayTimingProfile final {
    const char *name;
    const char *dpi_source_name;
    mipi_dsi_dpi_clock_source_t dpi_clock_source;
    float requested_dpi_mhz;
    std::uint32_t predicted_divider;
    float predicted_real_dpi_mhz;
    std::size_t htotal;
    std::size_t vtotal;
    float predicted_refresh_hz;
    float requested_lane_mbps;
};

inline constexpr DisplayTimingProfile kDisplayTimingBaseline{
    "baseline", "PLL_F240M", MIPI_DSI_DPI_CLK_SRC_PLL_F240M,
    80.0F, 3U, 80.0F, 880U, 1324U, 68.662455F, 1500.0F};

inline constexpr DisplayTimingProfile kDisplayTimingLower1{
    "lower1", "PLL_F240M", MIPI_DSI_DPI_CLK_SRC_PLL_F240M,
    48.0F, 5U, 48.0F, 880U, 1324U, 41.197473F, 700.0F};

inline constexpr DisplayTimingProfile kDisplayTimingLower2{
    "lower2", "PLL_F240M", MIPI_DSI_DPI_CLK_SRC_PLL_F240M,
    240.0F / 7.0F, 7U, 240.0F / 7.0F, 880U, 1324U, 29.426767F,
    500.0F};

static_assert(kDisplayTimingBaseline.dpi_clock_source ==
                  MIPI_DSI_DPI_CLK_SRC_PLL_F240M &&
                  kDisplayTimingLower1.dpi_clock_source ==
                      MIPI_DSI_DPI_CLK_SRC_PLL_F240M &&
                  kDisplayTimingLower2.dpi_clock_source ==
                      MIPI_DSI_DPI_CLK_SRC_PLL_F240M,
              "diagnostic display profiles must use PLL_F240M");
static_assert(kDisplayTimingBaseline.htotal == 880U &&
                  kDisplayTimingBaseline.vtotal == 1324U &&
                  kDisplayTimingLower1.htotal == 880U &&
                  kDisplayTimingLower1.vtotal == 1324U &&
                  kDisplayTimingLower2.htotal == 880U &&
                  kDisplayTimingLower2.vtotal == 1324U,
              "diagnostic display profiles must preserve native totals");
static_assert(kDisplayTimingBaseline.predicted_divider == 3U &&
                  kDisplayTimingLower1.predicted_divider == 5U &&
                  kDisplayTimingLower2.predicted_divider == 7U,
              "diagnostic display profile dividers changed");

} // namespace p4_nano_display
