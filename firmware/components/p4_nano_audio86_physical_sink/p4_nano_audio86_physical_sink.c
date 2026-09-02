/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_audio86_physical_sink/p4_nano_audio86_physical_sink.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#include "esp_heap_caps.h"
#define CALLBACK_IRAM IRAM_ATTR
#else
#define CALLBACK_IRAM
#endif

struct p4_nano_audio86_callback_gate {
    _Atomic uint32_t in_flight;
    _Atomic uint32_t armed;
    _Atomic uint32_t registration_generation;
    _Atomic uintptr_t target;
    _Atomic uint32_t stale_count;
};

struct p4_nano_audio86_physical_sink {
    struct p4_nano_audio86_physical_backend backend;
    _Atomic uint32_t state;
    _Atomic uint32_t generation;
    _Atomic uint32_t callbacks_active;
    struct p4_nano_audio86_callback_gate callback_gate;
    _Atomic uint32_t tx_eof_epoch;
    _Atomic uint32_t sticky_error;
    _Atomic uint32_t running_queue_overflow_count;
    _Atomic uint32_t draining_queue_overflow_count;
    uint32_t preloaded_units;
    uint32_t drain_snapshot_epoch;
    uint32_t drain_completion_epoch;
    uint64_t semantic_accepted_frames;
    uint64_t semantic_accepted_bytes;
    uint64_t physical_units_copied;
    uint64_t physical_bytes_copied;
    uint64_t physical_padding_frames;
    uint64_t accepted_pending_drain_frames;
    uint64_t physically_drained_frames;
    uint64_t physically_discarded_accepted_frames;
    _Alignas(16) uint8_t staging[P4_NANO_AUDIO86_PHYSICAL_UNIT_BYTES];
};

