/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_pie_preemption/p4_nano_pie_preemption.hpp"

#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "p4_nano_display/p4_nano_display_exact2x.hpp"

namespace {

constexpr std::size_t kSourceWidth = 400U;
constexpr std::size_t kSourceHeight = 128U;
constexpr std::size_t kDestinationWidth = 800U;
constexpr std::size_t kDestinationHeight = 256U;
constexpr std::size_t kSourceBytes =
    kSourceWidth * kSourceHeight * sizeof(std::uint16_t);
constexpr std::size_t kDestinationBytes =
    kDestinationWidth * kDestinationHeight * sizeof(std::uint16_t);
constexpr std::size_t kDestinationStrideBytes = kDestinationWidth *
                                                 sizeof(std::uint16_t);
constexpr std::size_t kGroupedIterationsPerRow = kSourceWidth / 16U;
constexpr std::size_t kControlIterations = 4U;
constexpr std::size_t kStressIterations = 512U;
constexpr std::size_t kMinimumHandoffs = (kStressIterations * 3U) / 4U;
/* The grouped 400x128 T128 transfer performs 409,600 bytes of 64-byte
 * PSRAM stores.  A 250-us one-shot is deliberately short relative to that
 * transfer on the P4-NANO fixture, while remaining long enough for the timer
 * task to wake after the leaf helper has entered.  This is a correctness
 * trigger only; no timing result is collected. */
constexpr std::uint64_t kIntentionalPreemptionDelayUs = 250U;
constexpr TickType_t kTaskWaitTicks = pdMS_TO_TICKS(100U);
constexpr std::size_t kCleanupWaitAttempts = 100U;
constexpr std::size_t kRequiredAlignment = 64U;

static_assert(kSourceWidth % 16U == 0U);
static_assert(kGroupedIterationsPerRow == 25U);
static_assert(kDestinationStrideBytes % kRequiredAlignment == 0U);
static_assert(kTaskWaitTicks > 0);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

struct HarnessState final {
    TaskHandle_t high_task = nullptr;
    esp_timer_handle_t wake_timer = nullptr;
    SemaphoreHandle_t high_started = nullptr;
    SemaphoreHandle_t high_handled = nullptr;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> helper_active{false};
    std::atomic<bool> high_done{false};
    std::atomic<bool> timer_callback_failed{false};
    std::atomic<std::uint32_t> trigger_count{0U};
    std::atomic<std::uint32_t> timer_wake_count{0U};
    std::atomic<std::uint32_t> high_wake_count{0U};
    std::atomic<std::uint32_t> high_pie_count{0U};
    std::atomic<std::uint32_t> helper_active_handoff_count{0U};
    std::atomic<std::uint32_t> low_core{UINT32_MAX};
    std::atomic<std::uint32_t> low_priority{0U};
    std::atomic<std::uint32_t> high_core{UINT32_MAX};
    std::atomic<std::uint32_t> high_priority{0U};
    alignas(16) std::uint8_t clobber_source[16]{};
    alignas(16) std::uint8_t clobber_output[32]{};
};

void timer_callback(void *arg)
{
    auto *state = static_cast<HarnessState *>(arg);
    if (state == nullptr || state->high_task == nullptr) {
        if (state != nullptr) {
            state->timer_callback_failed.store(true,
                                               std::memory_order_release);
        }
        return;
    }
    state->timer_wake_count.fetch_add(1U, std::memory_order_relaxed);
    if (xTaskNotifyGive(state->high_task) != pdPASS) {
        state->timer_callback_failed.store(true, std::memory_order_release);
    }
}

void high_priority_task(void *arg)
{
    auto *state = static_cast<HarnessState *>(arg);
    if (state == nullptr) {
        vTaskDelete(nullptr);
        return;
    }

    state->high_core.store(static_cast<std::uint32_t>(xPortGetCoreID()),
                           std::memory_order_release);
    state->high_priority.store(
        static_cast<std::uint32_t>(uxTaskPriorityGet(nullptr)),
        std::memory_order_release);
    (void)xSemaphoreGive(state->high_started);

    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 0U) {
            continue;
        }
        if (state->stop_requested.load(std::memory_order_acquire)) {
            break;
        }

