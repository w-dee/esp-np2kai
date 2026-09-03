/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_audio86_physical_sink/p4_nano_audio86_physical_sink_idf.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_timer.h"

namespace p4_nano_audio86_physical {
namespace {

struct Event {
    uint32_t sequence;
    uint32_t generation;
    const char *operation;
    int result;
    size_t bytes;
};

struct FakeBackend {
    TaskHandle_t *waiter_slot;
    p4_nano_audio86_callback_gate *gate;
    uint32_t generation;
    Event events[32];
    uint32_t event_count;
    uint32_t i2s;
    uint32_t callbacks;
    uint32_t i2c;
    uint32_t codec;
    uint32_t pa_high;
    uint32_t released;
    uint32_t destroyed;
};

FakeBackend s_backend{};

void record(const char *operation, int result = 0, size_t bytes = 0U)
{
    if (s_backend.event_count >= sizeof(s_backend.events) /
                                 sizeof(s_backend.events[0])) return;
    Event &event = s_backend.events[s_backend.event_count];
    event.sequence = s_backend.event_count++;
    event.generation = s_backend.generation;
    event.operation = operation;
    event.result = result;
    event.bytes = bytes;
}

int prepare(void *, p4_nano_audio86_callback_gate *gate, uint32_t generation)
{
    s_backend.gate = gate;
    s_backend.generation = generation;
    record("PREPARE_BEGIN");
    if (P4_NANO_AUDIO86_PHYSICAL_LIFECYCLE_TEST_PROFILE == 1) {
        record("PREPARE_FAIL", -1);
        return -1;
    }
    s_backend.i2s = 1U;
    record("I2S_CREATE");
    if (P4_NANO_AUDIO86_PHYSICAL_LIFECYCLE_TEST_PROFILE == 2) {
        record("PREPARE_FAIL", -1);
        return -1;
    }
    s_backend.callbacks = 1U;
    record("CALLBACK_REGISTER");
    if (P4_NANO_AUDIO86_PHYSICAL_LIFECYCLE_TEST_PROFILE == 3) {
        record("PREPARE_FAIL", -1);
        return -1;
    }
    s_backend.i2c = 1U;
    s_backend.codec = 1U;
    s_backend.pa_high = 1U;
    record("I2C_ACQUIRE");
    record("CODEC_CONFIG");
    record("PA_HIGH");
    record("PREPARE_FAIL", -1);
    return -1;
}

enum p4_nano_audio86_physical_io_result preload(
    void *, const uint8_t *, size_t bytes, size_t *loaded)
{
    record("PRELOAD", 0, bytes);
    *loaded = bytes;
    return P4_NANO_AUDIO86_PHYSICAL_IO_OK;
}

int enable(void *) { record("ENABLE"); return 0; }

enum p4_nano_audio86_physical_io_result write(
    void *, const uint8_t *, size_t bytes, size_t *written, uint32_t)
{
    record("WRITE", 0, bytes);
    *written = bytes;
    return P4_NANO_AUDIO86_PHYSICAL_IO_OK;
}

int mute(void *)
{
    record("CODEC_MUTE");
    s_backend.codec = 0U;
    return 0;
}

int pa_low(void *)
{
    record("PA_LOW");
    s_backend.pa_high = 0U;
    return 0;
}

int disable(void *) { record("DISABLE"); return 0; }

int unregister_callbacks(void *)
{
    record("DELETE_BEGIN");
    s_backend.callbacks = 0U;
    s_backend.i2s = 0U;
    record("DELETE_END");
    return 0;
}

uint64_t now_ms(void *)
{
    const int64_t value = esp_timer_get_time();
    return value < 0 ? UINT64_MAX : static_cast<uint64_t>(value) / 1000U;
}

void wait_hint(void *, uint32_t timeout_ms)
{
    vTaskDelay(pdMS_TO_TICKS(timeout_ms == 0U ? 1U : timeout_ms));
}

uint32_t notify_waiter(void *, bool from_isr)
{
    if (s_backend.waiter_slot == nullptr ||
        *s_backend.waiter_slot == nullptr) return P4_NANO_AUDIO86_NOTIFY_NONE;
    if (from_isr) {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveIndexedFromISR(*s_backend.waiter_slot, 0U, &woken);
        if (woken == pdTRUE) portYIELD_FROM_ISR();
        return P4_NANO_AUDIO86_NOTIFY_ATTEMPTED |
               (woken == pdTRUE
                    ? P4_NANO_AUDIO86_NOTIFY_HIGHER_PRIORITY_WOKEN : 0U);
    } else {
        (void)xTaskNotifyGiveIndexed(*s_backend.waiter_slot, 0U);
        return P4_NANO_AUDIO86_NOTIFY_ATTEMPTED;
    }
}

void release(void *)
{
    record("I2C_RELEASE");
    s_backend.i2c = 0U;
    s_backend.codec = 0U;
    s_backend.pa_high = 0U;
    s_backend.released = 1U;
    record("DESTROY");
    s_backend.destroyed = 1U;
}

const p4_nano_audio86_physical_backend kOperations = {
    prepare, preload, enable, nullptr, write, mute, pa_low, disable,
    unregister_callbacks, now_ms, wait_hint, notify_waiter, release, nullptr};

} // namespace

esp_err_t create_lifecycle_test(p4_nano_audio86_physical_sink **sink,
                                TaskHandle_t *waiter_slot) noexcept
{
    if (sink == nullptr || waiter_slot == nullptr) return ESP_ERR_INVALID_ARG;
    std::memset(&s_backend, 0, sizeof(s_backend));
    s_backend.waiter_slot = waiter_slot;
    p4_nano_audio86_physical_backend operations = kOperations;
    operations.opaque = &s_backend;
    return p4_nano_audio86_physical_sink_create(sink, &operations) == 0
               ? ESP_OK : ESP_ERR_NO_MEM;
}

bool lifecycle_test_evidence_valid() noexcept
{
    return s_backend.i2s == 0U && s_backend.callbacks == 0U &&
           s_backend.i2c == 0U && s_backend.codec == 0U &&
           s_backend.pa_high == 0U && s_backend.released == 1U &&
           s_backend.destroyed == 1U && s_backend.event_count >= 8U;
}

void emit_lifecycle_test_backend_evidence() noexcept
{
    for (uint32_t index = 0U; index < s_backend.event_count; ++index) {
        const Event &event = s_backend.events[index];
        std::printf("5D1_HISTORY schema=2 evidence_class=ESP_EMU_EXEC scenario=start_fatal_%u sequence=%" PRIu32 " generation=%" PRIu32 " operation=%s result=%d bytes=%zu\n",
                    P4_NANO_AUDIO86_PHYSICAL_LIFECYCLE_TEST_PROFILE,
                    event.sequence, event.generation, event.operation,
                    event.result, event.bytes);
    }
    std::printf("5D1_FAKE_BACKEND schema=2 evidence_class=ESP_EMU_EXEC scenario=start_fatal_%u i2s=%" PRIu32 " callbacks=%" PRIu32 " i2c=%" PRIu32 " codec=%" PRIu32 " pa_high=%" PRIu32 " released=%" PRIu32 " destroyed=%" PRIu32 "\n",
                P4_NANO_AUDIO86_PHYSICAL_LIFECYCLE_TEST_PROFILE,
                s_backend.i2s, s_backend.callbacks, s_backend.i2c,
                s_backend.codec, s_backend.pa_high, s_backend.released,
                s_backend.destroyed);
}

} // namespace p4_nano_audio86_physical
