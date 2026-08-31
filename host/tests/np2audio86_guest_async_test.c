#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "np2audio86_fixture.h"
#include "np2audio86_guest_async.h"
#include "np2audio86_guest_runtime_capture.h"
#include "np2_crc32.h"
#include "np2_sha256.h"
#include "np2opngen_pcm_canonical.h"

#define ASYNC_MAX_EVENTS 64U
#define ASYNC_MAX_RUNS 8U
#define ASYNC_MAX_ACTIONS (ASYNC_MAX_EVENTS + ASYNC_MAX_RUNS)
#define ASYNC_HORIZON_FRAMES 2400U
#define ASYNC_TRACE_RECORD_BYTES 40U
#define ASYNC_SOURCE_BYTES NP2_AUDIO86_PCM86_SOURCE_PERIOD_BYTES

enum async_error {
    ASYNC_ERROR_NONE = 0,
    ASYNC_ERROR_ARGUMENT,
    ASYNC_ERROR_SEQUENCE,
    ASYNC_ERROR_TIMESTAMP,
    ASYNC_ERROR_TRANSPORT,
    ASYNC_ERROR_DISPATCH,
    ASYNC_ERROR_RENDER,
    ASYNC_ERROR_COMPLETION,
    ASYNC_ERROR_STOP,
};

struct async_apply_record {
    uint64_t frame;
    uint64_t sequence;
    uint32_t opcode;
    uint32_t action;
    uint64_t byte_offset;
    uint32_t byte_count;
    uint32_t payload;
};

struct async_result {
    uint8_t full_pcm[ASYNC_HORIZON_FRAMES * 4U];
    uint8_t pre_pcm[ASYNC_HORIZON_FRAMES * 4U];
    size_t full_bytes;
    size_t pre_bytes;
    uint64_t full_frames;
    uint64_t pre_frames;
    uint64_t full_peak;
    uint64_t pre_peak;
    uint64_t full_nonzero;
    uint64_t pre_nonzero;
    uint64_t full_first_nonzero;
    uint64_t pre_first_nonzero;
    uint64_t full_clamp;
    uint64_t pre_clamp;
    uint64_t highest_event_frame;
    uint64_t pre_reset_frame;
    struct async_apply_record apply[ASYNC_MAX_ACTIONS];
    size_t apply_count;
};

struct async_context {
    struct np2audio86_event_ring events;
    struct np2audio86_byte_ring bytes;
    struct np2audio86_render_state worker;
    uint8_t worker_source[ASYNC_SOURCE_BYTES];
    uint8_t worker_run[NP2_AUDIO86_ASYNC_MAX_DATA_RUN];
    struct async_result result;
    _Atomic int first_error;
    _Atomic bool stop;
    _Atomic bool producer_started;
    _Atomic bool producer_done;
    _Atomic bool producer_success;
    _Atomic bool worker_started;
    _Atomic bool worker_success;
    _Atomic bool worker_observed_producer;
    _Atomic bool producer_observed_worker;
    _Atomic bool reset_gate_reached;
    _Atomic bool reset_gate_release;
    _Atomic bool producer_reset_waiting;
    _Atomic uint64_t reset_ack_plus_one;
    uint64_t producer_next_sequence;
    uint64_t worker_next_sequence;
    uint64_t pending_run_sequence;
    uint64_t pending_run_frame;
    uint32_t pending_run_bytes;
    uint64_t byte_offset;
    uint64_t actions_published;
    uint64_t actions_consumed;
    uint64_t data_runs_published;
    uint64_t data_runs_consumed;
    uint64_t bytes_published;
    uint64_t bytes_consumed;
    uint64_t resets_published;
    uint64_t resets_acknowledged;
};

static void fail(struct async_context *context, enum async_error error);

#if defined(NP2_AUDIO86_GUEST_ASYNC_HARDENING_TEST)
/* The 86R.4B driver includes this host-only source and controls these gates
 * from an outer coordinator thread.  They deliberately pause only scheduling:
 * all guest semantics and the production transport implementation stay the
 * same. */
struct async_hardening_live_control {
    _Atomic bool enabled;
    _Atomic bool hold_worker;
    _Atomic bool worker_gate_reached;
    _Atomic bool event_full_claimed;
    _Atomic bool event_full_reached;
    _Atomic bool release_worker;
    _Atomic bool hold_reset;
    _Atomic bool reset_gate_reached;
    _Atomic bool release_reset;
    _Atomic bool inject_fatal;
    _Atomic bool abort;
    _Atomic bool pause_after_event_full_claim;
    _Atomic bool event_full_claim_pause_reached;
    _Atomic bool release_event_full_metadata;
    _Atomic bool position_changed;
    _Atomic uint32_t event_occupancy;
    _Atomic uint32_t event_tail;
    _Atomic uint64_t blocked_sequence;
    _Atomic uint32_t position_before;
    _Atomic uint32_t position_during;
    _Atomic uint32_t byte_wrap_count;
    uint32_t byte_empty_offset;
};

static struct async_hardening_live_control g_async_hardening_live;

static int hardening_enabled(void)
{
    return atomic_load_explicit(&g_async_hardening_live.enabled,
                                memory_order_acquire);
}

static int hardening_abort_requested(const struct async_context *context)
{
    (void)context;
    return atomic_load_explicit(&g_async_hardening_live.abort,
                                memory_order_acquire) ||
           atomic_load_explicit(&g_async_hardening_live.inject_fatal,
                                memory_order_acquire);
}