        state->high_wake_count.fetch_add(1U, std::memory_order_relaxed);
        const bool helper_was_active =
            state->helper_active.load(std::memory_order_acquire);
        if (helper_was_active) {
            state->helper_active_handoff_count.fetch_add(
                1U, std::memory_order_relaxed);
        }
        p4_nano_display::exact2x_pie_preemption_clobber_q0_q1(
            state->clobber_source, state->clobber_output);
        state->high_pie_count.fetch_add(1U, std::memory_order_relaxed);
        (void)xSemaphoreGive(state->high_handled);
    }

    state->high_done.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}

void fill_source(std::uint16_t *source)
{
    for (std::size_t index = 0U; index < kSourceWidth * kSourceHeight;
         ++index) {
        const std::uint16_t x = static_cast<std::uint16_t>(index % kSourceWidth);
        const std::uint16_t y = static_cast<std::uint16_t>(index / kSourceWidth);
        source[index] = static_cast<std::uint16_t>(
            ((x * 37U + y * 19U + 0x1357U) ^ (x << 3U)) & 0xffffU);
    }
}

void drain_binary_semaphore(SemaphoreHandle_t semaphore)
{
    (void)xSemaphoreTake(semaphore, 0U);
}

bool aligned_and_expected(const void *pointer, std::size_t alignment,
                          bool internal, bool external)
{
    if (pointer == nullptr ||
        (reinterpret_cast<std::uintptr_t>(pointer) % alignment) != 0U) {
        return false;
    }
    return (!internal || esp_ptr_internal(pointer)) &&
           (!external || esp_ptr_external_ram(pointer));
}

bool validate_output(const std::uint8_t *candidate,
                     const std::uint8_t *golden,
                     std::size_t iteration,
                     const HarnessState &state)
{
    if (std::memcmp(candidate, golden, kDestinationBytes) == 0) {
        return true;
    }
    std::printf("P4_NANO_PIE_PREEMPT_MISMATCH iteration=%zu triggers=%" PRIu32
                " high_task_wakes=%" PRIu32 " high_task_pie_calls=%" PRIu32
                " helper_active_handoffs=%" PRIu32 " result=FAIL\n",
                iteration,
                state.trigger_count.load(std::memory_order_relaxed),
                state.high_wake_count.load(std::memory_order_relaxed),
                state.high_pie_count.load(std::memory_order_relaxed),
                state.helper_active_handoff_count.load(
                    std::memory_order_relaxed));
    return false;
}

void stop_and_delete_timer(HarnessState *state, bool *cleanup_ok)
{
    if (state->wake_timer == nullptr) {
        return;
    }
    if (esp_timer_is_active(state->wake_timer)) {
        if (esp_timer_stop(state->wake_timer) != ESP_OK) {
            *cleanup_ok = false;
        }
    }
    if (esp_timer_delete(state->wake_timer) != ESP_OK) {
        *cleanup_ok = false;
    }
    state->wake_timer = nullptr;
}

void stop_and_join_high_task(HarnessState *state, bool *cleanup_ok)
{
    if (state->high_task == nullptr) {
        return;
    }
    state->stop_requested.store(true, std::memory_order_release);
    if (xTaskNotifyGive(state->high_task) != pdPASS) {
        *cleanup_ok = false;
    }
    for (std::size_t attempt = 0U;
         attempt < kCleanupWaitAttempts &&
         !state->high_done.load(std::memory_order_acquire);
         ++attempt) {
        vTaskDelay(1U);
    }
    if (!state->high_done.load(std::memory_order_acquire)) {
        *cleanup_ok = false;
    }
    state->high_task = nullptr;
}

esp_err_t fail_run(HarnessState *state, std::uint16_t *source,
                   std::uint8_t *candidate, std::uint8_t *golden,
                   const char *reason)
{
    bool cleanup_ok = true;
    stop_and_delete_timer(state, &cleanup_ok);
    stop_and_join_high_task(state, &cleanup_ok);
    heap_caps_free(source);
    heap_caps_free(candidate);
    heap_caps_free(golden);
    std::printf("P4_NANO_PIE_PREEMPT_CLEANUP timer=%s high_task=%s buffers=freed"
                " result=%s reason=%s\n",
                state->wake_timer == nullptr ? "stopped" : "active",
                state->high_task == nullptr ? "stopped" : "active",
                cleanup_ok ? "PASS" : "FAIL", reason);
    std::printf("P4_NANO_PIE_PREEMPT_RESULT=FAIL\n");
    return ESP_FAIL;
}

} // namespace

