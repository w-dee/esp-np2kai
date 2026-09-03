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

#define DIAGNOSTIC_PHASE_BITS 4U
#define DIAGNOSTIC_SEQUENCE_BITS 14U
#define DIAGNOSTIC_PHASE_MASK ((1U << DIAGNOSTIC_PHASE_BITS) - 1U)
#define DIAGNOSTIC_SEQUENCE_MASK ((1U << DIAGNOSTIC_SEQUENCE_BITS) - 1U)
#define DIAGNOSTIC_CURRENT_SHIFT DIAGNOSTIC_PHASE_BITS
#define DIAGNOSTIC_PUBLISHED_SHIFT \
    (DIAGNOSTIC_PHASE_BITS + DIAGNOSTIC_SEQUENCE_BITS)
#define DIAGNOSTIC_WAIT_REASON_BITS 3U
#define DIAGNOSTIC_WAIT_SEQUENCE_BITS 9U
#define DIAGNOSTIC_WAIT_REASON_MASK \
    ((1U << DIAGNOSTIC_WAIT_REASON_BITS) - 1U)
#define DIAGNOSTIC_WAIT_SEQUENCE_MASK \
    ((1U << DIAGNOSTIC_WAIT_SEQUENCE_BITS) - 1U)
#define DIAGNOSTIC_WAIT_SEQUENCE_SHIFT DIAGNOSTIC_WAIT_REASON_BITS
#define DIAGNOSTIC_RENDERED_FRAME_BITS 17U
#define DIAGNOSTIC_PUBLISHED_UNIT_BITS 9U
#define DIAGNOSTIC_OCCUPANCY_BITS 4U
#define DIAGNOSTIC_RENDERED_FRAME_MASK \
    ((1U << DIAGNOSTIC_RENDERED_FRAME_BITS) - 1U)
#define DIAGNOSTIC_PUBLISHED_UNIT_MASK \
    ((1U << DIAGNOSTIC_PUBLISHED_UNIT_BITS) - 1U)
#define DIAGNOSTIC_OCCUPANCY_MASK \
    ((1U << DIAGNOSTIC_OCCUPANCY_BITS) - 1U)
#define DIAGNOSTIC_PUBLISHED_UNIT_SHIFT DIAGNOSTIC_RENDERED_FRAME_BITS
#define DIAGNOSTIC_OCCUPANCY_SHIFT \
    (DIAGNOSTIC_RENDERED_FRAME_BITS + DIAGNOSTIC_PUBLISHED_UNIT_BITS)
#define DIAGNOSTIC_PRODUCTION_DONE_SHIFT \
    (DIAGNOSTIC_OCCUPANCY_SHIFT + DIAGNOSTIC_OCCUPANCY_BITS)

struct p4_nano_audio86_diagnostic_publication {
    _Atomic uint32_t service_word;
    _Atomic uint32_t last_step_enter_us;
    _Atomic uint32_t last_submit_return_us;
    _Atomic uint32_t last_step_exit_us;
    _Atomic uint32_t last_running_accepted_us;
    _Atomic uint32_t ring_context_word;
    _Atomic uint32_t wait_context_word;
    _Atomic uint32_t last_wait_enter_us;
    _Atomic uint32_t last_wait_resume_us;
    _Atomic uint32_t last_wait_resume_context_word;
    _Atomic uint32_t retry_wait_enter_count;
    _Atomic uint32_t retry_wait_resume_count;
    _Atomic uint32_t ring_wait_enter_count;
    _Atomic uint32_t ring_wait_resume_count;
    _Atomic uint32_t eof_notify_count;
    _Atomic uint32_t eof_hpwoken_true_count;
    /* 0=empty, 1=callback is writing, 2=frozen and readable. */
    _Atomic uint32_t first_qovf_latch_state;
    _Atomic uint32_t first_qovf_state;
    _Atomic uint32_t first_qovf_eof_epoch;
    _Atomic uint32_t first_qovf_service_word;
    _Atomic uint32_t first_qovf_last_step_enter_us;
    _Atomic uint32_t first_qovf_last_submit_return_us;
    _Atomic uint32_t first_qovf_last_step_exit_us;
    _Atomic uint32_t first_qovf_last_running_accepted_us;
    _Atomic uint32_t first_qovf_ring_context_word;
    _Atomic uint32_t first_qovf_wait_context_word;
    _Atomic uint32_t first_qovf_last_wait_enter_us;
    _Atomic uint32_t first_qovf_last_wait_resume_us;
    _Atomic uint32_t first_qovf_last_wait_resume_context_word;
    _Atomic uint32_t first_qovf_retry_wait_enter_count;
    _Atomic uint32_t first_qovf_retry_wait_resume_count;
    _Atomic uint32_t first_qovf_ring_wait_enter_count;
    _Atomic uint32_t first_qovf_ring_wait_resume_count;
    _Atomic uint32_t first_qovf_eof_notify_count;
    _Atomic uint32_t first_qovf_eof_hpwoken_true_count;
    _Atomic uint32_t first_qovf_observed;
    _Atomic uint32_t first_qovf_observed_us;
    _Atomic uint32_t enable_stream_duration_us;
    _Atomic uint32_t codec_unmute_duration_us;
    _Atomic uint32_t startup_durations_valid;
};

