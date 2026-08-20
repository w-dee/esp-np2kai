#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/*
 * Project-owned JD9365 adapter. Unlike the upstream constructor, this API
 * does not perform display-side I2C or backlight writes while creating the
 * panel object. The caller owns that policy explicitly.
 */
esp_err_t p4_nano_esp_lcd_new_panel_jd9365_safe(
    esp_lcd_panel_io_handle_t io,
    const esp_lcd_panel_dev_config_t *panel_dev_config,
    esp_lcd_panel_handle_t *ret_panel);