namespace p4_nano_pie_preemption {

esp_err_t run()
{
    HarnessState state{};
    state.low_core.store(static_cast<std::uint32_t>(xPortGetCoreID()),
                         std::memory_order_release);
    state.low_priority.store(
        static_cast<std::uint32_t>(uxTaskPriorityGet(nullptr)),
        std::memory_order_release);

    const UBaseType_t low_priority = uxTaskPriorityGet(nullptr);
    if (low_priority + 1U >= configMAX_PRIORITIES) {
        std::printf("P4_NANO_PIE_PREEMPT_RESULT=FAIL reason=priority_headroom\n");
        return ESP_ERR_INVALID_STATE;
    }
    const UBaseType_t high_priority = low_priority + 1U;
    const BaseType_t low_core = xPortGetCoreID();
    std::printf("P4_NANO_PIE_PREEMPT_CONFIG helper=grouped64 qr_set=q0,q1,q2,q4"
                " q3=forbidden special_state=none same_core=1"
                " high_priority_gt_low=1 stress_iterations=%zu"
                " intentional_preemption_delay_us=%" PRIu64
                " low_core=%" PRIu32 " low_priority=%" PRIu32 "\n",
                kStressIterations, kIntentionalPreemptionDelayUs,
                state.low_core.load(std::memory_order_relaxed),
                state.low_priority.load(std::memory_order_relaxed));

    auto *source = static_cast<std::uint16_t *>(heap_caps_aligned_alloc(
        kRequiredAlignment, kSourceBytes,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    auto *candidate = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kRequiredAlignment, kDestinationBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *golden = static_cast<std::uint8_t *>(heap_caps_aligned_alloc(
        kRequiredAlignment, kDestinationBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!aligned_and_expected(source, kRequiredAlignment, true, false) ||
        !aligned_and_expected(candidate, kRequiredAlignment, false, true) ||
        !aligned_and_expected(golden, kRequiredAlignment, false, true)) {
        if (source != nullptr) {
            heap_caps_free(source);
        }
        if (candidate != nullptr) {
            heap_caps_free(candidate);
        }
        if (golden != nullptr) {
            heap_caps_free(golden);
        }
        std::printf("P4_NANO_PIE_PREEMPT_RESULT=FAIL reason=allocation_or_alignment\n");
        return ESP_ERR_NO_MEM;
    }
    fill_source(source);
    for (std::size_t index = 0U; index < sizeof(state.clobber_source);
         ++index) {
        state.clobber_source[index] =
            static_cast<std::uint8_t>(0x31U + index * 7U);
    }
    std::memset(state.clobber_output, 0, sizeof(state.clobber_output));
    std::memset(candidate, 0, kDestinationBytes);
    std::memset(golden, 0, kDestinationBytes);
    if (!p4_nano_display::exact2x_scalar(source, kSourceBytes,
                                         reinterpret_cast<std::uint16_t *>(golden),
                                         kDestinationBytes)) {
        return fail_run(&state, source, candidate, golden, "scalar_golden");
    }

    StaticSemaphore_t high_started_storage{};
    StaticSemaphore_t high_handled_storage{};
    state.high_started = xSemaphoreCreateBinaryStatic(&high_started_storage);
    state.high_handled = xSemaphoreCreateBinaryStatic(&high_handled_storage);
    if (state.high_started == nullptr || state.high_handled == nullptr) {
        return fail_run(&state, source, candidate, golden, "semaphore_create");
    }

    const esp_timer_create_args_t timer_args{
        .callback = timer_callback,
        .arg = &state,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "p10k-preempt",
        .skip_unhandled_events = false,
    };
    if (esp_timer_create(&timer_args, &state.wake_timer) != ESP_OK) {
        return fail_run(&state, source, candidate, golden, "timer_create");
    }
    if (xTaskCreatePinnedToCore(high_priority_task, "p10k_pie_high",
                                4096U, &state, high_priority,
                                &state.high_task, low_core) != pdPASS) {
        return fail_run(&state, source, candidate, golden, "task_create");
    }
    if (xSemaphoreTake(state.high_started, kTaskWaitTicks) != pdTRUE) {
        return fail_run(&state, source, candidate, golden, "task_start");
    }
    if (state.high_core.load(std::memory_order_acquire) !=
            state.low_core.load(std::memory_order_acquire) ||
        state.high_priority.load(std::memory_order_acquire) <=
            state.low_priority.load(std::memory_order_acquire)) {
        return fail_run(&state, source, candidate, golden, "task_contract");
    }
    std::printf("P4_NANO_PIE_PREEMPT_TASKS low_core=%" PRIu32
                " low_priority=%" PRIu32 " high_core=%" PRIu32
                " high_priority=%" PRIu32 " same_core=1"
                " high_priority_gt_low=1\n",
                state.low_core.load(std::memory_order_relaxed),
                state.low_priority.load(std::memory_order_relaxed),
                state.high_core.load(std::memory_order_relaxed),
                state.high_priority.load(std::memory_order_relaxed));

    for (std::size_t iteration = 0U; iteration < kControlIterations;
         ++iteration) {
        std::memset(candidate, 0, kDestinationBytes);
        p4_nano_display::exact2x_pie_tile128_grouped64_aligned(
            source, reinterpret_cast<std::uint16_t *>(candidate));
        if (!validate_output(candidate, golden, iteration, state)) {
            return fail_run(&state, source, candidate, golden, "control_mismatch");
        }
    }
    std::printf("P4_NANO_PIE_PREEMPT_CONTROL iterations=%zu byte_exact=1 result=PASS\n",
                kControlIterations);

    for (std::size_t iteration = 0U; iteration < kStressIterations;
         ++iteration) {
        drain_binary_semaphore(state.high_handled);
        std::memset(candidate, 0, kDestinationBytes);
        state.helper_active.store(true, std::memory_order_release);
        state.trigger_count.fetch_add(1U, std::memory_order_relaxed);
        if (esp_timer_start_once(state.wake_timer,
                                 kIntentionalPreemptionDelayUs) != ESP_OK) {
            state.helper_active.store(false, std::memory_order_release);
            return fail_run(&state, source, candidate, golden, "timer_start");
        }
        p4_nano_display::exact2x_pie_tile128_grouped64_aligned(
            source, reinterpret_cast<std::uint16_t *>(candidate));
        state.helper_active.store(false, std::memory_order_release);
        if (xSemaphoreTake(state.high_handled, kTaskWaitTicks) != pdTRUE) {
            return fail_run(&state, source, candidate, golden, "high_task_wait");
        }
        if (state.timer_callback_failed.load(std::memory_order_acquire)) {
            return fail_run(&state, source, candidate, golden,
                            "timer_callback");
        }
        if (!validate_output(candidate, golden, iteration, state)) {
            return fail_run(&state, source, candidate, golden, "stress_mismatch");
        }
    }

    const std::uint32_t handoffs =
        state.helper_active_handoff_count.load(std::memory_order_acquire);
    if (handoffs < kMinimumHandoffs) {
        return fail_run(&state, source, candidate, golden,
                        "insufficient_confirmed_handoffs");
    }
    std::printf("P4_NANO_PIE_PREEMPT_STRESS iterations=%zu triggers=%" PRIu32
                " high_task_wakes=%" PRIu32 " high_task_pie_calls=%" PRIu32
                " helper_active_handoffs=%" PRIu32 " mismatches=0 result=PASS\n",
                kStressIterations,
                state.trigger_count.load(std::memory_order_relaxed),
                state.high_wake_count.load(std::memory_order_relaxed),
                state.high_pie_count.load(std::memory_order_relaxed), handoffs);

    bool cleanup_ok = true;
    stop_and_delete_timer(&state, &cleanup_ok);
    stop_and_join_high_task(&state, &cleanup_ok);
    heap_caps_free(source);
    heap_caps_free(candidate);
    heap_caps_free(golden);
    std::printf("P4_NANO_PIE_PREEMPT_CLEANUP timer=%s high_task=%s buffers=freed"
                " result=%s\n",
                state.wake_timer == nullptr ? "stopped" : "active",
                state.high_task == nullptr ? "stopped" : "active",
                cleanup_ok ? "PASS" : "FAIL");
    if (!cleanup_ok) {
        std::printf("P4_NANO_PIE_PREEMPT_RESULT=FAIL reason=cleanup\n");
        return ESP_FAIL;
    }
    std::printf("P4_NANO_PIE_PREEMPT_RESULT=PASS\n");
    return ESP_OK;
}

} // namespace p4_nano_pie_preemption
