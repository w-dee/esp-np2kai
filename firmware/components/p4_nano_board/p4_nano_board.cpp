/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_board/p4_nano_board.hpp"

#include <cstddef>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "p4_nano_board";
constexpr gpio_num_t kSdaGpio = GPIO_NUM_7;
constexpr gpio_num_t kSclGpio = GPIO_NUM_8;
constexpr i2c_port_num_t kI2cPort = I2C_NUM_1;
constexpr std::uint32_t kI2cSpeedHz = 100000;
constexpr int kI2cTimeoutMs = 100;
constexpr TickType_t kPanelPowerDelay = pdMS_TO_TICKS(100);

i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_display_control = nullptr;

esp_err_t write_register(std::uint8_t reg, std::uint8_t value)
{
    if (s_display_control == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    const std::uint8_t payload[] = {reg, value};
    return i2c_master_transmit(s_display_control, payload, sizeof(payload),
                                kI2cTimeoutMs);
}

} // namespace

namespace p4_nano_board {

bool display_control_is_initialized()
{
    return s_bus != nullptr && s_display_control != nullptr;
}

esp_err_t display_control_init()
{
    if (display_control_is_initialized()) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = kI2cPort,
        .sda_io_num = kSdaGpio,
        .scl_io_num = kSclGpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
            .allow_pd = 0,
        },
    };
    esp_err_t ret = i2c_new_master_bus(&bus_config, &s_bus);
    if (ret != ESP_OK) {
        s_bus = nullptr;
        ESP_LOGE(kTag, "shared I2C bus creation failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = kDisplayControlAddress,
        .scl_speed_hz = kI2cSpeedHz,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        },
    };
    ret = i2c_master_bus_add_device(s_bus, &device_config,
                                    &s_display_control);
    if (ret != ESP_OK) {
        s_display_control = nullptr;
        (void)i2c_del_master_bus(s_bus);
        s_bus = nullptr;
        ESP_LOGE(kTag, "display control device creation failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(kTag, "shared I2C ready: GPIO7/GPIO8 address=0x%02x",
             kDisplayControlAddress);
    return ESP_OK;
}

esp_err_t display_control_write(std::uint8_t reg, std::uint8_t value)
{
    const esp_err_t ret = write_register(reg, value);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "display control write failed: reg=0x%02x value=0x%02x error=%s",
                 reg, value, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t display_control_panel_power_on()
{
    esp_err_t ret = display_control_write(kPanelControlRegister, 0x11);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = display_control_write(kPanelControlRegister, 0x17);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = display_backlight_set(kBacklightOff);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(kPanelPowerDelay);
    ESP_LOGI(kTag, "panel control sequence complete: 0x95=0x11->0x17 backlight=0x00");
    return ESP_OK;
}

esp_err_t display_backlight_set(std::uint8_t value)
{
    return display_control_write(kBacklightRegister, value);
}

esp_err_t display_control_safe_off()
{
    if (!display_control_is_initialized()) {
        return ESP_OK;
    }
    return display_backlight_set(kBacklightOff);
}

esp_err_t display_control_deinit()
{
    esp_err_t first_error = ESP_OK;
    if (display_control_is_initialized()) {
        const esp_err_t off_ret = display_control_safe_off();
        if (off_ret != ESP_OK) {
            first_error = off_ret;
        }
    }
    if (s_display_control != nullptr) {
        const esp_err_t ret = i2c_master_bus_rm_device(s_display_control);
        if (first_error == ESP_OK && ret != ESP_OK) {
            first_error = ret;
        }
        s_display_control = nullptr;
    }
    if (s_bus != nullptr) {
        const esp_err_t ret = i2c_del_master_bus(s_bus);
        if (first_error == ESP_OK && ret != ESP_OK) {
            first_error = ret;
        }
        s_bus = nullptr;
    }
    return first_error;
}

} // namespace p4_nano_board
