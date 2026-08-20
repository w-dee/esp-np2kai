#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "display_patterns.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_jd9365_10_1.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "p4_nano_jd9365_safe.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "p4_nano_display"

#define DISPLAY_DSI_LANE_COUNT 2
#define DISPLAY_DSI_LANE_BITRATE_MBPS 1500
#define DISPLAY_DPI_CLOCK_MHZ 80
#define DISPLAY_I2C_PORT I2C_NUM_1
#define DISPLAY_I2C_SDA GPIO_NUM_7
#define DISPLAY_I2C_SCL GPIO_NUM_8
#define DISPLAY_I2C_ADDRESS 0x45
#define DISPLAY_BACKLIGHT_REGISTER 0x96
#define DISPLAY_BACKLIGHT_OFF_LEVEL 0x00
#define DISPLAY_BACKLIGHT_INITIAL_LEVEL 0x40
#define DISPLAY_I2C_TIMEOUT_MS 100
#define DISPLAY_DSI_PHY_LDO_CHANNEL 3
#define DISPLAY_DSI_PHY_LDO_MV 2500
#define DISPLAY_PATTERN_HOLD_MS 1000
#define DISPLAY_FINAL_HOLD_MS 2000

static bool s_display_i2c_installed;

static bool display_hardware_test_enabled(void)
{
#ifdef CONFIG_P4_NANO_DISPLAY_RUN_HARDWARE_TEST
    return CONFIG_P4_NANO_DISPLAY_RUN_HARDWARE_TEST;
#else
    return false;
#endif
}

static esp_err_t display_i2c_init(void)
{
    const i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = DISPLAY_I2C_SDA,
        .scl_io_num = DISPLAY_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(DISPLAY_I2C_PORT, &config), TAG,
                        "display I2C configuration failed");
    esp_err_t ret = i2c_driver_install(DISPLAY_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret == ESP_OK) {
        s_display_i2c_installed = true;
    }
    return ret;
}

static esp_err_t display_backlight_set(uint8_t level)
{
    const uint8_t payload[] = {DISPLAY_BACKLIGHT_REGISTER, level};
    esp_err_t ret = i2c_master_write_to_device(
        DISPLAY_I2C_PORT, DISPLAY_I2C_ADDRESS, payload, sizeof(payload),
        pdMS_TO_TICKS(DISPLAY_I2C_TIMEOUT_MS));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "P4-NANO DISPLAY BACKLIGHT: PASS register=0x%02x level=0x%02x",
                 DISPLAY_BACKLIGHT_REGISTER, level);
    }
    return ret;
}

