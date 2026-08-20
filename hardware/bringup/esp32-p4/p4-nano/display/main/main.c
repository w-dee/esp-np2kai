#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "display_patterns.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_jd9365_10_1.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "p4_nano_display"

#define DISPLAY_DSI_LANE_COUNT 2
#define DISPLAY_DSI_LANE_BITRATE_MBPS 1500
#define DISPLAY_DPI_CLOCK_MHZ 80
#define DISPLAY_I2C_ADDRESS 0x45
#define DISPLAY_BACKLIGHT_REGISTER 0x96
#define DISPLAY_DSI_PHY_LDO_CHANNEL 3
#define DISPLAY_DSI_PHY_LDO_MV 2500

static bool display_hardware_test_enabled(void)
{
#ifdef CONFIG_P4_NANO_DISPLAY_RUN_HARDWARE_TEST
    return CONFIG_P4_NANO_DISPLAY_RUN_HARDWARE_TEST;
#else
    return false;
#endif
}

static esp_err_t run_physical_display_test(void)
{
    esp_err_t ret;
    esp_ldo_channel_handle_t ldo = NULL;
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_panel_io_handle_t dbi_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    const esp_ldo_channel_config_t ldo_config = {
        .chan_id = DISPLAY_DSI_PHY_LDO_CHANNEL,
        .voltage_mv = DISPLAY_DSI_PHY_LDO_MV,
    };
    ret = esp_ldo_acquire_channel(&ldo_config, &ldo);
    ESP_RETURN_ON_ERROR(ret, TAG, "MIPI DSI PHY LDO acquire failed");

    esp_lcd_dsi_bus_config_t bus_config = JD9365_PANEL_BUS_DSI_2CH_CONFIG();
    bus_config.lane_bit_rate_mbps = DISPLAY_DSI_LANE_BITRATE_MBPS;
    ret = esp_lcd_new_dsi_bus(&bus_config, &dsi_bus);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "MIPI DSI bus creation failed");

    esp_lcd_dbi_io_config_t dbi_config = JD9365_PANEL_IO_DBI_CONFIG();
    ret = esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &dbi_io);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "MIPI DBI IO creation failed");

    esp_lcd_dpi_panel_config_t dpi_config =
        JD9365_800_1280_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    dpi_config.dpi_clock_freq_mhz = DISPLAY_DPI_CLOCK_MHZ;
    jd9365_vendor_config_t vendor_config = {
        .flags = {
            .use_mipi_interface = 1,
        },
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_config,
            .lane_num = DISPLAY_DSI_LANE_COUNT,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ret = esp_lcd_new_panel_jd9365(dbi_io, &panel_config, &panel);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "JD9365 panel creation failed");
    ret = esp_lcd_panel_reset(panel);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "JD9365 panel reset failed");
    ret = esp_lcd_panel_init(panel);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "JD9365 panel init failed");
    ret = esp_lcd_panel_disp_on_off(panel, true);

cleanup:
    if (panel != NULL) {
        esp_lcd_panel_del(panel);
    }
    if (dbi_io != NULL) {
        esp_lcd_panel_io_del(dbi_io);
    }
    if (dsi_bus != NULL) {
        esp_lcd_del_dsi_bus(dsi_bus);
    }
    if (ldo != NULL) {
        esp_ldo_release_channel(ldo);
    }
    return ret;
}

static bool run_offline_pattern_test(void)
{
    ESP_LOGI(TAG, "P4-NANO DISPLAY OFFLINE START");
    ESP_LOGI(TAG, "P4-NANO DISPLAY HW GATE: %s",
             display_hardware_test_enabled() ? "ENABLED" : "DISABLED");
    ESP_LOGI(TAG, "P4-NANO DISPLAY CONFIG: RGB565 %ux%u stride=%u bytes=%u",
             DISPLAY_PATTERN_WIDTH, DISPLAY_PATTERN_HEIGHT, DISPLAY_PATTERN_STRIDE,
             DISPLAY_PATTERN_FRAMEBUFFER_BYTES);

    uint16_t *framebuffer = heap_caps_malloc(
        DISPLAY_PATTERN_FRAMEBUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (framebuffer == NULL) {
        ESP_LOGE(TAG, "P4-NANO DISPLAY FB ALLOC: FAIL bytes=%u", DISPLAY_PATTERN_FRAMEBUFFER_BYTES);
        return false;
    }
    ESP_LOGI(TAG, "P4-NANO DISPLAY FB ALLOC: PASS bytes=%u", DISPLAY_PATTERN_FRAMEBUFFER_BYTES);

    bool pass = true;
    for (display_pattern_kind_t pattern = DISPLAY_PATTERN_BLACK;
         pattern < DISPLAY_PATTERN_COUNT; ++pattern) {
        pass = display_pattern_fill(pattern, framebuffer,
                                    DISPLAY_PATTERN_WIDTH * DISPLAY_PATTERN_HEIGHT) && pass;
        pass = display_pattern_verify_representative(
                   pattern, framebuffer, DISPLAY_PATTERN_WIDTH * DISPLAY_PATTERN_HEIGHT) && pass;
        const uint32_t crc = display_pattern_crc32(
            (const uint8_t *)framebuffer, DISPLAY_PATTERN_FRAMEBUFFER_BYTES);
        ESP_LOGI(TAG, "P4-NANO DISPLAY PATTERN %s: %s crc32=0x%08" PRIx32,
                 display_pattern_name(pattern), pass ? "PASS" : "FAIL", crc);
    }
    free(framebuffer);

    if (pass) {
        ESP_LOGI(TAG, "P4-NANO DISPLAY HW ACCESS: NOT RUN");
        ESP_LOGI(TAG, "P4-NANO DISPLAY OFFLINE RESULT: PASS");
    } else {
        ESP_LOGE(TAG, "P4-NANO DISPLAY OFFLINE RESULT: FAIL");
    }
    return pass;
}

static esp_err_t run_hardware_test(void)
{
    if (!display_hardware_test_enabled()) {
        ESP_LOGI(TAG, "P4-NANO DISPLAY HARDWARE TEST: NOT RUN");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "P4-NANO DISPLAY HARDWARE TEST: ENABLED");
    if (run_physical_display_test() != ESP_OK) {
        ESP_LOGE(TAG, "P4-NANO DISPLAY HARDWARE RESULT: FAIL");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "P4-NANO DISPLAY HARDWARE RESULT: PASS");
    return ESP_OK;
}

void app_main(void)
{
    if (!run_offline_pattern_test()) {
        return;
    }
    (void)run_hardware_test();
}
