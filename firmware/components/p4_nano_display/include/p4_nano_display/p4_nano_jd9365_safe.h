#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Project-owned JD9365 adapter derived from
 * waveshare/esp_lcd_jd9365_10_1 version 1.0.4, upstream commit
 * e7721dd43e55cd6b10110543e3efa8dca8e3bfe4. The original source path is
 * esp_lcd_jd9365_10_1.c. Unlike the upstream constructor, this API does not
 * perform display-side I2C or backlight writes while creating the panel
 * object. The P4-NANO board component owns that policy explicitly.
 */
esp_err_t p4_nano_esp_lcd_new_panel_jd9365_safe(
    esp_lcd_panel_io_handle_t io,
    const esp_lcd_panel_dev_config_t *panel_dev_config,
    esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif
