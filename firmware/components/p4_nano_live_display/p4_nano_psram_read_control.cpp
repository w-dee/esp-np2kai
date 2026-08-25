/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_live_display/p4_nano_psram_read_control.hpp"

#include <atomic>

#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace p4_nano_psram_read_control {
namespace {

constexpr BaseType_t kTaskCore = 1;
constexpr UBaseType_t kTaskPriority = tskIDLE_PRIORITY + kTaskPriorityOffset;
constexpr TickType_t kControlTimeoutTicks = pdMS_TO_TICKS(5000U) == 0U
                                                ? 1U : pdMS_TO_TICKS(5000U);

struct Runtime {
    StaticTask_t task_buffer{};
    StackType_t task_stack[kTaskStackWords]{};
    StaticSemaphore_t ready_buffer{};
    StaticSemaphore_t done_buffer{};
    SemaphoreHandle_t ready = nullptr;
    SemaphoreHandle_t done = nullptr;
    TaskHandle_t task = nullptr;
    const std::uint8_t *buffer = nullptr;
    std::uint32_t expected_checksum = 0U;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> running{false};
    Calibration calibration{};
    Health health{};
};

/* Task stack, TCB, semaphores and all control state stay in internal DRAM. */
DRAM_ATTR Runtime s_runtime{};

void read_task(void *)
{
    s_runtime.health.core = static_cast<std::uint32_t>(xPortGetCoreID());
    s_runtime.health.priority =
        static_cast<std::uint32_t>(uxTaskPriorityGet(nullptr));

    const std::uint64_t calibration_start =
        static_cast<std::uint64_t>(esp_timer_get_time());
    std::uint32_t calibration_checksum = 0U;
    for (std::uint32_t sweep = 0U; sweep < kCalibrationSweeps; ++sweep) {
        calibration_checksum ^= read_sweep(s_runtime.buffer);
    }
    const std::uint64_t calibration_elapsed =
        static_cast<std::uint64_t>(esp_timer_get_time()) - calibration_start;
    s_runtime.calibration.calibration_sweeps = kCalibrationSweeps;
    s_runtime.calibration.calibration_elapsed_us = calibration_elapsed;
    s_runtime.calibration.valid = derive_sweeps_per_relief(
        calibration_elapsed, kCalibrationSweeps, kTargetIntervalUs,
        kMaxSweepsPerRelief, &s_runtime.calibration.sweeps_per_relief);
    if (calibration_checksum != s_runtime.expected_checksum) {
        s_runtime.health.checksum_valid = false;
    }
    if (!s_runtime.calibration.valid) {
        s_runtime.health.ready = false;
        s_runtime.health.clean_stop = true;
        (void)xSemaphoreGive(s_runtime.ready);
        s_runtime.task = nullptr;
        (void)xSemaphoreGive(s_runtime.done);
        vTaskDelete(nullptr);
        return;
    }

    s_runtime.health.ready = true;
    (void)xSemaphoreGive(s_runtime.ready);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    s_runtime.running.store(true, std::memory_order_release);
    s_runtime.health.running = true;
    while (!s_runtime.stop_requested.load(std::memory_order_acquire)) {
        for (std::uint32_t sweep = 0U;
             sweep < s_runtime.calibration.sweeps_per_relief; ++sweep) {
            const std::uint32_t checksum = read_sweep(s_runtime.buffer);
            s_runtime.health.last_sweep_checksum = checksum;
            s_runtime.health.rolling_checksum ^= checksum;
            ++s_runtime.health.sweeps;
            if (s_runtime.health.total_bytes >
                UINT64_MAX - static_cast<std::uint64_t>(kBufferBytes)) {
                s_runtime.health.checksum_valid = false;
                s_runtime.stop_requested.store(true, std::memory_order_release);
                break;
            }
            s_runtime.health.total_bytes += kBufferBytes;
            if (checksum != s_runtime.expected_checksum) {
                s_runtime.health.checksum_valid = false;
            }
        }
        if (s_runtime.stop_requested.load(std::memory_order_acquire)) {
            break;
        }
        ++s_runtime.health.relief_count;
        vTaskDelay(1);
    }
    s_runtime.health.stack_high_water_words =
        static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
    s_runtime.health.running = false;
    s_runtime.health.clean_stop = true;
    s_runtime.running.store(false, std::memory_order_release);
    (void)xSemaphoreGive(s_runtime.done);
    vTaskDelete(nullptr);
}

} // namespace

