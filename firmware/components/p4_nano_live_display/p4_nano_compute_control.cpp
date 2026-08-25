/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_live_display/p4_nano_compute_control.hpp"

#include <atomic>

#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace p4_nano_compute_control {
namespace {

constexpr BaseType_t kTaskCore = 1;
constexpr UBaseType_t kTaskPriority = tskIDLE_PRIORITY + 3U;
constexpr TickType_t kControlTimeoutTicks = pdMS_TO_TICKS(5000U) == 0U
                                                ? 1U : pdMS_TO_TICKS(5000U);
constexpr std::uint32_t kSeed = 0x13579bdfU;

struct Runtime {
    StaticTask_t task_buffer{};
    StackType_t task_stack[kTaskStackWords]{};
    StaticSemaphore_t ready_buffer{};
    StaticSemaphore_t done_buffer{};
    SemaphoreHandle_t ready = nullptr;
    SemaphoreHandle_t done = nullptr;
    TaskHandle_t task = nullptr;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> running{false};
    Calibration calibration{};
    Health health{};
    std::uint32_t state = kSeed;
};

/* This object, including task stack/TCB and all control state, is internal
 * static DRAM.  No P7 task or control buffer uses heap allocation. */
DRAM_ATTR Runtime s_runtime{};

void compute_task(void *)
{
    s_runtime.health.core = static_cast<std::uint32_t>(xPortGetCoreID());
    s_runtime.health.priority =
        static_cast<std::uint32_t>(uxTaskPriorityGet(nullptr));
    const std::uint64_t calibration_start =
        static_cast<std::uint64_t>(esp_timer_get_time());
    const std::uint32_t calibration_state =
        run_chunk(kSeed, kCalibrationIterations);
    const std::uint64_t calibration_elapsed =
        static_cast<std::uint64_t>(esp_timer_get_time()) - calibration_start;
    s_runtime.calibration.calibration_iterations = kCalibrationIterations;
    s_runtime.calibration.calibration_elapsed_us = calibration_elapsed;
    s_runtime.calibration.valid = derive_chunk_iterations(
        calibration_elapsed, kCalibrationIterations, kTargetIntervalUs,
        &s_runtime.calibration.chunk_iterations);
    s_runtime.state = calibration_state;
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
    while (!s_runtime.stop_requested.load(std::memory_order_acquire)) {
        s_runtime.state = run_chunk(
            s_runtime.state, s_runtime.calibration.chunk_iterations);
        ++s_runtime.health.chunks;
        s_runtime.health.iterations +=
            s_runtime.calibration.chunk_iterations;
        s_runtime.health.checksum ^= s_runtime.state;
        /* A fixed 250 ms compute interval gives a 20x margin to the 5 s
         * TWDT timeout, while vTaskDelay(1) lets CPU1 idle run. */
        if ((s_runtime.health.chunks %
             s_runtime.calibration.chunks_per_relief) == 0U) {
            ++s_runtime.health.relief_count;
            vTaskDelay(1);
        }
    }
    s_runtime.health.checksum ^= s_runtime.state;
    s_runtime.health.stack_high_water_words =
        static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
    s_runtime.health.clean_stop = true;
    s_runtime.running.store(false, std::memory_order_release);
    (void)xSemaphoreGive(s_runtime.done);
    vTaskDelete(nullptr);
}

} // namespace

IRAM_ATTR std::uint32_t run_chunk(std::uint32_t state,
                                  std::uint32_t iterations)
{
    return recurrence(state, iterations);
}

bool start_and_calibrate()
{
    s_runtime.ready = nullptr;
    s_runtime.done = nullptr;
    s_runtime.task = nullptr;
    s_runtime.stop_requested.store(false, std::memory_order_relaxed);
    s_runtime.running.store(false, std::memory_order_relaxed);
    s_runtime.calibration = Calibration{};
    s_runtime.health = Health{};
    s_runtime.state = kSeed;
    s_runtime.ready = xSemaphoreCreateBinaryStatic(&s_runtime.ready_buffer);
    s_runtime.done = xSemaphoreCreateBinaryStatic(&s_runtime.done_buffer);
    if (s_runtime.ready == nullptr || s_runtime.done == nullptr) {
        return false;
    }
    s_runtime.task = xTaskCreateStaticPinnedToCore(
        compute_task, "p7_compute", kTaskStackWords, nullptr,
        kTaskPriority, s_runtime.task_stack, &s_runtime.task_buffer,
        kTaskCore);
    if (s_runtime.task == nullptr) {
        return false;
    }
    if (xSemaphoreTake(s_runtime.ready, kControlTimeoutTicks) != pdTRUE) {
        (void)stop();
        return false;
    }
    if (!s_runtime.calibration.valid || !s_runtime.health.ready) {
        if (s_runtime.task != nullptr) {
            (void)stop();
        } else {
            (void)xSemaphoreTake(s_runtime.done, kControlTimeoutTicks);
        }
        return false;
    }
    return s_runtime.calibration.valid && s_runtime.health.ready &&
           s_runtime.health.core == static_cast<std::uint32_t>(kTaskCore) &&
           s_runtime.health.priority == kTaskPriority &&
           esp_ptr_internal(s_runtime.task_stack) &&
           esp_ptr_internal(&s_runtime.task_buffer);
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
            /* The task may still be blocked on its notification or may be
             * between notification consumption and the running publication.
             * Request a bounded cleanup so a failed begin cannot leak a
             * permanently-ready CPU1 task into benchmark teardown. */
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
    return s_runtime.health.clean_stop && !s_runtime.running.load(
        std::memory_order_acquire);
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

} // namespace p4_nano_compute_control