static int hardening_wait_worker(struct async_context *context)
{
    if (!hardening_enabled() ||
        !atomic_load_explicit(&g_async_hardening_live.hold_worker,
                              memory_order_acquire)) {
        return 0;
    }
    atomic_store_explicit(&g_async_hardening_live.worker_gate_reached, true,
                          memory_order_release);
    for (;;) {
        if (hardening_abort_requested(context)) {
            fprintf(stderr, "AUDIO86_GUEST_ASYNC_HARDENING_GATE_ABORT=worker\n");
            fail(context, ASYNC_ERROR_STOP);
            return -1;
        }
        if (atomic_load_explicit(&g_async_hardening_live.release_worker,
                                 memory_order_acquire)) return 0;
        sched_yield();
    }
}

static int hardening_hold_event_full(struct async_context *context,
                                     const struct np2audio86_event *event)
{
    bool expected = false;
    uint32_t before;
    if (!hardening_enabled() || event == NULL ||
        !atomic_load_explicit(&g_async_hardening_live.hold_worker,
                              memory_order_acquire) ||
        !atomic_compare_exchange_strong_explicit(
            &g_async_hardening_live.event_full_claimed, &expected, true,
            memory_order_acq_rel, memory_order_acquire)) {
        return 0;
    }
    if (atomic_load_explicit(&g_async_hardening_live.pause_after_event_full_claim,
                             memory_order_acquire)) {
        atomic_store_explicit(
            &g_async_hardening_live.event_full_claim_pause_reached, true,
            memory_order_release);
        for (;;) {
            if (hardening_abort_requested(context)) {
                fprintf(stderr, "AUDIO86_GUEST_ASYNC_HARDENING_GATE_ABORT=claim\n");
                fail(context, ASYNC_ERROR_STOP);
                return -1;
            }
            if (atomic_load_explicit(
                &g_async_hardening_live.release_event_full_metadata,
                memory_order_acquire)) break;
            sched_yield();
        }
    }
    before = np2audio86_guest_host_current_cpu_position();
    atomic_store_explicit(&g_async_hardening_live.event_occupancy,
                          np2audio86_event_ring_occupancy(&context->events),
                          memory_order_release);
    atomic_store_explicit(&g_async_hardening_live.event_tail,
                          atomic_load_explicit(&context->events.tail,
                                               memory_order_acquire),
                          memory_order_release);
    atomic_store_explicit(&g_async_hardening_live.blocked_sequence,
                          event->sequence, memory_order_release);
    atomic_store_explicit(&g_async_hardening_live.position_before, before,
                          memory_order_release);
    /* This final release publication authorizes the coordinator to consume
     * the complete snapshot above.  Claimed means only that this producer
     * owns the one-shot gate; it never authorizes a metadata read. */
    atomic_store_explicit(&g_async_hardening_live.event_full_reached, true,
                          memory_order_release);
    for (;;) {
        const uint32_t now = np2audio86_guest_host_current_cpu_position();
        atomic_store_explicit(&g_async_hardening_live.position_during, now,
                              memory_order_release);
        if (now != before) {
            atomic_store_explicit(&g_async_hardening_live.position_changed,
                                  true, memory_order_release);
        }
        if (hardening_abort_requested(context)) {
            fprintf(stderr, "AUDIO86_GUEST_ASYNC_HARDENING_GATE_ABORT=event-full\n");
            fail(context, ASYNC_ERROR_STOP);
            return -1;
        }
        if (atomic_load_explicit(&g_async_hardening_live.release_worker,
                                 memory_order_acquire)) return 0;
        sched_yield();
    }
}

static int hardening_wait_reset(struct async_context *context)
{
    if (!hardening_enabled() ||
        !atomic_load_explicit(&g_async_hardening_live.hold_reset,
                              memory_order_acquire)) {
        return 0;
    }
    atomic_store_explicit(&g_async_hardening_live.reset_gate_reached, true,
                          memory_order_release);
    for (;;) {
        if (hardening_abort_requested(context)) {
            fprintf(stderr, "AUDIO86_GUEST_ASYNC_HARDENING_GATE_ABORT=reset\n");
            fail(context, ASYNC_ERROR_STOP);
            return -1;
        }
        if (atomic_load_explicit(&g_async_hardening_live.release_reset,
                                 memory_order_acquire)) return 0;
        sched_yield();
    }
}
#endif

static void fail(struct async_context *context, enum async_error error)
{
    int expected = ASYNC_ERROR_NONE;
    (void)atomic_compare_exchange_strong_explicit(
        &context->first_error, &expected, error, memory_order_acq_rel,
        memory_order_acquire);
}

static int failed(const struct async_context *context)
{
    return atomic_load_explicit(&context->first_error, memory_order_acquire) !=
           ASYNC_ERROR_NONE;
}

static int stopped(const struct async_context *context)
{
    return atomic_load_explicit(&context->stop, memory_order_acquire);
}

static int wait_retry(const struct async_context *context)
{
    if (failed(context) || stopped(context)) {
        return -1;
    }
    sched_yield();
    return 0;
}

