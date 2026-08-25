/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#else
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#endif

namespace p4_nano_psram_read_control {

constexpr std::size_t kBufferBytes = 4U * 1024U * 1024U;
constexpr std::size_t kAlignmentBytes = 64U;
constexpr std::size_t kWordsPerSweep = kBufferBytes / sizeof(std::uint32_t);
constexpr std::uint32_t kTaskStackWords = 1024U;
constexpr std::uint32_t kTaskPriorityOffset = 3U;
constexpr std::uint32_t kTargetIntervalUs = 250000U;
constexpr std::uint32_t kCalibrationSweeps = 1U;
constexpr std::uint32_t kMaxSweepsPerRelief = 128U;

constexpr std::uint32_t rotate_left(std::uint32_t value,
                                    std::uint32_t amount) noexcept
{
    return (value << amount) | (value >> (32U - amount));
}

constexpr std::uint32_t pattern_word(std::uint32_t index) noexcept
{
    const std::uint32_t mixed = index * 0x9e3779b9U;
    return 0x6d2b79f5U ^ mixed ^ rotate_left(index * 0x85ebca6bU, 11U);
}

constexpr std::uint32_t initial_lane(std::uint32_t lane) noexcept
{
    constexpr std::uint32_t kInitial[] = {
        0x13579bdfU, 0x2468ace0U, 0x9e3779b9U, 0x7f4a7c15U,
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    };
    return kInitial[lane & 7U];
}

constexpr std::uint32_t fold_lane(std::uint32_t accumulator,
                                  std::uint32_t value,
                                  std::uint32_t word_index) noexcept
{
    accumulator ^= value + 0x9e3779b9U + word_index;
    accumulator = rotate_left(accumulator, 5U);
    return accumulator + (0x7f4a7c15U ^ (word_index * 0x85ebca6bU));
}

constexpr std::uint32_t expected_sweep_checksum() noexcept
{
    std::uint32_t lanes[8] = {
        initial_lane(0U), initial_lane(1U), initial_lane(2U), initial_lane(3U),
        initial_lane(4U), initial_lane(5U), initial_lane(6U), initial_lane(7U),
    };
    for (std::uint32_t index = 0U;
         index < static_cast<std::uint32_t>(kWordsPerSweep); index += 8U) {
        for (std::uint32_t lane = 0U; lane < 8U; ++lane) {
            lanes[lane] = fold_lane(lanes[lane],
                                    pattern_word(index + lane), index + lane);
        }
    }
    return lanes[0] ^ lanes[1] ^ lanes[2] ^ lanes[3] ^ lanes[4] ^ lanes[5] ^
           lanes[6] ^ lanes[7];
}

/* Precomputed from expected_sweep_checksum() for target-side comparisons.
 * Keeping the constexpr reference implementation above makes the host
 * contract test independent, while avoiding a million-iteration constexpr
 * expansion in the large display benchmark translation unit. */
constexpr std::uint32_t kExpectedSweepChecksum = 0xbee5190fU;

constexpr bool derive_sweeps_per_relief(
    std::uint64_t elapsed_us, std::uint32_t calibration_sweeps,
    std::uint32_t target_interval_us, std::uint32_t max_sweeps,
    std::uint32_t *result) noexcept
{
    if (result == nullptr || elapsed_us == 0U || calibration_sweeps == 0U ||
        target_interval_us == 0U || max_sweeps == 0U) {
        return false;
    }
    const std::uint64_t scaled =
        static_cast<std::uint64_t>(calibration_sweeps) * target_interval_us;
    if (scaled / target_interval_us != calibration_sweeps ||
        elapsed_us - 1U >
            std::numeric_limits<std::uint64_t>::max() - scaled) {
        return false;
    }
    const std::uint64_t derived = (scaled + elapsed_us - 1U) / elapsed_us;
    if (derived == 0U || derived > max_sweeps) {
        return false;
    }
    *result = static_cast<std::uint32_t>(derived);
    return true;
}

constexpr bool payload_mib_per_second(std::uint64_t total_bytes,
                                      std::uint64_t wall_us,
                                      double *result) noexcept
{
    if (result == nullptr || wall_us == 0U) {
        return false;
    }
    *result = (static_cast<double>(total_bytes) * 1000000.0) /
              static_cast<double>(wall_us) / (1024.0 * 1024.0);
    return true;
}

struct Calibration {
    std::uint32_t calibration_sweeps = 0U;
    std::uint64_t calibration_elapsed_us = 0U;
    std::uint32_t sweeps_per_relief = 0U;
    std::uint32_t target_interval_us = kTargetIntervalUs;
    bool valid = false;
};

struct Health {
    std::uint32_t core = 0U;
    std::uint32_t priority = 0U;
    std::uint32_t stack_high_water_words = 0U;
    std::uint64_t sweeps = 0U;
    std::uint64_t total_bytes = 0U;
    std::uint32_t relief_count = 0U;
    std::uint32_t last_sweep_checksum = 0U;
    std::uint32_t rolling_checksum = 0U;
    bool ready = false;
    bool running = false;
    bool clean_stop = false;
    bool checksum_valid = true;
};

/* Dedicated no-helper-call hot kernel; inspect this symbol in the P8 map. */
IRAM_ATTR std::uint32_t read_sweep(const std::uint8_t *buffer) noexcept;

bool start_and_calibrate(const std::uint8_t *buffer,
                         std::uint32_t expected_checksum);
bool begin();
bool stop();
bool stack_internal();
bool tcb_internal();
bool state_internal();
std::uint32_t stack_bytes();
std::uint32_t tcb_bytes();
std::uint32_t state_bytes();
std::uint32_t static_bytes();
const Calibration &calibration();
const Health &health();

} // namespace p4_nano_psram_read_control
