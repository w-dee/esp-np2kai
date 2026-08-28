/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_board/p4_nano_board.hpp"

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

using p4_nano_board::I2cDeviceLease;
using p4_nano_board::SharedI2cLeaseModel;
using p4_nano_board::SharedI2cLeaseOps;
using p4_nano_board::SharedI2cLeaseResult;
using p4_nano_board::shared_i2c_device_transmit;

constexpr char kTag[] = "p4_nano_board";
constexpr gpio_num_t kSdaGpio = GPIO_NUM_7;
constexpr gpio_num_t kSclGpio = GPIO_NUM_8;
constexpr i2c_port_num_t kI2cPort = I2C_NUM_1;
constexpr std::uint32_t kI2cSpeedHz = 100000;
constexpr int kI2cTimeoutMs = 100;
constexpr TickType_t kPanelPowerDelay = pdMS_TO_TICKS(100);

bool s_pa_initialized = false;
bool s_pa_enabled = false;

int create_bus(void *, std::uintptr_t *bus_token)
{
    if (bus_token == nullptr) {
        return ESP_ERR_INVALID_ARG;
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
    i2c_master_bus_handle_t bus = nullptr;
    const esp_err_t ret = i2c_new_master_bus(&bus_config, &bus);
    if (ret == ESP_OK) {
        *bus_token = reinterpret_cast<std::uintptr_t>(bus);
    }
    return ret;
}

int delete_bus(void *, std::uintptr_t bus_token)
{
    if (bus_token == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_del_master_bus(
        reinterpret_cast<i2c_master_bus_handle_t>(bus_token));
}

int create_device(void *, std::uintptr_t bus_token, std::uint8_t address,
                  std::uintptr_t *device_token)
{
    if (bus_token == 0 || device_token == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = kI2cSpeedHz,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        },
    };
    i2c_master_dev_handle_t device = nullptr;
    const esp_err_t ret = i2c_master_bus_add_device(
        reinterpret_cast<i2c_master_bus_handle_t>(bus_token), &device_config,
        &device);
    if (ret == ESP_OK) {
        *device_token = reinterpret_cast<std::uintptr_t>(device);
    }
    return ret;
}

int delete_device(void *, std::uintptr_t device_token)
{
    if (device_token == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_bus_rm_device(
        reinterpret_cast<i2c_master_dev_handle_t>(device_token));
}

const SharedI2cLeaseOps kI2cLeaseOps = {
    .create_bus = create_bus,
    .delete_bus = delete_bus,
    .create_device = create_device,
    .delete_device = delete_device,
    .context = nullptr,
};

SharedI2cLeaseModel s_i2c_model(kI2cLeaseOps);
I2cDeviceLease s_display_control;

esp_err_t write_register(std::uint8_t reg, std::uint8_t value)
{
    if (!s_i2c_model.owns(&s_display_control)) {
        return ESP_ERR_INVALID_STATE;
    }

    const std::uint8_t payload[] = {reg, value};
    return shared_i2c_device_transmit(&s_display_control, payload,
                                      sizeof(payload), kI2cTimeoutMs);
}

} // namespace

namespace p4_nano_board {

bool display_control_is_initialized()
{
    return s_i2c_model.bus_alive() && s_i2c_model.owns(&s_display_control);
}

esp_err_t shared_i2c_acquire_device(std::uint8_t address,
                                    I2cDeviceLease *lease)
{
    const SharedI2cLeaseResult result = s_i2c_model.acquire(address, lease);
    if (result == SharedI2cLeaseResult::kOk) {
        return ESP_OK;
    }
    switch (result) {
    case SharedI2cLeaseResult::kInvalidArgument:
        return ESP_ERR_INVALID_ARG;
    case SharedI2cLeaseResult::kShutdownBusy:
    case SharedI2cLeaseResult::kDuplicateAddress:
    case SharedI2cLeaseResult::kNoLeaseSlots:
    case SharedI2cLeaseResult::kInvalidOperations:
        return ESP_ERR_INVALID_STATE;
    case SharedI2cLeaseResult::kBusCreateFailed:
    case SharedI2cLeaseResult::kDeviceCreateFailed:
    case SharedI2cLeaseResult::kDeviceDeleteFailed:
    case SharedI2cLeaseResult::kBusDeleteFailed:
        return ESP_FAIL;
    case SharedI2cLeaseResult::kOk:
        break;
    }
    return ESP_FAIL;
}

esp_err_t shared_i2c_release_device(I2cDeviceLease *lease)
{
    const SharedI2cLeaseResult result = s_i2c_model.release(lease);
    if (result == SharedI2cLeaseResult::kOk) {
        return ESP_OK;
    }
    if (result == SharedI2cLeaseResult::kInvalidArgument) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_FAIL;
}

esp_err_t shared_i2c_device_transmit(const I2cDeviceLease *lease,
                                     const void *data, std::size_t length,
                                     int timeout_ms)
{
    if (data == nullptr || !s_i2c_model.owns(lease)) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit(
        reinterpret_cast<i2c_master_dev_handle_t>(lease->token()),
        static_cast<const std::uint8_t *>(data),
        length, timeout_ms);
}

esp_err_t shared_i2c_shutdown()
{
    const SharedI2cLeaseResult result = s_i2c_model.shutdown();
    if (result == SharedI2cLeaseResult::kOk) {
        return ESP_OK;
    }
    if (result == SharedI2cLeaseResult::kShutdownBusy) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_FAIL;
}

bool shared_i2c_is_initialized()
{
    return s_i2c_model.bus_alive();
}

esp_err_t display_control_init()
{
    if (display_control_is_initialized()) {
        return ESP_OK;
    }
    const esp_err_t ret = shared_i2c_acquire_device(kDisplayControlAddress,
                                                    &s_display_control);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "display control lease acquisition failed: %s",
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
    if (s_display_control.is_active()) {
        const esp_err_t ret = shared_i2c_release_device(&s_display_control);
        if (first_error == ESP_OK && ret != ESP_OK) {
            first_error = ret;
        }
    }
    if (!s_display_control.is_active()) {
        /* A future codec lease may keep the bus alive; that is not an error. */
        const esp_err_t ret = shared_i2c_shutdown();
        if (first_error == ESP_OK && ret != ESP_OK &&
            ret != ESP_ERR_INVALID_STATE) {
            first_error = ret;
        }
    }
    return first_error;
}

esp_err_t pa_service_init()
{
    if (s_pa_initialized) {
        return ESP_OK;
    }
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << kPaControlGpioNumber;
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    esp_err_t ret = gpio_config(&config);
    if (ret != ESP_OK) {
        s_pa_initialized = false;
        s_pa_enabled = false;
        return ret;
    }
    ret = gpio_set_level(static_cast<gpio_num_t>(kPaControlGpioNumber), 0);
    if (ret != ESP_OK) {
        s_pa_initialized = false;
        s_pa_enabled = false;
        return ret;
    }
    s_pa_initialized = true;
    s_pa_enabled = false;
    return ESP_OK;
}

esp_err_t pa_service_enable()
{
    if (!s_pa_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t ret = gpio_set_level(
        static_cast<gpio_num_t>(kPaControlGpioNumber), 1);
    if (ret == ESP_OK) {
        s_pa_enabled = true;
    }
    return ret;
}

esp_err_t pa_service_disable()
{
    if (!s_pa_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t ret = gpio_set_level(
        static_cast<gpio_num_t>(kPaControlGpioNumber), 0);
    if (ret == ESP_OK) {
        s_pa_enabled = false;
    }
    return ret;
}

esp_err_t pa_service_shutdown()
{
    if (!s_pa_initialized) {
        return ESP_OK;
    }
    const esp_err_t ret = gpio_set_level(
        static_cast<gpio_num_t>(kPaControlGpioNumber), 0);
    if (ret != ESP_OK) {
        return ret;
    }
    s_pa_enabled = false;
    s_pa_initialized = false;
    return ESP_OK;
}

bool pa_service_is_initialized()
{
    return s_pa_initialized;
}

bool pa_service_is_enabled()
{
    return s_pa_enabled;
}

} // namespace p4_nano_board