__attribute__((noinline)) std::uint32_t read_sweep(
    const std::uint8_t *buffer) noexcept
{
    const auto *input = reinterpret_cast<const std::uint32_t *>(buffer);
    std::uint32_t a0 = initial_lane(0U);
    std::uint32_t a1 = initial_lane(1U);
    std::uint32_t a2 = initial_lane(2U);
    std::uint32_t a3 = initial_lane(3U);
    std::uint32_t a4 = initial_lane(4U);
    std::uint32_t a5 = initial_lane(5U);
    std::uint32_t a6 = initial_lane(6U);
    std::uint32_t a7 = initial_lane(7U);
    for (std::uint32_t index = 0U;
         index < static_cast<std::uint32_t>(kWordsPerSweep); index += 8U) {
        a0 = fold_lane(a0, input[index + 0U], index + 0U);
        a1 = fold_lane(a1, input[index + 1U], index + 1U);
        a2 = fold_lane(a2, input[index + 2U], index + 2U);
        a3 = fold_lane(a3, input[index + 3U], index + 3U);
        a4 = fold_lane(a4, input[index + 4U], index + 4U);
        a5 = fold_lane(a5, input[index + 5U], index + 5U);
        a6 = fold_lane(a6, input[index + 6U], index + 6U);
        a7 = fold_lane(a7, input[index + 7U], index + 7U);
    }
    return a0 ^ a1 ^ a2 ^ a3 ^ a4 ^ a5 ^ a6 ^ a7;
}

bool start_and_calibrate(const std::uint8_t *buffer,
                         std::uint32_t expected_checksum)
{
    if (buffer == nullptr) {
        return false;
    }
    s_runtime.ready = nullptr;
    s_runtime.done = nullptr;
    s_runtime.task = nullptr;
    s_runtime.buffer = buffer;
    s_runtime.expected_checksum = expected_checksum;
    s_runtime.stop_requested.store(false, std::memory_order_relaxed);
    s_runtime.running.store(false, std::memory_order_relaxed);
    s_runtime.calibration = Calibration{};
    s_runtime.health = Health{};
    s_runtime.ready = xSemaphoreCreateBinaryStatic(&s_runtime.ready_buffer);
    s_runtime.done = xSemaphoreCreateBinaryStatic(&s_runtime.done_buffer);
    if (s_runtime.ready == nullptr || s_runtime.done == nullptr) {
        return false;
    }
    s_runtime.task = xTaskCreateStaticPinnedToCore(
        read_task, "p8_psram", kTaskStackWords, nullptr, kTaskPriority,
        s_runtime.task_stack, &s_runtime.task_buffer, kTaskCore);
    if (s_runtime.task == nullptr) {
        return false;
    }
    if (xSemaphoreTake(s_runtime.ready, kControlTimeoutTicks) != pdTRUE) {
        (void)stop();
        return false;
    }
    if (!s_runtime.calibration.valid || !s_runtime.health.ready ||
        !s_runtime.health.checksum_valid) {
        if (s_runtime.task != nullptr) {
            (void)stop();
        } else {
            (void)xSemaphoreTake(s_runtime.done, kControlTimeoutTicks);
        }
        return false;
    }
    return s_runtime.health.core == static_cast<std::uint32_t>(kTaskCore) &&
           s_runtime.health.priority == kTaskPriority && stack_internal() &&
           tcb_internal() && state_internal();
}

bool begin()
{
    if (s_runtime.task == nullptr || !s_runtime.calibration.valid) {
        return false;
    }
    s_runtime.stop_requested.store(false, std::memory_order_release);
    xTaskNotifyGive(s_runtime.task);
    const TickType_t wait_start = xTaskGetTickCount();
    while (!s_runtime.running.load(std::memory_order_acquire)) {
        if ((xTaskGetTickCount() - wait_start) >= kControlTimeoutTicks) {
            s_runtime.stop_requested.store(true, std::memory_order_release);
            xTaskNotifyGive(s_runtime.task);
            if (xSemaphoreTake(s_runtime.done, kControlTimeoutTicks) ==
                pdTRUE) {
                s_runtime.task = nullptr;
            }
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

bool stop()
{
    if (s_runtime.task == nullptr) {
        return false;
    }
    s_runtime.stop_requested.store(true, std::memory_order_release);
    xTaskNotifyGive(s_runtime.task);
    if (xSemaphoreTake(s_runtime.done, kControlTimeoutTicks) != pdTRUE) {
        return false;
    }
    s_runtime.task = nullptr;
    return s_runtime.health.clean_stop &&
           !s_runtime.running.load(std::memory_order_acquire);
}

bool stack_internal()
{
    return esp_ptr_internal(s_runtime.task_stack);
}

bool tcb_internal()
{
    return esp_ptr_internal(&s_runtime.task_buffer);
}

bool state_internal()
{
    return esp_ptr_internal(&s_runtime);
}

std::uint32_t stack_bytes()
{
    return static_cast<std::uint32_t>(sizeof(s_runtime.task_stack));
}

std::uint32_t tcb_bytes()
{
    return static_cast<std::uint32_t>(sizeof(s_runtime.task_buffer));
}

std::uint32_t state_bytes()
{
    return static_cast<std::uint32_t>(sizeof(s_runtime) -
                                      sizeof(s_runtime.task_stack) -
                                      sizeof(s_runtime.task_buffer));
}

std::uint32_t static_bytes()
{
    return static_cast<std::uint32_t>(sizeof(s_runtime));
}

const Calibration &calibration()
{
    return s_runtime.calibration;
}

const Health &health()
{
    return s_runtime.health;
}

} // namespace p4_nano_psram_read_control
