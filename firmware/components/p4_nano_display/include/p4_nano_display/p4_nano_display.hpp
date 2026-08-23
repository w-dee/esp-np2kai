#pragma once

#include <cstdint>

#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"

namespace p4_nano_display {

/* Concrete P4-NANO display ownership used by bounded diagnostics and the
 * first live consumer. The driver-owned framebuffer remains singular. */
struct DisplaySession {
    esp_ldo_channel_handle_t ldo = nullptr;
    esp_lcd_dsi_bus_handle_t dsi_bus = nullptr;
    esp_lcd_panel_io_handle_t dbi_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    std::uint16_t *framebuffer = nullptr;
    /* Used only by the existing source-pattern diagnostic. */
    std::uint16_t *transform_source = nullptr;
};

/* Reuses the qualified Step 7B.2a initialization sequence with backlight OFF.
 * The caller owns the session object and must call display_session_cleanup(). */
esp_err_t display_session_initialize(DisplaySession *session);
esp_err_t display_session_sync_framebuffer(DisplaySession *session);
esp_err_t display_session_cleanup(DisplaySession *session);

/* Runs the bounded, static native-panel foundation validation. */
esp_err_t run_foundation();

} // namespace p4_nano_display