static esp_err_t display_framebuffer_sync(void *framebuffer)
{
    return esp_cache_msync(framebuffer, DISPLAY_PATTERN_FRAMEBUFFER_BYTES,
                           ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static esp_err_t run_physical_display_test(void)
{
    esp_err_t ret;
    esp_ldo_channel_handle_t ldo = NULL;
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_panel_io_handle_t dbi_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    void *panel_framebuffer = NULL;

    ret = display_i2c_init();
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "display I2C initialization failed");
    ESP_LOGI(TAG, "P4-NANO DISPLAY BACKLIGHT SAFE-OFF: BEGIN address=0x%02x register=0x%02x",
             DISPLAY_I2C_ADDRESS, DISPLAY_BACKLIGHT_REGISTER);
    ret = display_backlight_set(DISPLAY_BACKLIGHT_OFF_LEVEL);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "display backlight safe-off failed");

    const esp_ldo_channel_config_t ldo_config = {
        .chan_id = DISPLAY_DSI_PHY_LDO_CHANNEL,
        .voltage_mv = DISPLAY_DSI_PHY_LDO_MV,
    };
    ret = esp_ldo_acquire_channel(&ldo_config, &ldo);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "MIPI DSI PHY LDO acquire failed");

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
    ret = p4_nano_esp_lcd_new_panel_jd9365_safe(dbi_io, &panel_config, &panel);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "JD9365 panel creation failed");

    ret = esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &panel_framebuffer);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "DPI framebuffer acquisition failed");
    ESP_LOGI(TAG, "P4-NANO DISPLAY FB ALLOC: PASS bytes=%u", DISPLAY_PATTERN_FRAMEBUFFER_BYTES);
    if (!display_pattern_fill(DISPLAY_PATTERN_BLACK, panel_framebuffer,
                              DISPLAY_PATTERN_WIDTH * DISPLAY_PATTERN_HEIGHT) ||
        !display_pattern_verify_representative(
            DISPLAY_PATTERN_BLACK, panel_framebuffer,
            DISPLAY_PATTERN_WIDTH * DISPLAY_PATTERN_HEIGHT)) {
        ret = ESP_FAIL;
        ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "initial black framebuffer generation failed");
    }
    ret = display_framebuffer_sync(panel_framebuffer);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "initial framebuffer cache sync failed");
    ret = esp_lcd_panel_reset(panel);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "JD9365 panel reset failed");
    ret = esp_lcd_panel_init(panel);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "JD9365 panel init failed");
    ret = esp_lcd_panel_disp_on_off(panel, true);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "JD9365 display-on failed");

    vTaskDelay(pdMS_TO_TICKS(200));
    ret = display_backlight_set(DISPLAY_BACKLIGHT_INITIAL_LEVEL);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "conservative backlight enable failed");
    ESP_LOGI(TAG, "P4-NANO DISPLAY BACKLIGHT SAFE: PASS level=0x%02x",
             DISPLAY_BACKLIGHT_INITIAL_LEVEL);

    for (display_pattern_kind_t pattern = DISPLAY_PATTERN_BLACK;
         pattern < DISPLAY_PATTERN_COUNT; ++pattern) {
        const bool fill_ok = display_pattern_fill(
            pattern, panel_framebuffer, DISPLAY_PATTERN_WIDTH * DISPLAY_PATTERN_HEIGHT);
        const bool representative_ok = fill_ok && display_pattern_verify_representative(
            pattern, panel_framebuffer, DISPLAY_PATTERN_WIDTH * DISPLAY_PATTERN_HEIGHT);
        const esp_err_t sync_ret = representative_ok ? display_framebuffer_sync(panel_framebuffer) : ESP_FAIL;
        const uint32_t crc = display_pattern_crc32(
            (const uint8_t *)panel_framebuffer, DISPLAY_PATTERN_FRAMEBUFFER_BYTES);
        const bool pattern_pass = fill_ok && representative_ok && sync_ret == ESP_OK &&
                                  crc == display_pattern_expected_crc32(pattern);
        ESP_LOGI(TAG, "P4-NANO DISPLAY PATTERN %s: %s crc32=0x%08" PRIx32,
                 display_pattern_name(pattern), pattern_pass ? "PASS" : "FAIL", crc);
        if (!pattern_pass) {
            ret = ESP_FAIL;
            ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "physical pattern validation failed");
        }
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_PATTERN_HOLD_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(DISPLAY_FINAL_HOLD_MS));
    ret = display_backlight_set(DISPLAY_BACKLIGHT_OFF_LEVEL);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "display backlight safe-off failed");

cleanup:
    if (panel != NULL) {
        if (s_display_i2c_installed) {
            (void)display_backlight_set(DISPLAY_BACKLIGHT_OFF_LEVEL);
        }
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
    if (s_display_i2c_installed) {
        i2c_driver_delete(DISPLAY_I2C_PORT);
        s_display_i2c_installed = false;
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

    bool overall_pass = true;
    for (display_pattern_kind_t pattern = DISPLAY_PATTERN_BLACK;
         pattern < DISPLAY_PATTERN_COUNT; ++pattern) {
        const bool fill_ok = display_pattern_fill(
            pattern, framebuffer, DISPLAY_PATTERN_WIDTH * DISPLAY_PATTERN_HEIGHT);
        const bool representative_ok = fill_ok && display_pattern_verify_representative(
            pattern, framebuffer, DISPLAY_PATTERN_WIDTH * DISPLAY_PATTERN_HEIGHT);
        const uint32_t crc = display_pattern_crc32(
            (const uint8_t *)framebuffer, DISPLAY_PATTERN_FRAMEBUFFER_BYTES);
        const bool pattern_pass = fill_ok && representative_ok &&
                                  crc == display_pattern_expected_crc32(pattern);
        overall_pass = overall_pass && pattern_pass;
        ESP_LOGI(TAG, "P4-NANO DISPLAY PATTERN %s: %s crc32=0x%08" PRIx32,
                 display_pattern_name(pattern), pattern_pass ? "PASS" : "FAIL", crc);
    }
    free(framebuffer);

    if (overall_pass) {
        ESP_LOGI(TAG, "P4-NANO DISPLAY HW ACCESS: NOT RUN");
        ESP_LOGI(TAG, "P4-NANO DISPLAY OFFLINE RESULT: PASS");
    } else {
        ESP_LOGE(TAG, "P4-NANO DISPLAY OFFLINE RESULT: FAIL");
    }
    return overall_pass;
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