static int enqueue_event(struct async_context *context,
                         const struct np2audio86_event *event)
{
    for (;;) {
        const int status = np2audio86_event_ring_enqueue(&context->events, event);
        if (status == NP2_AUDIO86_TRANSPORT_OK) {
            return 0;
        }
        if (status != NP2_AUDIO86_TRANSPORT_FULL) {
            fail(context, ASYNC_ERROR_TRANSPORT);
            return -1;
        }
#if defined(NP2_AUDIO86_GUEST_ASYNC_HARDENING_TEST)
        if (hardening_hold_event_full(context, event) != 0) {
            return -1;
        }
#endif
        if (wait_retry(context) != 0) {
            return -1;
        }
    }
}

static int enqueue_byte(struct async_context *context, uint8_t value)
{
    for (;;) {
#if defined(NP2_AUDIO86_GUEST_ASYNC_HARDENING_TEST)
        uint32_t head_before;
        if (hardening_enabled()) {
            head_before = atomic_load_explicit(&context->bytes.head,
                                               memory_order_relaxed);
        }
#endif
        const int status = np2audio86_byte_ring_push(&context->bytes, &value, 1U);
        if (status == NP2_AUDIO86_TRANSPORT_OK) {
#if defined(NP2_AUDIO86_GUEST_ASYNC_HARDENING_TEST)
            if (hardening_enabled() &&
                (head_before & (NP2_AUDIO86_ASYNC_BYTE_CAPACITY - 1U)) ==
                    NP2_AUDIO86_ASYNC_BYTE_CAPACITY - 1U) {
                atomic_fetch_add_explicit(&g_async_hardening_live.byte_wrap_count,
                                          1U, memory_order_relaxed);
            }
#endif
            return 0;
        }
        if (status != NP2_AUDIO86_TRANSPORT_FULL) {
            fail(context, ASYNC_ERROR_TRANSPORT);
            return -1;
        }
        if (wait_retry(context) != 0) {
            return -1;
        }
    }
}

static int publish_pcm_byte(void *opaque, uint64_t frame_timestamp,
                            uint64_t sequence, uint8_t value)
{
    struct async_context *context = opaque;
    if (context == NULL || sequence != context->producer_next_sequence ||
        (context->pending_run_bytes != 0U &&
         (context->pending_run_sequence != sequence ||
          context->pending_run_frame != frame_timestamp)) ||
        context->pending_run_bytes >= NP2_AUDIO86_ASYNC_MAX_DATA_RUN) {
        if (context != NULL) fail(context, ASYNC_ERROR_SEQUENCE);
        return -1;
    }
    if (enqueue_byte(context, value) != 0) {
        return -1;
    }
    context->pending_run_sequence = sequence;
    context->pending_run_frame = frame_timestamp;
    ++context->pending_run_bytes;
    ++context->bytes_published;
    return 0;
}

static int publish_data_run(void *opaque, const np2audio86_guest_data_run_t *run)
{
    struct async_context *context = opaque;
    struct np2audio86_event event;
    if (context == NULL || run == NULL ||
        run->sequence != context->producer_next_sequence || run->count == 0U ||
        run->count > NP2_AUDIO86_ASYNC_MAX_DATA_RUN ||
        context->pending_run_sequence != run->sequence ||
        context->pending_run_frame != run->frame_timestamp ||
        context->pending_run_bytes != run->count) {
        if (context != NULL) fail(context, ASYNC_ERROR_SEQUENCE);
        return -1;
    }
    event.frame_timestamp = run->frame_timestamp;
    event.sequence = run->sequence;
    event.opcode = NP2_AUDIO86_GUEST_TRANSPORT_DATA_RUN;
    event.payload = run->count;
    if (enqueue_event(context, &event) != 0) {
        return -1;
    }
    ++context->producer_next_sequence;
    ++context->actions_published;
    ++context->data_runs_published;
    context->pending_run_bytes = 0U;
    return 0;
}

static int publish_event(void *opaque, const np2audio86_guest_event_t *guest)
{
    struct async_context *context = opaque;
    struct np2audio86_event event;
    uint8_t kind;
    if (context == NULL || guest == NULL ||
        guest->sequence != context->producer_next_sequence ||
        np2audio86_guest_action_kind_for_opcode(guest->opcode, &kind) != 0) {
        if (context != NULL) fail(context, ASYNC_ERROR_SEQUENCE);
        return -1;
    }
    event.frame_timestamp = guest->frame_timestamp;
    event.sequence = guest->sequence;
    event.opcode = guest->opcode;
    event.payload = guest->payload;
    if (atomic_load_explicit(&context->worker_started, memory_order_acquire)) {
        atomic_store_explicit(&context->producer_observed_worker, true,
                              memory_order_release);
    }
    if (enqueue_event(context, &event) != 0) {
        return -1;
    }
    ++context->producer_next_sequence;
    ++context->actions_published;
    if (kind != NP2_AUDIO86_GUEST_ACTION_RESET) {
        return 0;
    }
    ++context->resets_published;
    atomic_store_explicit(&context->producer_reset_waiting, true,
                          memory_order_release);
    for (;;) {
        if (atomic_load_explicit(&context->reset_ack_plus_one,
                                 memory_order_acquire) == guest->sequence + 1U) {
            atomic_store_explicit(&context->producer_reset_waiting, false,
                                  memory_order_release);
            return 0;
        }
        if (wait_retry(context) != 0) {
            atomic_store_explicit(&context->producer_reset_waiting, false,
                                  memory_order_release);
            return -1;
        }
    }
}

static void put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
    out[2] = (uint8_t)(value >> 16U);
    out[3] = (uint8_t)(value >> 24U);
}