struct p4_nano_audio86_callback_gate {
    _Atomic uint32_t in_flight;
    _Atomic uint32_t armed;
    _Atomic uint32_t registration_generation;
    _Atomic uintptr_t target;
    _Atomic uint32_t stale_count;
    struct p4_nano_audio86_diagnostic_publication diagnostic;
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
    uint32_t full_units;
    uint32_t final_partial_units;
    uint32_t final_valid_frames;
    uint32_t drain_snapshot_epoch;
    uint32_t drain_completion_epoch;
    uint32_t quiescent_eof_epoch;
    uint64_t submit_attempts;
    uint64_t retry_count;
    uint64_t drain_duration_ms;
    uint64_t semantic_accepted_frames;
    uint64_t semantic_accepted_bytes;
    uint64_t physical_units_copied;
    uint64_t physical_bytes_copied;
    uint64_t physical_padding_frames;
    uint64_t accepted_pending_drain_frames;
    uint64_t physically_drained_frames;
    uint64_t physically_discarded_accepted_frames;
    bool prepare_completed;
    bool pa_initial_low;
    bool codec_initialized_muted;
    bool i2s_initialized;
    bool muted_warmup_completed;
    bool callbacks_registered;
    bool stream_started;
    bool codec_unmute_completed;
    bool finish_completed;
    bool codec_final_muted;
    bool pa_final_low;
    bool i2s_enabled;
    bool i2s_created;
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

static uint32_t diagnostic_service_word(
    enum p4_nano_audio86_consumer_service_phase phase,
    uint32_t current_sequence, uint32_t published_sequence)
{
    return ((uint32_t)phase & DIAGNOSTIC_PHASE_MASK) |
           ((current_sequence & DIAGNOSTIC_SEQUENCE_MASK) <<
            DIAGNOSTIC_CURRENT_SHIFT) |
           ((published_sequence & DIAGNOSTIC_SEQUENCE_MASK) <<
            DIAGNOSTIC_PUBLISHED_SHIFT);
}

static enum p4_nano_audio86_consumer_service_phase diagnostic_phase(
    uint32_t service_word)
{
    return (enum p4_nano_audio86_consumer_service_phase)(
        service_word & DIAGNOSTIC_PHASE_MASK);
}

static uint32_t diagnostic_current_sequence(uint32_t service_word)
{
    return (service_word >> DIAGNOSTIC_CURRENT_SHIFT) &
           DIAGNOSTIC_SEQUENCE_MASK;
}

static uint32_t diagnostic_published_sequence(uint32_t service_word)
{
    return (service_word >> DIAGNOSTIC_PUBLISHED_SHIFT) &
           DIAGNOSTIC_SEQUENCE_MASK;
}

static uint32_t diagnostic_wait_context_word(
    enum p4_nano_audio86_consumer_wait_reason reason,
    uint32_t consumer_next_sequence)
{
    return ((uint32_t)reason & DIAGNOSTIC_WAIT_REASON_MASK) |
           ((consumer_next_sequence & DIAGNOSTIC_WAIT_SEQUENCE_MASK) <<
            DIAGNOSTIC_WAIT_SEQUENCE_SHIFT);
}

static uint32_t diagnostic_ring_context_word(
    uint32_t rendered_frames, uint32_t next_published_sequence,
    uint32_t occupancy, bool production_done)
{
    return (rendered_frames & DIAGNOSTIC_RENDERED_FRAME_MASK) |
           ((next_published_sequence & DIAGNOSTIC_PUBLISHED_UNIT_MASK) <<
            DIAGNOSTIC_PUBLISHED_UNIT_SHIFT) |
           ((occupancy & DIAGNOSTIC_OCCUPANCY_MASK) <<
            DIAGNOSTIC_OCCUPANCY_SHIFT) |
           ((production_done ? 1U : 0U) <<
            DIAGNOSTIC_PRODUCTION_DONE_SHIFT);
}

static uint32_t diagnostic_wait_reason(uint32_t word)
{
    return word & DIAGNOSTIC_WAIT_REASON_MASK;
}

static uint32_t diagnostic_wait_sequence(uint32_t word)
{
    return (word >> DIAGNOSTIC_WAIT_SEQUENCE_SHIFT) &
           DIAGNOSTIC_WAIT_SEQUENCE_MASK;
}

static uint32_t diagnostic_rendered_frames(uint32_t word)
{
    return word & DIAGNOSTIC_RENDERED_FRAME_MASK;
}

static uint32_t diagnostic_next_published_sequence(uint32_t word)
{
    return (word >> DIAGNOSTIC_PUBLISHED_UNIT_SHIFT) &
           DIAGNOSTIC_PUBLISHED_UNIT_MASK;
}

static uint32_t diagnostic_ring_occupancy(uint32_t word)
{
    return (word >> DIAGNOSTIC_OCCUPANCY_SHIFT) & DIAGNOSTIC_OCCUPANCY_MASK;
}

static uint32_t diagnostic_production_done(uint32_t word)
{
    return (word >> DIAGNOSTIC_PRODUCTION_DONE_SHIFT) & 1U;
}

static void diagnostic_set_phase(
    struct p4_nano_audio86_diagnostic_publication *diagnostic,
    enum p4_nano_audio86_consumer_service_phase phase)
{
    uint32_t old_word;
    uint32_t new_word;
    if (diagnostic == NULL) return;
    old_word = atomic_load_explicit(&diagnostic->service_word,
                                    memory_order_acquire);
    do {
        new_word = (old_word & ~DIAGNOSTIC_PHASE_MASK) |
                   ((uint32_t)phase & DIAGNOSTIC_PHASE_MASK);
    } while (!atomic_compare_exchange_weak_explicit(
        &diagnostic->service_word, &old_word, new_word,
        memory_order_release, memory_order_acquire));
}

void p4_nano_audio86_callback_gate_set_service_phase(
    struct p4_nano_audio86_callback_gate *gate,
    enum p4_nano_audio86_consumer_service_phase phase)
{
    if (gate != NULL) diagnostic_set_phase(&gate->diagnostic, phase);
}

void p4_nano_audio86_physical_sink_publish_consumer_progress(
    struct p4_nano_audio86_physical_sink *sink,
    enum p4_nano_audio86_consumer_progress_point point,
    enum p4_nano_audio86_consumer_service_phase phase,
    uint32_t current_sequence, uint32_t published_sequence,
    uint32_t relative_us)
{
    struct p4_nano_audio86_diagnostic_publication *diagnostic;
    if (sink == NULL || (uint32_t)phase >
            P4_NANO_AUDIO86_CONSUMER_PHASE_FINISH ||
        current_sequence > DIAGNOSTIC_SEQUENCE_MASK ||
        published_sequence > DIAGNOSTIC_SEQUENCE_MASK)
        return;
    diagnostic = &sink->callback_gate.diagnostic;
    switch (point) {
    case P4_NANO_AUDIO86_PROGRESS_STEP_ENTER:
        atomic_store_explicit(&diagnostic->last_step_enter_us, relative_us,
                              memory_order_release);
        break;
    case P4_NANO_AUDIO86_PROGRESS_SUBMIT_RETURN:
        atomic_store_explicit(&diagnostic->last_submit_return_us, relative_us,
                              memory_order_release);
        break;
    case P4_NANO_AUDIO86_PROGRESS_STEP_EXIT:
        atomic_store_explicit(&diagnostic->last_step_exit_us, relative_us,
                              memory_order_release);
        break;
    case P4_NANO_AUDIO86_PROGRESS_RUNNING_ACCEPTED:
        atomic_store_explicit(&diagnostic->last_running_accepted_us,
                              relative_us, memory_order_release);
        break;
    case P4_NANO_AUDIO86_PROGRESS_PUBLISH_ONLY:
    default:
        break;
    }
    /* One release publication keeps phase/current/published sequence coherent
     * without an ISR-side lock or a retrying seqlock. */
    atomic_store_explicit(&diagnostic->service_word,
                          diagnostic_service_word(
                              phase, current_sequence, published_sequence),
                          memory_order_release);
}

void p4_nano_audio86_physical_sink_observe_first_qovf(
    struct p4_nano_audio86_physical_sink *sink, uint32_t relative_us)
{
    struct p4_nano_audio86_diagnostic_publication *diagnostic;
    uint32_t expected = 0U;
    if (sink == NULL) return;
    diagnostic = &sink->callback_gate.diagnostic;
    if (atomic_load_explicit(&diagnostic->first_qovf_latch_state,
                             memory_order_acquire) != 2U)
        return;
    if (atomic_compare_exchange_strong_explicit(
            &diagnostic->first_qovf_observed, &expected, 1U,
            memory_order_acq_rel, memory_order_acquire)) {
        atomic_store_explicit(&diagnostic->first_qovf_observed_us, relative_us,
                              memory_order_release);
        atomic_store_explicit(&diagnostic->first_qovf_observed, 2U,
                              memory_order_release);
    }
}

void p4_nano_audio86_physical_sink_publish_ring_context(
    struct p4_nano_audio86_physical_sink *sink, uint32_t rendered_frames,
    uint32_t next_published_sequence, uint32_t occupancy,
    bool production_done)
{
    if (sink == NULL || rendered_frames > DIAGNOSTIC_RENDERED_FRAME_MASK ||
        next_published_sequence > DIAGNOSTIC_PUBLISHED_UNIT_MASK ||
        occupancy > DIAGNOSTIC_OCCUPANCY_MASK)
        return;
    atomic_store_explicit(&sink->callback_gate.diagnostic.ring_context_word,
                          diagnostic_ring_context_word(
                              rendered_frames, next_published_sequence,
                              occupancy, production_done),
                          memory_order_release);
}

void p4_nano_audio86_physical_sink_publish_wait_enter(
    struct p4_nano_audio86_physical_sink *sink,
    enum p4_nano_audio86_consumer_wait_reason reason,
    uint32_t consumer_next_sequence, uint32_t relative_us)
{
    struct p4_nano_audio86_diagnostic_publication *diagnostic;
    if (sink == NULL || reason == P4_NANO_AUDIO86_CONSUMER_WAIT_RUNNABLE ||
        (uint32_t)reason > P4_NANO_AUDIO86_CONSUMER_WAIT_FINISH_OR_TERMINAL ||
        consumer_next_sequence > DIAGNOSTIC_WAIT_SEQUENCE_MASK)
        return;
    diagnostic = &sink->callback_gate.diagnostic;
    atomic_store_explicit(&diagnostic->last_wait_enter_us, relative_us,
                          memory_order_relaxed);
    if (reason == P4_NANO_AUDIO86_CONSUMER_WAIT_RETRY_EOF) {
        atomic_fetch_add_explicit(&diagnostic->retry_wait_enter_count, 1U,
                                  memory_order_relaxed);
    } else if (reason == P4_NANO_AUDIO86_CONSUMER_WAIT_PCM_RING_EMPTY ||
               reason == P4_NANO_AUDIO86_CONSUMER_WAIT_PCM_PREFILL) {
        atomic_fetch_add_explicit(&diagnostic->ring_wait_enter_count, 1U,
                                  memory_order_relaxed);
    }
    /* Release the reason last so the ISR cannot observe a new wait reason with
     * the preceding task-authored context still unpublished. */
    atomic_store_explicit(&diagnostic->wait_context_word,
                          diagnostic_wait_context_word(
                              reason, consumer_next_sequence),
                          memory_order_release);
}

void p4_nano_audio86_physical_sink_publish_wait_resume(
    struct p4_nano_audio86_physical_sink *sink,
    enum p4_nano_audio86_consumer_wait_reason reason,
    uint32_t consumer_next_sequence, uint32_t relative_us)
{
    struct p4_nano_audio86_diagnostic_publication *diagnostic;
    uint32_t resume_word;
    if (sink == NULL || reason == P4_NANO_AUDIO86_CONSUMER_WAIT_RUNNABLE ||
        (uint32_t)reason > P4_NANO_AUDIO86_CONSUMER_WAIT_FINISH_OR_TERMINAL ||
        consumer_next_sequence > DIAGNOSTIC_WAIT_SEQUENCE_MASK)
        return;
    diagnostic = &sink->callback_gate.diagnostic;
    resume_word = diagnostic_wait_context_word(reason,
                                                consumer_next_sequence);
    if (reason == P4_NANO_AUDIO86_CONSUMER_WAIT_RETRY_EOF) {
        atomic_fetch_add_explicit(&diagnostic->retry_wait_resume_count, 1U,
                                  memory_order_relaxed);
    } else if (reason == P4_NANO_AUDIO86_CONSUMER_WAIT_PCM_RING_EMPTY ||
               reason == P4_NANO_AUDIO86_CONSUMER_WAIT_PCM_PREFILL) {
        atomic_fetch_add_explicit(&diagnostic->ring_wait_resume_count, 1U,
                                  memory_order_relaxed);
    }
    atomic_store_explicit(&diagnostic->last_wait_resume_us, relative_us,
                          memory_order_relaxed);
    atomic_store_explicit(&diagnostic->last_wait_resume_context_word,
                          resume_word, memory_order_relaxed);
    /* RUNNABLE is published before the caller reevaluates level predicates. */
    atomic_store_explicit(&diagnostic->wait_context_word,
                          diagnostic_wait_context_word(
                              P4_NANO_AUDIO86_CONSUMER_WAIT_RUNNABLE,
                              consumer_next_sequence),
                          memory_order_release);
}

void p4_nano_audio86_physical_sink_publish_runnable(
    struct p4_nano_audio86_physical_sink *sink,
    uint32_t consumer_next_sequence)
{
    if (sink == NULL ||
        consumer_next_sequence > DIAGNOSTIC_WAIT_SEQUENCE_MASK)
        return;
    atomic_store_explicit(&sink->callback_gate.diagnostic.wait_context_word,
                          diagnostic_wait_context_word(
                              P4_NANO_AUDIO86_CONSUMER_WAIT_RUNNABLE,
                              consumer_next_sequence),
                          memory_order_release);
}

size_t p4_nano_audio86_physical_sink_diagnostic_storage_bytes(void)
{
    return sizeof(struct p4_nano_audio86_diagnostic_publication);
}

static uint32_t notify_waiter(struct p4_nano_audio86_physical_sink *sink,
                              bool from_isr)
{
    if (sink->backend.notify_waiter != NULL) {
        return sink->backend.notify_waiter(sink->backend.opaque, from_isr);
    }
    return P4_NANO_AUDIO86_NOTIFY_NONE;
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
    atomic_init(&sink->callback_gate.diagnostic.service_word, 0U);
    atomic_init(&sink->callback_gate.diagnostic.last_step_enter_us, 0U);
    atomic_init(&sink->callback_gate.diagnostic.last_submit_return_us, 0U);
    atomic_init(&sink->callback_gate.diagnostic.last_step_exit_us, 0U);
    atomic_init(&sink->callback_gate.diagnostic.last_running_accepted_us, 0U);
    atomic_init(&sink->callback_gate.diagnostic.ring_context_word, 0U);
    atomic_init(&sink->callback_gate.diagnostic.wait_context_word, 0U);
    atomic_init(&sink->callback_gate.diagnostic.last_wait_enter_us, 0U);
    atomic_init(&sink->callback_gate.diagnostic.last_wait_resume_us, 0U);
    atomic_init(
        &sink->callback_gate.diagnostic.last_wait_resume_context_word, 0U);
    atomic_init(&sink->callback_gate.diagnostic.retry_wait_enter_count, 0U);
    atomic_init(&sink->callback_gate.diagnostic.retry_wait_resume_count, 0U);
    atomic_init(&sink->callback_gate.diagnostic.ring_wait_enter_count, 0U);
    atomic_init(&sink->callback_gate.diagnostic.ring_wait_resume_count, 0U);
    atomic_init(&sink->callback_gate.diagnostic.eof_notify_count, 0U);
    atomic_init(&sink->callback_gate.diagnostic.eof_hpwoken_true_count, 0U);
    atomic_init(&sink->callback_gate.diagnostic.first_qovf_latch_state, 0U);
    atomic_init(&sink->callback_gate.diagnostic.first_qovf_state, 0U);
    atomic_init(&sink->callback_gate.diagnostic.first_qovf_eof_epoch, 0U);
    atomic_init(&sink->callback_gate.diagnostic.first_qovf_service_word, 0U);
    atomic_init(&sink->callback_gate.diagnostic.first_qovf_last_step_enter_us,
                0U);
    atomic_init(&sink->callback_gate.diagnostic.first_qovf_last_submit_return_us,
                0U);
    atomic_init(&sink->callback_gate.diagnostic.first_qovf_last_step_exit_us,
                0U);
    atomic_init(
        &sink->callback_gate.diagnostic.first_qovf_last_running_accepted_us, 0U);
    atomic_init(&sink->callback_gate.diagnostic.first_qovf_ring_context_word,
                0U);
    atomic_init(&sink->callback_gate.diagnostic.first_qovf_wait_context_word,
                0U);
    atomic_init(&sink->callback_gate.diagnostic.first_qovf_last_wait_enter_us,
                0U);
    atomic_init(&sink->callback_gate.diagnostic.first_qovf_last_wait_resume_us,
                0U);
    atomic_init(
        &sink->callback_gate.diagnostic.first_qovf_last_wait_resume_context_word,
        0U);
    atomic_init(
        &sink->callback_gate.diagnostic.first_qovf_retry_wait_enter_count, 0U);
    atomic_init(
        &sink->callback_gate.diagnostic.first_qovf_retry_wait_resume_count, 0U);
    atomic_init(
        &sink->callback_gate.diagnostic.first_qovf_ring_wait_enter_count, 0U);
    atomic_init(
        &sink->callback_gate.diagnostic.first_qovf_ring_wait_resume_count, 0U);
    atomic_init(&sink->callback_gate.diagnostic.first_qovf_eof_notify_count,
                0U);
    atomic_init(
        &sink->callback_gate.diagnostic.first_qovf_eof_hpwoken_true_count, 0U);
    atomic_init(&sink->callback_gate.diagnostic.first_qovf_observed, 0U);
    atomic_init(&sink->callback_gate.diagnostic.first_qovf_observed_us, 0U);
    atomic_init(&sink->callback_gate.diagnostic.enable_stream_duration_us, 0U);
    atomic_init(&sink->callback_gate.diagnostic.codec_unmute_duration_us, 0U);
    atomic_init(&sink->callback_gate.diagnostic.startup_durations_valid, 0U);
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
    /* A successful production prepare contract includes PA-low entry,
     * muted codec initialization, I2S initialization, muted warm-up, and
     * callback registration.  These are history latches, not current state. */
    sink->prepare_completed = true;
    sink->pa_initial_low = true;
    sink->codec_initialized_muted = true;
    sink->i2s_initialized = true;
    sink->muted_warmup_completed = true;
    sink->callbacks_registered = true;
    sink->i2s_created = true;
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
    uint32_t enable_stream_us = 0U;
    uint32_t codec_unmute_us = 0U;
    atomic_store_explicit(&sink->state, P4_NANO_AUDIO86_PHYSICAL_STARTING,
                          memory_order_release);
    atomic_store_explicit(&sink->callbacks_active, 1U,
                          memory_order_release);
    diagnostic_set_phase(&sink->callback_gate.diagnostic,
                         P4_NANO_AUDIO86_CONSUMER_PHASE_START_ENABLE);
    if (sink->backend.enable_stream(sink->backend.opaque) != 0) {
        if (sink->backend.get_startup_durations != NULL) {
            sink->backend.get_startup_durations(
                sink->backend.opaque, &enable_stream_us, &codec_unmute_us);
            atomic_store_explicit(
                &sink->callback_gate.diagnostic.enable_stream_duration_us,
                enable_stream_us, memory_order_release);
            atomic_store_explicit(
                &sink->callback_gate.diagnostic.codec_unmute_duration_us,
                codec_unmute_us, memory_order_release);
            atomic_store_explicit(
                &sink->callback_gate.diagnostic.startup_durations_valid, 1U,
                memory_order_release);
        }
        atomic_store_explicit(&sink->callbacks_active, 0U,
                              memory_order_release);
        mark_failed(sink);
        return false;
    }
    if (sink->backend.get_startup_durations != NULL) {
        sink->backend.get_startup_durations(
            sink->backend.opaque, &enable_stream_us, &codec_unmute_us);
        atomic_store_explicit(
            &sink->callback_gate.diagnostic.enable_stream_duration_us,
            enable_stream_us, memory_order_release);
        atomic_store_explicit(
            &sink->callback_gate.diagnostic.codec_unmute_duration_us,
            codec_unmute_us, memory_order_release);
        atomic_store_explicit(
            &sink->callback_gate.diagnostic.startup_durations_valid, 1U,
            memory_order_release);
    }
    diagnostic_set_phase(&sink->callback_gate.diagnostic,
                         P4_NANO_AUDIO86_CONSUMER_PHASE_DOWNSTREAM_SUBMIT);
    sink->stream_started = true;
    sink->codec_unmute_completed = true;
    sink->i2s_enabled = true;
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
    if (sink != NULL) sink->submit_attempts++;
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
        sink->retry_count++;
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
    if (view->valid_frames == P4_NANO_AUDIO86_PHYSICAL_FRAMES_PER_UNIT) {
        sink->full_units++;
    } else {
        sink->final_partial_units++;
        sink->final_valid_frames = view->valid_frames;
    }
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
    sink->drain_duration_ms = sink->backend.now_ms(sink->backend.opaque) -
                              start_ms;
    disarm_callbacks(sink);
    if (sink->backend.mute(sink->backend.opaque) != 0) {
        mark_failed(sink);
        return NP2_PCM_SINK_FATAL;
    }
    sink->codec_final_muted = true;
    if (sink->backend.pa_low(sink->backend.opaque) != 0) {
        mark_failed(sink);
        return NP2_PCM_SINK_FATAL;
    }
    sink->pa_final_low = true;
    if (sink->backend.disable(sink->backend.opaque) != 0) {
        mark_failed(sink);
        return NP2_PCM_SINK_FATAL;
    }
    sink->i2s_enabled = false;
    if (!close_callbacks(sink)) {
        mark_failed(sink);
        return NP2_PCM_SINK_FATAL;
    }
    /* unregister_callbacks() is the IDF interrupt-delivery barrier and
     * close_callbacks() additionally observes in_flight == 0.  The valid
     * generation EOF writer is therefore permanently quiescent here. */
    sink->quiescent_eof_epoch = atomic_load_explicit(
        &sink->tx_eof_epoch, memory_order_acquire);
    sink->i2s_created = false;
    sink->physically_drained_frames += sink->accepted_pending_drain_frames;
    sink->accepted_pending_drain_frames = 0U;
    atomic_store_explicit(&sink->state, P4_NANO_AUDIO86_PHYSICAL_QUIESCENT,
                          memory_order_release);
    sink->finish_completed = true;
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
    else sink->codec_final_muted = true;
    if (sink->backend.pa_low(sink->backend.opaque) != 0) ok = false;
    else sink->pa_final_low = true;
    if (sink->backend.disable(sink->backend.opaque) != 0) ok = false;
    else sink->i2s_enabled = false;
    if (sink->backend.unregister_callbacks(sink->backend.opaque) != 0) {
        ok = false;
    } else {
        sink->i2s_created = false;
    }
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
        uint32_t notify_result;
        atomic_fetch_add_explicit(&sink->tx_eof_epoch, 1U,
                                  memory_order_release);
        notify_result = notify_waiter(sink, true);
        if ((notify_result & P4_NANO_AUDIO86_NOTIFY_ATTEMPTED) != 0U) {
            atomic_fetch_add_explicit(&gate->diagnostic.eof_notify_count, 1U,
                                      memory_order_relaxed);
        }
        if ((notify_result &
             P4_NANO_AUDIO86_NOTIFY_HIGHER_PRIORITY_WOKEN) != 0U) {
            atomic_fetch_add_explicit(
                &gate->diagnostic.eof_hpwoken_true_count, 1U,
                memory_order_relaxed);
        }
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
            struct p4_nano_audio86_diagnostic_publication *diagnostic =
                &sink->callback_gate.diagnostic;
            uint32_t expected = 0U;
            atomic_fetch_add_explicit(&sink->running_queue_overflow_count, 1U,
                                      memory_order_relaxed);
            if (atomic_compare_exchange_strong_explicit(
                    &diagnostic->first_qovf_latch_state, &expected, 1U,
                    memory_order_acq_rel, memory_order_acquire)) {
                atomic_store_explicit(
                    &diagnostic->first_qovf_state, (uint32_t)state,
                    memory_order_relaxed);
                atomic_store_explicit(
                    &diagnostic->first_qovf_eof_epoch,
                    atomic_load_explicit(&sink->tx_eof_epoch,
                                         memory_order_acquire),
                    memory_order_relaxed);
                atomic_store_explicit(
                    &diagnostic->first_qovf_service_word,
                    atomic_load_explicit(&diagnostic->service_word,
                                         memory_order_acquire),
                    memory_order_relaxed);
                atomic_store_explicit(
                    &diagnostic->first_qovf_last_step_enter_us,
                    atomic_load_explicit(&diagnostic->last_step_enter_us,
                                         memory_order_acquire),
                    memory_order_relaxed);
                atomic_store_explicit(
                    &diagnostic->first_qovf_last_submit_return_us,
                    atomic_load_explicit(&diagnostic->last_submit_return_us,
                                         memory_order_acquire),
                    memory_order_relaxed);
                atomic_store_explicit(
                    &diagnostic->first_qovf_last_step_exit_us,
                    atomic_load_explicit(&diagnostic->last_step_exit_us,
                                         memory_order_acquire),
                    memory_order_relaxed);
                atomic_store_explicit(
                    &diagnostic->first_qovf_last_running_accepted_us,
                    atomic_load_explicit(
                        &diagnostic->last_running_accepted_us,
                         memory_order_acquire),
                    memory_order_relaxed);
                atomic_store_explicit(
                    &diagnostic->first_qovf_ring_context_word,
                    atomic_load_explicit(&diagnostic->ring_context_word,
                                         memory_order_acquire),
                    memory_order_relaxed);
                atomic_store_explicit(
                    &diagnostic->first_qovf_wait_context_word,
                    atomic_load_explicit(&diagnostic->wait_context_word,
                                         memory_order_acquire),
                    memory_order_relaxed);
                atomic_store_explicit(
                    &diagnostic->first_qovf_last_wait_enter_us,
                    atomic_load_explicit(&diagnostic->last_wait_enter_us,
                                         memory_order_acquire),
                    memory_order_relaxed);
                atomic_store_explicit(
                    &diagnostic->first_qovf_last_wait_resume_us,
                    atomic_load_explicit(&diagnostic->last_wait_resume_us,
                                         memory_order_acquire),
                    memory_order_relaxed);
                atomic_store_explicit(
                    &diagnostic->first_qovf_last_wait_resume_context_word,
                    atomic_load_explicit(
                        &diagnostic->last_wait_resume_context_word,
                        memory_order_acquire),
                    memory_order_relaxed);
#define FREEZE_DIAGNOSTIC_COUNTER(name) \
                atomic_store_explicit( \
                    &diagnostic->first_qovf_##name, \
                    atomic_load_explicit(&diagnostic->name, \
                                         memory_order_acquire), \
                    memory_order_relaxed)
                FREEZE_DIAGNOSTIC_COUNTER(retry_wait_enter_count);
                FREEZE_DIAGNOSTIC_COUNTER(retry_wait_resume_count);
                FREEZE_DIAGNOSTIC_COUNTER(ring_wait_enter_count);
                FREEZE_DIAGNOSTIC_COUNTER(ring_wait_resume_count);
                FREEZE_DIAGNOSTIC_COUNTER(eof_notify_count);
                FREEZE_DIAGNOSTIC_COUNTER(eof_hpwoken_true_count);
#undef FREEZE_DIAGNOSTIC_COUNTER
                atomic_store_explicit(&diagnostic->first_qovf_latch_state, 2U,
                                      memory_order_release);
            }
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
    telemetry->submit_attempts = sink->submit_attempts;
    telemetry->retry_count = sink->retry_count;
    telemetry->drain_duration_ms = sink->drain_duration_ms;
    telemetry->accepted_pending_drain_frames =
        sink->accepted_pending_drain_frames;
    telemetry->physically_drained_frames = sink->physically_drained_frames;
    telemetry->physically_discarded_accepted_frames =
        sink->physically_discarded_accepted_frames;
    telemetry->full_units = sink->full_units;
    telemetry->final_partial_units = sink->final_partial_units;
    telemetry->final_valid_frames = sink->final_valid_frames;
    telemetry->tx_eof_epoch = atomic_load_explicit(
        &sink->tx_eof_epoch, memory_order_acquire);
    telemetry->drain_snapshot_epoch = sink->drain_snapshot_epoch;
    telemetry->drain_completion_epoch = sink->drain_completion_epoch;
    telemetry->quiescent_eof_epoch = sink->quiescent_eof_epoch;
    telemetry->registered_generation = atomic_load_explicit(
        &sink->callback_gate.registration_generation, memory_order_acquire);
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
    telemetry->enable_stream_duration_us = atomic_load_explicit(
        &sink->callback_gate.diagnostic.enable_stream_duration_us,
        memory_order_acquire);
    telemetry->codec_unmute_duration_us = atomic_load_explicit(
        &sink->callback_gate.diagnostic.codec_unmute_duration_us,
        memory_order_acquire);
    telemetry->startup_durations_valid = atomic_load_explicit(
        &sink->callback_gate.diagnostic.startup_durations_valid,
        memory_order_acquire);
    {
        const struct p4_nano_audio86_diagnostic_publication *diagnostic =
            &sink->callback_gate.diagnostic;
        const uint32_t latch_state = atomic_load_explicit(
            &diagnostic->first_qovf_latch_state, memory_order_acquire);
        const uint32_t service_word = latch_state == 2U
            ? atomic_load_explicit(&diagnostic->first_qovf_service_word,
                                   memory_order_acquire)
            : 0U;
        const uint32_t wait_word = latch_state == 2U
            ? atomic_load_explicit(&diagnostic->first_qovf_wait_context_word,
                                   memory_order_acquire)
            : 0U;
        const uint32_t ring_word = latch_state == 2U
            ? atomic_load_explicit(&diagnostic->first_qovf_ring_context_word,
                                   memory_order_acquire)
            : 0U;
        const uint32_t resume_word = latch_state == 2U
            ? atomic_load_explicit(
                  &diagnostic->first_qovf_last_wait_resume_context_word,
                  memory_order_acquire)
            : 0U;
        telemetry->first_active_qovf_latched = latch_state == 2U ? 1U : 0U;
        telemetry->first_qovf_state = latch_state == 2U
            ? atomic_load_explicit(&diagnostic->first_qovf_state,
                                   memory_order_acquire)
            : 0U;
        telemetry->first_qovf_eof_epoch = latch_state == 2U
            ? atomic_load_explicit(&diagnostic->first_qovf_eof_epoch,
                                   memory_order_acquire)
            : 0U;
        telemetry->first_qovf_phase = (uint32_t)diagnostic_phase(service_word);
        telemetry->first_qovf_current_sequence =
            diagnostic_current_sequence(service_word);
        telemetry->first_qovf_published_sequence =
            diagnostic_published_sequence(service_word);
        telemetry->first_qovf_last_step_enter_us = latch_state == 2U
            ? atomic_load_explicit(&diagnostic->first_qovf_last_step_enter_us,
                                   memory_order_acquire)
            : 0U;
        telemetry->first_qovf_last_submit_return_us = latch_state == 2U
            ? atomic_load_explicit(
                  &diagnostic->first_qovf_last_submit_return_us,
                  memory_order_acquire)
            : 0U;
        telemetry->first_qovf_last_step_exit_us = latch_state == 2U
            ? atomic_load_explicit(&diagnostic->first_qovf_last_step_exit_us,
                                   memory_order_acquire)
            : 0U;
        telemetry->first_qovf_last_running_accepted_us = latch_state == 2U
            ? atomic_load_explicit(
                  &diagnostic->first_qovf_last_running_accepted_us,
                  memory_order_acquire)
            : 0U;
        telemetry->first_qovf_wait_reason = diagnostic_wait_reason(wait_word);
        telemetry->first_qovf_consumer_next_sequence =
            diagnostic_wait_sequence(wait_word);
        telemetry->first_qovf_next_published_sequence =
            diagnostic_next_published_sequence(ring_word);
        telemetry->first_qovf_ring_occupancy =
            diagnostic_ring_occupancy(ring_word);
        telemetry->first_qovf_production_done =
            diagnostic_production_done(ring_word);
        telemetry->first_qovf_rendered_frames =
            diagnostic_rendered_frames(ring_word);
#define LOAD_FROZEN_DIAGNOSTIC_COUNTER(name) \
        telemetry->first_qovf_##name = latch_state == 2U \
            ? atomic_load_explicit(&diagnostic->first_qovf_##name, \
                                   memory_order_acquire) \
            : 0U
        LOAD_FROZEN_DIAGNOSTIC_COUNTER(eof_notify_count);
        LOAD_FROZEN_DIAGNOSTIC_COUNTER(retry_wait_enter_count);
        LOAD_FROZEN_DIAGNOSTIC_COUNTER(retry_wait_resume_count);
        LOAD_FROZEN_DIAGNOSTIC_COUNTER(ring_wait_enter_count);
        LOAD_FROZEN_DIAGNOSTIC_COUNTER(ring_wait_resume_count);
#undef LOAD_FROZEN_DIAGNOSTIC_COUNTER
        telemetry->first_qovf_hpwoken_true_count = latch_state == 2U
            ? atomic_load_explicit(
                  &diagnostic->first_qovf_eof_hpwoken_true_count,
                  memory_order_acquire)
            : 0U;
        telemetry->first_qovf_last_wait_enter_us = latch_state == 2U
            ? atomic_load_explicit(
                  &diagnostic->first_qovf_last_wait_enter_us,
                  memory_order_acquire)
            : 0U;
        telemetry->first_qovf_last_wait_resume_us = latch_state == 2U
            ? atomic_load_explicit(
                  &diagnostic->first_qovf_last_wait_resume_us,
                  memory_order_acquire)
            : 0U;
        telemetry->first_qovf_last_resume_reason =
            diagnostic_wait_reason(resume_word);
        telemetry->first_qovf_last_resume_sequence =
            diagnostic_wait_sequence(resume_word);
        telemetry->first_qovf_observed = atomic_load_explicit(
            &diagnostic->first_qovf_observed, memory_order_acquire) == 2U
            ? 1U : 0U;
        telemetry->first_qovf_observed_us = telemetry->first_qovf_observed
            ? atomic_load_explicit(&diagnostic->first_qovf_observed_us,
                                   memory_order_acquire)
            : 0U;
    }
    telemetry->state = load_state(sink);
    telemetry->prepare_completed = sink->prepare_completed;
    telemetry->pa_initial_low = sink->pa_initial_low;
    telemetry->codec_initialized_muted = sink->codec_initialized_muted;
    telemetry->i2s_initialized = sink->i2s_initialized;
    telemetry->muted_warmup_completed = sink->muted_warmup_completed;
    telemetry->callbacks_registered = sink->callbacks_registered;
    telemetry->stream_started = sink->stream_started;
    telemetry->codec_unmute_completed = sink->codec_unmute_completed;
    telemetry->finish_completed = sink->finish_completed;
    telemetry->codec_final_muted = sink->codec_final_muted;
    telemetry->pa_final_low = sink->pa_final_low;
    telemetry->i2s_enabled = sink->i2s_enabled;
    telemetry->i2s_created = sink->i2s_created;
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
