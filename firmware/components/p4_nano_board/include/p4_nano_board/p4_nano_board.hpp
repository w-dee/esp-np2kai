#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "p4_nano_board/shared_i2c_lease_model.hpp"

namespace p4_nano_board {

inline constexpr std::uint8_t kDisplayControlAddress = 0x45;
inline constexpr std::uint8_t kFutureCodecDeviceAddress = 0x18;
inline constexpr std::uint8_t kPanelControlRegister = 0x95;
inline constexpr std::uint8_t kBacklightRegister = 0x96;
inline constexpr std::uint8_t kBacklightOff = 0x00;
inline constexpr std::uint8_t kBacklightConservative = 0x40;
inline constexpr int kPaControlGpioNumber = 51;

/*
 * A lease owns one device handle only.  The board service retains the shared
 * GPIO7/GPIO8 bus until every lease has been released and shutdown is allowed.
 * Init/release/shutdown calls are serialized by the board/display control task;
 * no audio-path lock is required by this lifecycle API.
 */
using I2cDeviceLease = SharedI2cLeaseModel::Lease;

esp_err_t shared_i2c_acquire_device(std::uint8_t address,
                                    I2cDeviceLease *lease);
esp_err_t shared_i2c_release_device(I2cDeviceLease *lease);
esp_err_t shared_i2c_device_transmit(const I2cDeviceLease *lease,
                                     const void *data, std::size_t length,
                                     int timeout_ms);
esp_err_t shared_i2c_device_transmit_receive(const I2cDeviceLease *lease,
                                             const void *write_data,
                                             std::size_t write_length,
                                             void *read_data,
                                             std::size_t read_length,
                                             int timeout_ms);
esp_err_t shared_i2c_shutdown();
bool shared_i2c_is_initialized();

/*
 * The board service is the sole production owner of P4-NANO GPIO7/GPIO8.
 * Device-specific clients never install or delete an I2C driver themselves.
 */
esp_err_t display_control_init();
esp_err_t display_control_panel_power_on();
esp_err_t display_control_write(std::uint8_t reg, std::uint8_t value);
esp_err_t display_backlight_set(std::uint8_t value);
esp_err_t display_control_safe_off();
esp_err_t display_control_deinit();
bool display_control_is_initialized();

/* Board-owned PA control.  GPIO51 LOW is the safe amplifier-disabled state. */
esp_err_t pa_service_init();
esp_err_t pa_service_enable();
esp_err_t pa_service_disable();
esp_err_t pa_service_shutdown();
bool pa_service_is_initialized();
bool pa_service_is_enabled();

} // namespace p4_nano_board