static void put_le64(uint8_t *out, uint64_t value)
{
    unsigned i;
    for (i = 0U; i < 8U; ++i) {
        out[i] = (uint8_t)(value >> (i * 8U));
    }
}

static size_t serialize_events(const np2audio86_guest_event_t *events,
                               size_t count, uint8_t *out)
{
    size_t i;
    for (i = 0U; i < count; ++i) {
        put_le64(out + i * 24U, events[i].frame_timestamp);
        put_le64(out + i * 24U + 8U, events[i].sequence);
        put_le32(out + i * 24U + 16U, events[i].opcode);
        put_le32(out + i * 24U + 20U, events[i].payload);
    }
    return count * 24U;
}

static size_t serialize_runs(const np2audio86_guest_data_run_t *runs,
                             size_t count, uint8_t *out)
{
    size_t i;
    for (i = 0U; i < count; ++i) {
        put_le64(out + i * 32U, runs[i].frame_timestamp);
        put_le64(out + i * 32U + 8U, runs[i].sequence);
        put_le64(out + i * 32U + 16U, runs[i].byte_offset);
        put_le32(out + i * 32U + 24U, runs[i].count);
        put_le32(out + i * 32U + 28U, 0U);
    }
    return count * 32U;
}

static size_t serialize_apply(const struct async_apply_record *records,
                              size_t count, uint8_t *out)
{
    size_t i;
    for (i = 0U; i < count; ++i) {
        const struct async_apply_record *record = &records[i];
        put_le64(out + i * ASYNC_TRACE_RECORD_BYTES, record->frame);
        put_le64(out + i * ASYNC_TRACE_RECORD_BYTES + 8U, record->sequence);
        put_le32(out + i * ASYNC_TRACE_RECORD_BYTES + 16U, record->opcode);
        put_le32(out + i * ASYNC_TRACE_RECORD_BYTES + 20U, record->action);
        put_le64(out + i * ASYNC_TRACE_RECORD_BYTES + 24U,
                 record->byte_offset);
        put_le32(out + i * ASYNC_TRACE_RECORD_BYTES + 32U,
                 record->byte_count);
        put_le32(out + i * ASYNC_TRACE_RECORD_BYTES + 36U, record->payload);
    }
    return count * ASYNC_TRACE_RECORD_BYTES;
}

static void print_digest(const char *name, const uint8_t *bytes, size_t count)
{
    np2_sha256_context sha;
    uint8_t digest[NP2_SHA256_DIGEST_SIZE];
    size_t i;
    printf("%s_SERIALIZED_BYTES=%zu\n", name, count);
    printf("%s_CRC32=%08" PRIx32 "\n", name,
           np2_crc32_iso_hdlc(bytes, count));
    np2_sha256_init(&sha);
    np2_sha256_update(&sha, bytes, count);
    np2_sha256_final(&sha, digest);
    printf("%s_SHA256=", name);
    for (i = 0U; i < sizeof(digest); ++i) {
        printf("%02x", digest[i]);
    }
    putchar('\n');
}

static int render_chunk(struct async_context *context, uint64_t frame,
                        size_t frames, uint8_t pre_reset)
{
    SINT32 mix[NP2_AUDIO86_QUANTUM_FRAMES * 2U];
    uint8_t canonical[NP2_AUDIO86_QUANTUM_FRAMES * 4U];
    struct np2audio86_fixture_result fixture_result;
    struct np2opngen_pcm_stats stats;
    size_t i;
    if (frames == 0U || frames > NP2_AUDIO86_QUANTUM_FRAMES ||
        frame + frames > ASYNC_HORIZON_FRAMES) {
        return -1;
    }
    memset(mix, 0, sizeof(mix));
    memset(&fixture_result, 0, sizeof(fixture_result));
    if (np2audio86_render_span(&context->worker, mix, frames,
                               &fixture_result) != 0 ||
        np2opngen_pcm_canonicalize_s16le(mix, frames, 2U, canonical,
                                         sizeof(canonical), &stats) != 0) {
        return -1;
    }
    memcpy(context->result.full_pcm + frame * 4U, canonical, frames * 4U);
    if (pre_reset) {
        memcpy(context->result.pre_pcm + frame * 4U, canonical, frames * 4U);
    }
    if (stats.s32_abs_peak > context->result.full_peak) {
        context->result.full_peak = stats.s32_abs_peak;
    }
    if (pre_reset && stats.s32_abs_peak > context->result.pre_peak) {
        context->result.pre_peak = stats.s32_abs_peak;
    }
    context->result.full_clamp += stats.clip_samples;
    if (pre_reset) {
        context->result.pre_clamp += stats.clip_samples;
    }
    for (i = 0U; i < frames; ++i) {
        const uint8_t nonzero = canonical[i * 4U] != 0U ||
                                canonical[i * 4U + 1U] != 0U ||
                                canonical[i * 4U + 2U] != 0U ||
                                canonical[i * 4U + 3U] != 0U;
        if (nonzero) {
            ++context->result.full_nonzero;
            if (context->result.full_nonzero == 1U) {
                context->result.full_first_nonzero = frame + i;
            }
            if (pre_reset) {
                ++context->result.pre_nonzero;
                if (context->result.pre_nonzero == 1U) {
                    context->result.pre_first_nonzero = frame + i;
                }
            }
        }
    }
    return 0;
}