static void *sink_calloc(size_t size)
{
#if defined(ESP_PLATFORM)
    return heap_caps_calloc(1U, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return calloc(1U, size);
#endif
}

static void sink_free(void *pointer)
{
#if defined(ESP_PLATFORM)
    heap_caps_free(pointer);
#else
    free(pointer);
#endif
}

static enum p4_nano_audio86_physical_state load_state(
    const struct p4_nano_audio86_physical_sink *sink)
{
    return (enum p4_nano_audio86_physical_state)atomic_load_explicit(
        &sink->state, memory_order_acquire);
}

static void notify_waiter(struct p4_nano_audio86_physical_sink *sink,
                          bool from_isr)
{
    if (sink->backend.notify_waiter != NULL) {
        sink->backend.notify_waiter(sink->backend.opaque, from_isr);
    }
}

static void mark_failed(struct p4_nano_audio86_physical_sink *sink)
{
    atomic_store_explicit(&sink->sticky_error, 1U, memory_order_release);
    atomic_store_explicit(&sink->state, P4_NANO_AUDIO86_PHYSICAL_FAILED,
                          memory_order_release);
    notify_waiter(sink, false);
}

static bool backend_valid(
    const struct p4_nano_audio86_physical_backend *backend)
{
    return backend != NULL && backend->prepare != NULL &&
           backend->preload != NULL && backend->enable_stream != NULL &&
           backend->write != NULL && backend->mute != NULL &&
           backend->pa_low != NULL && backend->disable != NULL &&
           backend->unregister_callbacks != NULL &&
           backend->now_ms != NULL && backend->wait_hint != NULL;
}

int p4_nano_audio86_physical_sink_create(
    struct p4_nano_audio86_physical_sink **out,
    const struct p4_nano_audio86_physical_backend *backend)
{
    struct p4_nano_audio86_physical_sink *sink;
    if (out == NULL || !backend_valid(backend)) {
        return -1;
    }
    *out = NULL;
    sink = sink_calloc(sizeof(*sink));
    if (sink == NULL) {
        return -1;
    }
    sink->backend = *backend;
    atomic_init(&sink->state, P4_NANO_AUDIO86_PHYSICAL_INITIAL);
    atomic_init(&sink->generation, 0U);
    atomic_init(&sink->callbacks_active, 0U);
    atomic_init(&sink->callback_gate.in_flight, 0U);
    atomic_init(&sink->callback_gate.armed, 0U);
    atomic_init(&sink->callback_gate.registration_generation, 0U);
    atomic_init(&sink->callback_gate.target, (uintptr_t)0U);
    atomic_init(&sink->callback_gate.stale_count, 0U);
    atomic_init(&sink->tx_eof_epoch, 0U);
    atomic_init(&sink->sticky_error, 0U);
    atomic_init(&sink->running_queue_overflow_count, 0U);
    atomic_init(&sink->draining_queue_overflow_count, 0U);
    *out = sink;
    return 0;
}

int p4_nano_audio86_physical_sink_destroy(
    struct p4_nano_audio86_physical_sink *sink)
{
    enum p4_nano_audio86_physical_state state;
    if (sink == NULL) {
        return -1;
    }
    state = load_state(sink);
    if ((state != P4_NANO_AUDIO86_PHYSICAL_INITIAL &&
         state != P4_NANO_AUDIO86_PHYSICAL_QUIESCENT) ||
        atomic_load_explicit(&sink->callback_gate.in_flight,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&sink->callbacks_active,
                             memory_order_acquire) != 0U) {
        return -1;
    }
    if (sink->backend.release != NULL) {
        sink->backend.release(sink->backend.opaque);
    }
    memset(sink, 0, sizeof(*sink));
    sink_free(sink);
    return 0;
}

static enum np2_pcm_sink_result physical_start(void *opaque)
{
    struct p4_nano_audio86_physical_sink *sink = opaque;
    uint32_t generation;
    if (sink == NULL || load_state(sink) != P4_NANO_AUDIO86_PHYSICAL_INITIAL) {
        return NP2_PCM_SINK_FATAL;
    }
    generation = atomic_fetch_add_explicit(&sink->generation, 1U,
                                            memory_order_acq_rel) + 1U;
    atomic_store_explicit(&sink->callback_gate.registration_generation,
                          generation, memory_order_release);
    atomic_store_explicit(&sink->callback_gate.target, (uintptr_t)sink,
                          memory_order_release);
    if (sink->backend.prepare(sink->backend.opaque, &sink->callback_gate,
                              generation) != 0) {
        mark_failed(sink);
        return NP2_PCM_SINK_FATAL;
    }
    atomic_store_explicit(&sink->callback_gate.armed, 1U,
                          memory_order_release);
    atomic_store_explicit(&sink->state,
                          P4_NANO_AUDIO86_PHYSICAL_PREPARED_ACCEPTING,
                          memory_order_release);
    return NP2_PCM_SINK_ACCEPTED;
}

static bool prepare_physical_unit(
    struct p4_nano_audio86_physical_sink *sink,
    const struct np2_pcm_sink_view *view, uint64_t *semantic_bytes,
    uint32_t *padding_frames)
{
    size_t bytes;
    bool full;
    if (view == NULL || view->pcm == NULL || view->valid_frames == 0U ||
        view->valid_frames > P4_NANO_AUDIO86_PHYSICAL_FRAMES_PER_UNIT) {
        return false;
    }
    full = view->valid_frames == P4_NANO_AUDIO86_PHYSICAL_FRAMES_PER_UNIT;
    if ((full && view->flags != 0U) ||
        (!full && view->flags != NP2_OPNGEN_PCM_RING_FLAG_FINAL_PARTIAL)) {
        return false;
    }
    bytes = (size_t)view->valid_frames *
            P4_NANO_AUDIO86_PHYSICAL_BYTES_PER_FRAME;
    memset(sink->staging, 0, sizeof(sink->staging));
    memcpy(sink->staging, view->pcm, bytes);
    *semantic_bytes = bytes;
    *padding_frames = P4_NANO_AUDIO86_PHYSICAL_FRAMES_PER_UNIT -
                      view->valid_frames;
    return true;
}

static bool start_stream(struct p4_nano_audio86_physical_sink *sink,
                         enum p4_nano_audio86_physical_state success_state)
{
    atomic_store_explicit(&sink->state, P4_NANO_AUDIO86_PHYSICAL_STARTING,
                          memory_order_release);
    atomic_store_explicit(&sink->callbacks_active, 1U,
                          memory_order_release);
    if (sink->backend.enable_stream(sink->backend.opaque) != 0) {
        atomic_store_explicit(&sink->callbacks_active, 0U,
                              memory_order_release);
        mark_failed(sink);
        return false;
    }
    atomic_store_explicit(&sink->state, success_state, memory_order_release);
    return true;
}

static enum np2_pcm_sink_result physical_submit(
    void *opaque, const struct np2_pcm_sink_view *view)
{
    struct p4_nano_audio86_physical_sink *sink = opaque;
    enum p4_nano_audio86_physical_state state;
    enum p4_nano_audio86_physical_io_result result;
    uint64_t semantic_bytes = 0U;
    uint32_t padding_frames = 0U;
    size_t copied = 0U;
    if (sink == NULL ||
        !prepare_physical_unit(sink, view, &semantic_bytes, &padding_frames)) {
        if (sink != NULL) mark_failed(sink);
        return NP2_PCM_SINK_FATAL;
    }
    state = load_state(sink);
    if (state == P4_NANO_AUDIO86_PHYSICAL_PREPARED_ACCEPTING) {
        result = sink->backend.preload(sink->backend.opaque, sink->staging,
                                       sizeof(sink->staging), &copied);
    } else if (state == P4_NANO_AUDIO86_PHYSICAL_RUNNING) {
        result = sink->backend.write(sink->backend.opaque, sink->staging,
                                     sizeof(sink->staging), &copied, 0U);
    } else {
        mark_failed(sink);
        return NP2_PCM_SINK_FATAL;
    }

    if (result == P4_NANO_AUDIO86_PHYSICAL_IO_TIMEOUT && copied == 0U &&
        state == P4_NANO_AUDIO86_PHYSICAL_RUNNING) {
        return NP2_PCM_SINK_RETRY;
    }
    if (result != P4_NANO_AUDIO86_PHYSICAL_IO_OK ||
        copied != P4_NANO_AUDIO86_PHYSICAL_UNIT_BYTES) {
        mark_failed(sink);
        return NP2_PCM_SINK_FATAL;
    }

    sink->semantic_accepted_frames += view->valid_frames;
    sink->semantic_accepted_bytes += semantic_bytes;
    sink->physical_units_copied++;
    sink->physical_bytes_copied += P4_NANO_AUDIO86_PHYSICAL_UNIT_BYTES;
    sink->physical_padding_frames += padding_frames;
    sink->accepted_pending_drain_frames += view->valid_frames;
    sink->drain_snapshot_epoch = atomic_load_explicit(
        &sink->tx_eof_epoch, memory_order_acquire);
    if (state == P4_NANO_AUDIO86_PHYSICAL_PREPARED_ACCEPTING) {
        sink->preloaded_units++;
        if (sink->preloaded_units ==
                P4_NANO_AUDIO86_PHYSICAL_DMA_DESCRIPTORS &&
            !start_stream(sink, P4_NANO_AUDIO86_PHYSICAL_RUNNING)) {
            return NP2_PCM_SINK_FATAL;
        }
    }
    return NP2_PCM_SINK_ACCEPTED;
}

static bool deadline_expired(struct p4_nano_audio86_physical_sink *sink,
                             uint64_t start_ms)
{
    uint64_t now = sink->backend.now_ms(sink->backend.opaque);
    return now < start_ms ||
           now - start_ms >= P4_NANO_AUDIO86_PHYSICAL_DRAIN_TIMEOUT_MS;
}

static bool wait_for_callbacks_zero(
    struct p4_nano_audio86_physical_sink *sink)
{
    uint64_t start_ms = sink->backend.now_ms(sink->backend.opaque);
    while (atomic_load_explicit(&sink->callback_gate.in_flight,
                                memory_order_acquire) != 0U) {
        if (deadline_expired(sink, start_ms)) {
            return false;
        }
        sink->backend.wait_hint(sink->backend.opaque, 1U);
    }
    return true;
}

static void disarm_callbacks(struct p4_nano_audio86_physical_sink *sink)
{
    atomic_store_explicit(&sink->callbacks_active, 0U,
                          memory_order_release);
    atomic_store_explicit(&sink->callback_gate.armed, 0U,
                          memory_order_release);
    atomic_store_explicit(&sink->callback_gate.target, (uintptr_t)0U,
                          memory_order_release);
    atomic_fetch_add_explicit(&sink->generation, 1U, memory_order_acq_rel);
}

static bool close_callbacks(struct p4_nano_audio86_physical_sink *sink)
{
    if (sink->backend.unregister_callbacks(sink->backend.opaque) != 0) {
        return false;
    }
    return wait_for_callbacks_zero(sink);
}

static enum np2_pcm_sink_result physical_finish(void *opaque)
{
    struct p4_nano_audio86_physical_sink *sink = opaque;
    enum p4_nano_audio86_physical_state state;
    uint64_t start_ms;
    uint32_t snapshot;
    if (sink == NULL) return NP2_PCM_SINK_FATAL;
    state = load_state(sink);
    if (state != P4_NANO_AUDIO86_PHYSICAL_PREPARED_ACCEPTING &&
        state != P4_NANO_AUDIO86_PHYSICAL_RUNNING) {
        mark_failed(sink);
        return NP2_PCM_SINK_FATAL;
    }
    snapshot = sink->drain_snapshot_epoch;
    if (state == P4_NANO_AUDIO86_PHYSICAL_PREPARED_ACCEPTING) {
        if (sink->preloaded_units == 0U ||
            !start_stream(sink, P4_NANO_AUDIO86_PHYSICAL_DRAINING)) {
            return NP2_PCM_SINK_FATAL;
        }
    } else {
        atomic_store_explicit(&sink->state,
                              P4_NANO_AUDIO86_PHYSICAL_DRAINING,
                              memory_order_release);
    }
    start_ms = sink->backend.now_ms(sink->backend.opaque);
    while ((uint32_t)(atomic_load_explicit(&sink->tx_eof_epoch,
                                           memory_order_acquire) - snapshot) <
           P4_NANO_AUDIO86_PHYSICAL_DMA_DESCRIPTORS) {
        if (atomic_load_explicit(&sink->sticky_error,
                                 memory_order_acquire) != 0U ||
            deadline_expired(sink, start_ms)) {
            mark_failed(sink);
            return NP2_PCM_SINK_FATAL;
        }
        sink->backend.wait_hint(sink->backend.opaque, 1U);
    }
    sink->drain_completion_epoch = atomic_load_explicit(
        &sink->tx_eof_epoch, memory_order_acquire);
    disarm_callbacks(sink);
    if (sink->backend.mute(sink->backend.opaque) != 0 ||
        sink->backend.pa_low(sink->backend.opaque) != 0 ||
        sink->backend.disable(sink->backend.opaque) != 0 ||
        !close_callbacks(sink)) {
        mark_failed(sink);
        return NP2_PCM_SINK_FATAL;
    }
    sink->physically_drained_frames += sink->accepted_pending_drain_frames;
    sink->accepted_pending_drain_frames = 0U;
    atomic_store_explicit(&sink->state, P4_NANO_AUDIO86_PHYSICAL_QUIESCENT,
                          memory_order_release);
    return NP2_PCM_SINK_ACCEPTED;
}

static enum np2_pcm_sink_result physical_abort(void *opaque)
{
    struct p4_nano_audio86_physical_sink *sink = opaque;
    bool ok = true;
    if (sink == NULL) return NP2_PCM_SINK_FATAL;
    atomic_store_explicit(&sink->state, P4_NANO_AUDIO86_PHYSICAL_ABORTING,
                          memory_order_release);
    disarm_callbacks(sink);
    notify_waiter(sink, false);
    if (sink->backend.mute(sink->backend.opaque) != 0) ok = false;
    if (sink->backend.pa_low(sink->backend.opaque) != 0) ok = false;
    if (sink->backend.disable(sink->backend.opaque) != 0) ok = false;
    if (sink->backend.unregister_callbacks(sink->backend.opaque) != 0)
        ok = false;
    if (!wait_for_callbacks_zero(sink)) ok = false;
    sink->physically_discarded_accepted_frames +=
        sink->accepted_pending_drain_frames;
    sink->accepted_pending_drain_frames = 0U;
    atomic_store_explicit(&sink->state,
                          ok ? P4_NANO_AUDIO86_PHYSICAL_QUIESCENT
                             : P4_NANO_AUDIO86_PHYSICAL_FAILED,
                          memory_order_release);
    return ok ? NP2_PCM_SINK_ACCEPTED : NP2_PCM_SINK_FATAL;
}

struct np2_pcm_sink p4_nano_audio86_physical_sink_interface(
    struct p4_nano_audio86_physical_sink *sink)
{
    const struct np2_pcm_sink interface = {
        sink, physical_start, physical_submit, physical_finish, physical_abort};
    return interface;
}

uint32_t p4_nano_audio86_physical_sink_retry_snapshot(
    const struct p4_nano_audio86_physical_sink *sink)
{
    return sink == NULL ? 0U : atomic_load_explicit(
        &sink->tx_eof_epoch, memory_order_acquire);
}

bool p4_nano_audio86_physical_sink_retry_ready(
    const struct p4_nano_audio86_physical_sink *sink, uint32_t snapshot)
{
    enum p4_nano_audio86_physical_state state;
    if (sink == NULL) return true;
    state = load_state(sink);
    return atomic_load_explicit(&sink->tx_eof_epoch, memory_order_acquire) !=
               snapshot ||
           atomic_load_explicit(&sink->sticky_error, memory_order_acquire) !=
               0U ||
           (state != P4_NANO_AUDIO86_PHYSICAL_RUNNING &&
            state != P4_NANO_AUDIO86_PHYSICAL_STARTING);
}

static CALLBACK_IRAM struct p4_nano_audio86_physical_sink *callback_gate_enter(
    struct p4_nano_audio86_callback_gate *gate, uint32_t generation,
    bool generation_override)
{
    uintptr_t target;
    uint32_t armed;
    uint32_t registered_generation;
    if (gate == NULL) return NULL;
    /* This must remain the first meaningful callback action: the gate is the
     * guaranteed-live IDF user_data object protecting every later pointer. */
    atomic_fetch_add_explicit(&gate->in_flight, 1U, memory_order_acq_rel);
    if (!generation_override) {
        generation = atomic_load_explicit(&gate->registration_generation,
                                          memory_order_acquire);
    }
    armed = atomic_load_explicit(&gate->armed, memory_order_acquire);
    registered_generation = atomic_load_explicit(
        &gate->registration_generation, memory_order_acquire);
    target = (armed != 0U && registered_generation == generation)
        ? atomic_load_explicit(&gate->target, memory_order_acquire)
        : (uintptr_t)0U;
    if (armed == 0U || registered_generation != generation ||
        target == (uintptr_t)0U) {
        atomic_fetch_add_explicit(&gate->stale_count, 1U,
                                  memory_order_relaxed);
        atomic_fetch_sub_explicit(&gate->in_flight, 1U,
                                  memory_order_release);
        return NULL;
    }
    return (struct p4_nano_audio86_physical_sink *)target;
}

static CALLBACK_IRAM void callback_gate_exit(
    struct p4_nano_audio86_callback_gate *gate)
{
    atomic_fetch_sub_explicit(&gate->in_flight, 1U, memory_order_release);
}

static CALLBACK_IRAM void callback_on_sent(
    struct p4_nano_audio86_callback_gate *gate, uint32_t generation,
    bool generation_override)
{
    struct p4_nano_audio86_physical_sink *sink =
        callback_gate_enter(gate, generation, generation_override);
    if (sink != NULL) {
        atomic_fetch_add_explicit(&sink->tx_eof_epoch, 1U,
                                  memory_order_release);
        notify_waiter(sink, true);
        callback_gate_exit(gate);
    }
}

static CALLBACK_IRAM void callback_on_send_q_ovf(
    struct p4_nano_audio86_callback_gate *gate, uint32_t generation,
    bool generation_override)
{
    enum p4_nano_audio86_physical_state state;
    struct p4_nano_audio86_physical_sink *sink =
        callback_gate_enter(gate, generation, generation_override);
    if (sink != NULL) {
        state = load_state(sink);
        if (state == P4_NANO_AUDIO86_PHYSICAL_DRAINING) {
            atomic_fetch_add_explicit(&sink->draining_queue_overflow_count, 1U,
                                      memory_order_relaxed);
        } else if (state == P4_NANO_AUDIO86_PHYSICAL_RUNNING ||
                   state == P4_NANO_AUDIO86_PHYSICAL_STARTING) {
            atomic_fetch_add_explicit(&sink->running_queue_overflow_count, 1U,
                                      memory_order_relaxed);
            atomic_store_explicit(&sink->sticky_error, 1U,
                                  memory_order_release);
            notify_waiter(sink, true);
        }
        callback_gate_exit(gate);
    }
}

void CALLBACK_IRAM p4_nano_audio86_callback_gate_on_sent(
    struct p4_nano_audio86_callback_gate *gate)
{
    if (gate == NULL) return;
    callback_on_sent(gate, 0U, false);
}

void CALLBACK_IRAM p4_nano_audio86_callback_gate_on_send_q_ovf(
    struct p4_nano_audio86_callback_gate *gate)
{
    if (gate == NULL) return;
    callback_on_send_q_ovf(gate, 0U, false);
}

void p4_nano_audio86_physical_sink_get_telemetry(
    const struct p4_nano_audio86_physical_sink *sink,
    struct p4_nano_audio86_physical_telemetry *telemetry)
{
    if (sink == NULL || telemetry == NULL) return;
    telemetry->semantic_accepted_frames = sink->semantic_accepted_frames;
    telemetry->semantic_accepted_bytes = sink->semantic_accepted_bytes;
    telemetry->physical_units_copied = sink->physical_units_copied;
    telemetry->physical_bytes_copied = sink->physical_bytes_copied;
    telemetry->physical_padding_frames = sink->physical_padding_frames;
    telemetry->accepted_pending_drain_frames =
        sink->accepted_pending_drain_frames;
    telemetry->physically_drained_frames = sink->physically_drained_frames;
    telemetry->physically_discarded_accepted_frames =
        sink->physically_discarded_accepted_frames;
    telemetry->tx_eof_epoch = atomic_load_explicit(
        &sink->tx_eof_epoch, memory_order_acquire);
    telemetry->drain_snapshot_epoch = sink->drain_snapshot_epoch;
    telemetry->drain_completion_epoch = sink->drain_completion_epoch;
    telemetry->generation = atomic_load_explicit(&sink->generation,
                                                  memory_order_acquire);
    telemetry->stale_callback_count = atomic_load_explicit(
        &sink->callback_gate.stale_count, memory_order_acquire);
    telemetry->running_queue_overflow_count = atomic_load_explicit(
        &sink->running_queue_overflow_count, memory_order_acquire);
    telemetry->draining_queue_overflow_count = atomic_load_explicit(
        &sink->draining_queue_overflow_count, memory_order_acquire);
    telemetry->callback_refcount = atomic_load_explicit(
        &sink->callback_gate.in_flight, memory_order_acquire);
    telemetry->preloaded_units = sink->preloaded_units;
    telemetry->state = load_state(sink);
    telemetry->sticky_error = atomic_load_explicit(
        &sink->sticky_error, memory_order_acquire) != 0U;
    telemetry->callbacks_active = atomic_load_explicit(
        &sink->callbacks_active, memory_order_acquire) != 0U;
}

#if defined(P4_NANO_AUDIO86_PHYSICAL_SINK_TESTING)
void p4_nano_audio86_physical_sink_test_set_callback_refcount(
    struct p4_nano_audio86_physical_sink *sink, uint32_t count)
{
    if (sink != NULL) {
        atomic_store_explicit(&sink->callback_gate.in_flight, count,
                              memory_order_release);
    }
}

void p4_nano_audio86_physical_sink_test_on_sent_generation(
    struct p4_nano_audio86_physical_sink *sink, uint32_t generation)
{
    if (sink != NULL)
        callback_on_sent(&sink->callback_gate, generation, true);
}

void p4_nano_audio86_physical_sink_test_on_send_q_ovf_generation(
    struct p4_nano_audio86_physical_sink *sink, uint32_t generation)
{
    if (sink != NULL)
        callback_on_send_q_ovf(&sink->callback_gate, generation, true);
}

bool p4_nano_audio86_physical_sink_test_callback_enter(
    struct p4_nano_audio86_physical_sink *sink, uint32_t generation)
{
    return sink != NULL &&
        callback_gate_enter(&sink->callback_gate, generation, true) != NULL;
}

void p4_nano_audio86_physical_sink_test_callback_exit(
    struct p4_nano_audio86_physical_sink *sink)
{
    if (sink != NULL) callback_gate_exit(&sink->callback_gate);
}

void p4_nano_audio86_physical_sink_test_disarm_callbacks(
    struct p4_nano_audio86_physical_sink *sink)
{
    if (sink != NULL) disarm_callbacks(sink);
}
#endif
