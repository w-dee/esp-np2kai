/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#else
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#endif

namespace p4_nano_compute_control {

constexpr std::uint32_t kCalibrationIterations = 100000U;
constexpr std::uint32_t kTargetIntervalUs = 250000U;
constexpr std::uint32_t kMaxChunkIterations = 200000000U;
/* RISC-V ESP-IDF uses one byte per StackType_t; 1024 bytes leaves room for
 * the FreeRTOS notification/semaphore call frames without approaching the
 * existing 32 KiB NP2 runner stack. */
constexpr std::uint32_t kTaskStackWords = 1024U;

struct Calibration {
    std::uint32_t calibration_iterations = 0U;
    std::uint64_t calibration_elapsed_us = 0U;
    std::uint32_t chunk_iterations = 0U;
    std::uint32_t chunks_per_relief = 1U;
    std::uint32_t target_interval_us = kTargetIntervalUs;
    bool valid = false;
};

struct Health {
    std::uint32_t core = 0U;
    std::uint32_t priority = 0U;
    std::uint32_t stack_words = kTaskStackWords;
    std::uint32_t stack_high_water_words = 0U;
    std::uint64_t chunks = 0U;
    std::uint64_t iterations = 0U;
    std::uint32_t relief_count = 0U;
    std::uint32_t checksum = 0U;
    bool ready = false;
    bool clean_stop = false;
};

constexpr std::uint32_t recurrence(std::uint32_t state,
                                   std::uint32_t iterations)
{
    for (std::uint32_t index = 0U; index < iterations; ++index) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        state += 0x9e3779b9U + index;
        state = (state << 7U) | (state >> 25U);
    }
    return state;
}

constexpr bool derive_chunk_iterations(std::uint64_t elapsed_us,
                                       std::uint32_t calibration_iterations,
                                       std::uint32_t target_interval_us,
                                       std::uint32_t *result)
{
    if (result == nullptr || elapsed_us == 0U ||
        calibration_iterations == 0U || target_interval_us == 0U) {
        return false;
    }
    const std::uint64_t scaled =
        static_cast<std::uint64_t>(calibration_iterations) *
        static_cast<std::uint64_t>(target_interval_us);
    if (scaled / target_interval_us != calibration_iterations) {
        return false;
    }
    const std::uint64_t derived = (scaled + elapsed_us - 1U) / elapsed_us;
    if (derived == 0U || derived > kMaxChunkIterations) {
        return false;
    }
    *result = static_cast<std::uint32_t>(derived);
    return true;
}

/* The implementation is benchmark-only and is never linked by production. */
bool start_and_calibrate();
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

/* Kept separate so target disassembly can be audited directly. */
std::uint32_t run_chunk(std::uint32_t state, std::uint32_t iterations);

} // namespace p4_nano_compute_control