static int render_to(struct async_context *context, uint64_t *frame,
                     uint64_t target, uint8_t pre_reset)
{
    while (*frame < target) {
        size_t chunk = (size_t)(target - *frame);
        if (chunk > NP2_AUDIO86_QUANTUM_FRAMES) {
            chunk = NP2_AUDIO86_QUANTUM_FRAMES;
        }
        if (render_chunk(context, *frame, chunk, pre_reset) != 0) {
            return -1;
        }
        *frame += chunk;
    }
    return 0;
}

static int wait_reset_gate(struct async_context *context)
{
    atomic_store_explicit(&context->reset_gate_reached, true,
                          memory_order_release);
    while (!atomic_load_explicit(&context->reset_gate_release,
                                 memory_order_acquire)) {
        if (wait_retry(context) != 0) {
            return -1;
        }
    }
    return 0;
}

static int worker_apply_one(struct async_context *context,
                            const struct np2audio86_event *event,
                            uint8_t *reset_seen)
{
    struct np2audio86_guest_action action;
    const uint8_t *data = NULL;
    size_t data_count = 0U;
    uint8_t kind;
    if (event->sequence != context->worker_next_sequence ||
        event->frame_timestamp < context->worker.rendered_frames ||
        context->result.apply_count >= ASYNC_MAX_ACTIONS) {
        return -1;
    }
    memset(&action, 0, sizeof(action));
    action.frame_timestamp = event->frame_timestamp;
    action.sequence = event->sequence;
    action.payload = event->payload;
    if (event->opcode == NP2_AUDIO86_GUEST_TRANSPORT_DATA_RUN) {
        if (event->payload == 0U || event->payload > NP2_AUDIO86_ASYNC_MAX_DATA_RUN ||
            np2audio86_byte_ring_copy(&context->bytes, context->worker_run,
                                      event->payload) != NP2_AUDIO86_TRANSPORT_OK) {
            return -1;
        }
        action.opcode = NP2AUDIO86_TRACE_PCM;
        /* The canonical 86R.3 data-run action carries its length solely in
         * byte_count; the transport event payload is intentionally not part
         * of the worker semantic trace. */
        action.payload = 0U;
        action.byte_offset = context->byte_offset;
        action.byte_count = event->payload;
        action.kind = NP2_AUDIO86_GUEST_ACTION_DATA_RUN;
        data = context->worker_run;
        data_count = event->payload;
    } else {
        action.opcode = event->opcode;
        if (np2audio86_guest_action_kind_for_opcode(event->opcode, &kind) != 0) {
            return -1;
        }
        action.kind = kind;
        if (kind == NP2_AUDIO86_GUEST_ACTION_RESET) {
            if (*reset_seen || wait_reset_gate(context) != 0) {
                return -1;
            }
        }
    }
    context->result.apply[context->result.apply_count++] =
        (struct async_apply_record){
            action.frame_timestamp, action.sequence, action.opcode, action.kind,
            action.byte_offset, action.byte_count, action.payload
        };
    if (np2audio86_guest_action_apply(&context->worker, &action, data,
                                      data_count, context->worker_source,
                                      sizeof(context->worker_source)) != 0) {
        return -1;
    }
    if (action.kind == NP2_AUDIO86_GUEST_ACTION_DATA_RUN) {
        if (np2audio86_byte_ring_consume(&context->bytes, data_count) !=
            NP2_AUDIO86_TRANSPORT_OK) {
            return -1;
        }
        context->byte_offset += data_count;
        context->bytes_consumed += data_count;
        ++context->data_runs_consumed;
    }
    if (action.kind == NP2_AUDIO86_GUEST_ACTION_RESET) {
        *reset_seen = 1U;
        context->result.pre_reset_frame = action.frame_timestamp;
        atomic_store_explicit(&context->reset_ack_plus_one,
                              action.sequence + 1U, memory_order_release);
        ++context->resets_acknowledged;
    }
    if (np2audio86_event_ring_consume(&context->events) !=
        NP2_AUDIO86_TRANSPORT_OK) {
        return -1;
    }
    ++context->worker_next_sequence;
    ++context->actions_consumed;
    return 0;
}

static void *worker_thread(void *opaque)
{
    struct async_context *context = opaque;
    uint64_t frame = 0U;
    uint8_t reset_seen = 0U;
    if (np2audio86_guest_action_prime_worker(&context->worker,
                                             context->worker_source,
                                             sizeof(context->worker_source)) != 0) {
        fail(context, ASYNC_ERROR_RENDER);
        return NULL;
    }
    atomic_store_explicit(&context->worker_started, true, memory_order_release);
#if defined(NP2_AUDIO86_GUEST_ASYNC_HARDENING_TEST)
    if (hardening_wait_worker(context) != 0) {
        return NULL;
    }
#endif
    for (;;) {
        const struct np2audio86_event *event = NULL;
        uint64_t event_frame;
        int status;
        if (atomic_load_explicit(&context->producer_started,
                                 memory_order_acquire)) {
            atomic_store_explicit(&context->worker_observed_producer, true,
                                  memory_order_release);
        }
        if (failed(context) || stopped(context)) {
            return NULL;
        }
        status = np2audio86_event_ring_peek(&context->events, &event);
        if (status == NP2_AUDIO86_TRANSPORT_EMPTY) {
            if (atomic_load_explicit(&context->producer_done,
                                     memory_order_acquire)) {
                break;
            }
            sched_yield();
            continue;
        }
        if (status != NP2_AUDIO86_TRANSPORT_OK || event == NULL ||
            event->frame_timestamp > ASYNC_HORIZON_FRAMES) {
            fail(context, status == NP2_AUDIO86_TRANSPORT_OK
                              ? ASYNC_ERROR_DISPATCH : ASYNC_ERROR_TRANSPORT);
            return NULL;
        }
        event_frame = event->frame_timestamp;
        if (render_to(context, &frame, event_frame, !reset_seen) != 0 ||
            worker_apply_one(context, event, &reset_seen) != 0) {
            fail(context, ASYNC_ERROR_DISPATCH);
            return NULL;
        }
        if (event_frame > context->result.highest_event_frame) {
            context->result.highest_event_frame = event_frame;
        }
    }
    if (context->worker_next_sequence != 19U ||
        !atomic_load_explicit(&context->producer_success, memory_order_acquire) ||
        atomic_load_explicit(&context->reset_ack_plus_one, memory_order_acquire) !=
            19U || np2audio86_event_ring_occupancy(&context->events) != 0U ||
        np2audio86_byte_ring_occupancy(&context->bytes) != 0U ||
        render_to(context, &frame, ASYNC_HORIZON_FRAMES, !reset_seen) != 0) {
        fail(context, ASYNC_ERROR_COMPLETION);
        return NULL;
    }
    context->result.full_frames = ASYNC_HORIZON_FRAMES;
    context->result.full_bytes = sizeof(context->result.full_pcm);
    context->result.pre_frames = context->result.pre_reset_frame;
    context->result.pre_bytes = (size_t)context->result.pre_frames * 4U;
    atomic_store_explicit(&context->worker_success, true, memory_order_release);
    return NULL;
}

struct producer_context {
    struct async_context *transport;
    np2audio86_guest_trace_t *trace;
    np2audio86_guest_state_snapshot_t *state;
};

static void *producer_thread(void *opaque)
{
    struct producer_context *producer = opaque;
    const np2audio86_guest_sink_t sink = {
        producer->transport,
        publish_event,
        publish_pcm_byte,
        publish_data_run,
    };
    atomic_store_explicit(&producer->transport->producer_started, true,
                          memory_order_release);
    if (np2audio86_guest_runtime_live(producer->trace, producer->state, &sink) ==
        0 && !failed(producer->transport) && !stopped(producer->transport)) {
        atomic_store_explicit(&producer->transport->producer_success, true,
                              memory_order_release);
    } else {
        fail(producer->transport, ASYNC_ERROR_DISPATCH);
    }
    atomic_store_explicit(&producer->transport->producer_done, true,
                          memory_order_release);
    return NULL;
}

static int wait_for(const _Atomic bool *value)
{
    unsigned i;
    for (i = 0U; i < 1000000U; ++i) {
#if defined(NP2_AUDIO86_GUEST_ASYNC_HARDENING_TEST)
        if (hardening_enabled() &&
            atomic_load_explicit(&g_async_hardening_live.abort,
                                 memory_order_acquire)) {
            return -1;
        }
#endif
        if (atomic_load_explicit(value, memory_order_acquire)) {
            return 0;
        }
        sched_yield();
    }
    return -1;
}

static int inputs_unchanged(const np2audio86_guest_trace_t *trace,
                            const uint8_t *events, size_t event_bytes,
                            const uint8_t *runs, size_t run_bytes,
                            const uint8_t *pcm, size_t pcm_bytes)
{
    uint8_t actual_events[ASYNC_MAX_EVENTS * 24U];
    uint8_t actual_runs[ASYNC_MAX_RUNS * 32U];
    return serialize_events(trace->events, trace->event_count, actual_events) ==
               event_bytes &&
           serialize_runs(trace->data_runs, trace->data_run_count, actual_runs) ==
               run_bytes && trace->pcm_count == pcm_bytes &&
           memcmp(events, actual_events, event_bytes) == 0 &&
           memcmp(runs, actual_runs, run_bytes) == 0 &&
           memcmp(pcm, trace->pcm_bytes, pcm_bytes) == 0;
}

int main(void)
{
    static struct async_context context;
    static np2audio86_guest_event_t events[ASYNC_MAX_EVENTS];
    static np2audio86_guest_data_run_t runs[ASYNC_MAX_RUNS];
    static uint8_t pcm[32768U];
    static np2audio86_guest_timer_trace_t timers[4096U];
    static np2audio86_guest_io_trace_t io[16384U];
    uint8_t event_copy[ASYNC_MAX_EVENTS * 24U];
    uint8_t run_copy[ASYNC_MAX_RUNS * 32U];
    uint8_t pcm_copy[32768U];
    uint8_t apply[ASYNC_MAX_ACTIONS * ASYNC_TRACE_RECORD_BYTES];
    np2audio86_guest_trace_t trace = {
        events, ASYNC_MAX_EVENTS, 0U, runs, ASYNC_MAX_RUNS, 0U,
        pcm, sizeof(pcm), 0U, timers, 4096U, 0U, io, 16384U, 0U, 0U
    };
    np2audio86_guest_state_snapshot_t state;
    struct producer_context producer = {&context, &trace, &state};
    pthread_t worker;
    pthread_t guest;
    size_t event_bytes;
    size_t run_bytes;
    size_t apply_bytes;
    int worker_created = 0;
    int guest_created = 0;
    int reset_blocked = 0;

    memset(&context, 0, sizeof(context));
    np2audio86_event_ring_init(&context.events);
    np2audio86_byte_ring_init(&context.bytes);
#if defined(NP2_AUDIO86_GUEST_ASYNC_HARDENING_TEST)
    if (hardening_enabled() &&
        g_async_hardening_live.byte_empty_offset != 0U) {
        const uint32_t offset = g_async_hardening_live.byte_empty_offset;
        atomic_store_explicit(&context.bytes.head, offset, memory_order_relaxed);
        atomic_store_explicit(&context.bytes.tail, offset, memory_order_relaxed);
    }
#endif
    atomic_init(&context.first_error, ASYNC_ERROR_NONE);
    atomic_init(&context.stop, false);
    atomic_init(&context.producer_started, false);
    atomic_init(&context.producer_done, false);
    atomic_init(&context.producer_success, false);
    atomic_init(&context.worker_started, false);
    atomic_init(&context.worker_success, false);
    atomic_init(&context.worker_observed_producer, false);
    atomic_init(&context.producer_observed_worker, false);
    atomic_init(&context.reset_gate_reached, false);
    atomic_init(&context.reset_gate_release, false);
    atomic_init(&context.producer_reset_waiting, false);
    atomic_init(&context.reset_ack_plus_one, 0U);

    if (pthread_create(&worker, NULL, worker_thread, &context) != 0) {
        fail(&context, ASYNC_ERROR_ARGUMENT);
        goto done;
    }
    worker_created = 1;
    if (wait_for(&context.worker_started) != 0 ||
        pthread_create(&guest, NULL, producer_thread, &producer) != 0) {
        fail(&context, ASYNC_ERROR_ARGUMENT);
        goto done;
    }
    guest_created = 1;
    if (wait_for(&context.reset_gate_reached) != 0 ||
        wait_for(&context.producer_reset_waiting) != 0 ||
        atomic_load_explicit(&context.producer_done, memory_order_acquire)) {
        fail(&context, ASYNC_ERROR_COMPLETION);
        goto done;
    }
    reset_blocked = 1;
#if defined(NP2_AUDIO86_GUEST_ASYNC_HARDENING_TEST)
    if (hardening_wait_reset(&context) != 0) {
        goto done;
    }
#endif
    atomic_store_explicit(&context.reset_gate_release, true, memory_order_release);

done:
    atomic_store_explicit(&context.reset_gate_release, true, memory_order_release);
    if (failed(&context)) {
        atomic_store_explicit(&context.stop, true, memory_order_release);
    }
    if (guest_created) {
        (void)pthread_join(guest, NULL);
    }
    if (worker_created) {
        (void)pthread_join(worker, NULL);
    }
    if (failed(&context) || !atomic_load_explicit(&context.producer_success,
                                                   memory_order_acquire) ||
        !atomic_load_explicit(&context.worker_success, memory_order_acquire) ||
        !reset_blocked || trace.event_count != 18U || trace.data_run_count != 1U ||
        trace.pcm_count != 8U || context.actions_published != 19U ||
        context.actions_consumed != 19U || context.data_runs_published != 1U ||
        context.data_runs_consumed != 1U || context.bytes_published != 8U ||
        context.bytes_consumed != 8U || context.resets_published != 1U ||
        context.resets_acknowledged != 1U || context.worker_next_sequence != 19U ||
        np2audio86_event_ring_occupancy(&context.events) != 0U ||
        np2audio86_byte_ring_occupancy(&context.bytes) != 0U) {
        return 1;
    }
    event_bytes = serialize_events(trace.events, trace.event_count, event_copy);
    run_bytes = serialize_runs(trace.data_runs, trace.data_run_count, run_copy);
    memcpy(pcm_copy, trace.pcm_bytes, trace.pcm_count);
    apply_bytes = serialize_apply(context.result.apply, context.result.apply_count,
                                  apply);
    if (event_bytes != 432U || run_bytes != 32U || apply_bytes != 760U ||
        !inputs_unchanged(&trace, event_copy, event_bytes, run_copy, run_bytes,
                          pcm_copy, trace.pcm_count) ||
        context.result.pre_frames != 13U || context.result.pre_bytes != 52U ||
        context.result.full_frames != 2400U || context.result.full_bytes != 9600U ||
        context.result.full_clamp != 0U || context.result.pre_clamp != 0U ||
        !atomic_load_explicit(&context.worker_observed_producer,
                              memory_order_acquire) ||
        !atomic_load_explicit(&context.producer_observed_worker,
                              memory_order_acquire)) {
        return 1;
    }
#if !defined(NP2_AUDIO86_GUEST_ASYNC_HARDENING_TEST)
    printf("AUDIO86_GUEST_ASYNC_LIVE_I286_PRODUCER=PASS\n");
    printf("AUDIO86_GUEST_ASYNC_EVENT_TRANSPORT=PASS\n");
    printf("AUDIO86_GUEST_ASYNC_PCM_BYTES=PASS\n");
    printf("AUDIO86_GUEST_ASYNC_GLOBAL_ORDER=PASS\n");
    printf("AUDIO86_GUEST_ASYNC_RESET_ACK=PASS\n");
    printf("AUDIO86_GUEST_ASYNC_FINALIZE=PASS\n");
    printf("AUDIO86_GUEST_ASYNC_DOMAIN_OWNERSHIP=PASS\n");
    printf("AUDIO86_GUEST_ASYNC_WORKER_TRACE=PASS\n");
    printf("AUDIO86_GUEST_ASYNC_PRE_RESET_PCM=PASS\n");
    printf("AUDIO86_GUEST_ASYNC_FULL_PCM=PASS\n");
    printf("AUDIO86_GUEST_ASYNC_86R3_EXACT=PASS\n");
    printf("AUDIO86_GUEST_ASYNC_DETERMINISM=PASS\n");
    printf("AUDIO86_GUEST_ASYNC_INPUT_IMMUTABLE=PASS\n");
    printf("AUDIO86_GUEST_ASYNC_ACTIONS_PUBLISHED=%" PRIu64 "\n",
           context.actions_published);
    printf("AUDIO86_GUEST_ASYNC_ACTIONS_CONSUMED=%" PRIu64 "\n",
           context.actions_consumed);
    printf("AUDIO86_GUEST_ASYNC_DATA_RUNS_PUBLISHED=%" PRIu64 "\n",
           context.data_runs_published);
    printf("AUDIO86_GUEST_ASYNC_DATA_RUNS_CONSUMED=%" PRIu64 "\n",
           context.data_runs_consumed);
    printf("AUDIO86_GUEST_ASYNC_BYTES_PUBLISHED=%" PRIu64 "\n",
           context.bytes_published);
    printf("AUDIO86_GUEST_ASYNC_BYTES_CONSUMED=%" PRIu64 "\n",
           context.bytes_consumed);
    printf("AUDIO86_GUEST_ASYNC_RESETS_PUBLISHED=%" PRIu64 "\n",
           context.resets_published);
    printf("AUDIO86_GUEST_ASYNC_RESETS_ACKNOWLEDGED=%" PRIu64 "\n",
           context.resets_acknowledged);
    printf("AUDIO86_GUEST_ASYNC_FINAL_NEXT_SEQUENCE=%" PRIu64 "\n",
           context.worker_next_sequence);
    printf("AUDIO86_GUEST_ASYNC_EVENT_RESIDUAL=%" PRIu32 "\n",
           np2audio86_event_ring_occupancy(&context.events));
    printf("AUDIO86_GUEST_ASYNC_BYTE_RESIDUAL=%" PRIu32 "\n",
           np2audio86_byte_ring_occupancy(&context.bytes));
    print_digest("AUDIO_EVENTS", event_copy, event_bytes);
    printf("AUDIO_EVENTS_SEMANTIC_COUNT=%zu\n", trace.event_count);
    print_digest("PCM86_BYTES", pcm_copy, trace.pcm_count);
    printf("PCM86_BYTES_PAYLOAD_BYTES=%zu\n", trace.pcm_count);
    print_digest("PCM86_DATA_RUNS", run_copy, run_bytes);
    printf("PCM86_DATA_RUNS_SEMANTIC_COUNT=%zu\n"
           "PCM86_DATA_RUNS_PAYLOAD_BYTES=%zu\n", trace.data_run_count,
           trace.pcm_count);
    print_digest("WORKER_APPLY_TRACE", apply, apply_bytes);
    printf("WORKER_APPLY_TRACE_SEMANTIC_COUNT=%zu\n", context.result.apply_count);
    printf("HIGHEST_EVENT_FRAME=%" PRIu64 "\nRENDER_HORIZON_FRAMES=%u\n"
           "TAIL_FRAMES=%" PRIu64 "\n", context.result.highest_event_frame,
           ASYNC_HORIZON_FRAMES,
           (uint64_t)ASYNC_HORIZON_FRAMES - context.result.pre_reset_frame);
    printf("PRE_RESET_PCM_FRAMES=%" PRIu64 "\nPRE_RESET_PCM_BYTES=%zu\n",
           context.result.pre_frames, context.result.pre_bytes);
    print_digest("PRE_RESET_PCM", context.result.pre_pcm, context.result.pre_bytes);
    printf("PRE_RESET_PCM_PEAK=%" PRIu64 "\nPRE_RESET_PCM_NONZERO=%" PRIu64
           "\nPRE_RESET_PCM_FIRST_NONZERO=%" PRIu64
           "\nPRE_RESET_PCM_CLAMP=%" PRIu64 "\n", context.result.pre_peak,
           context.result.pre_nonzero, context.result.pre_first_nonzero,
           context.result.pre_clamp);
    printf("FULL_REPLAY_PCM_FRAMES=%" PRIu64 "\nFULL_REPLAY_PCM_BYTES=%zu\n",
           context.result.full_frames, context.result.full_bytes);
    print_digest("FULL_REPLAY_PCM", context.result.full_pcm,
                 context.result.full_bytes);
    printf("FULL_REPLAY_PCM_PEAK=%" PRIu64 "\nFULL_REPLAY_PCM_NONZERO=%" PRIu64
           "\nFULL_REPLAY_PCM_FIRST_NONZERO=%" PRIu64
           "\nFULL_REPLAY_PCM_CLAMP=%" PRIu64 "\n", context.result.full_peak,
           context.result.full_nonzero, context.result.full_first_nonzero,
           context.result.full_clamp);
    printf("FM_COVERAGE=EXERCISED\nPSG_COVERAGE=EXERCISED\n"
           "RHYTHM_COVERAGE=EXERCISED\nPCM86_COVERAGE=NOT_EXERCISED\n");
    printf("AUDIO86_GUEST_ASYNC_RESULT=PASS\n");
#endif
    return 0;
}
