#include "np2opngen_e1b_stream.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define E1B_RATE_HZ 48000U
#define E1B_CHANNELS 2U
#define E1B_PCM_BYTES_PER_SAMPLE 2U

void np2opngen_e1b_control_init(struct np2opngen_e1b_control *control)
{
    if (control == 0) {
        return;
    }
    atomic_init(&control->first_error, NP2_OPNGEN_E1B_ERROR_NONE);
    atomic_init(&control->producer_done, false);
}

void np2opngen_e1b_control_fail(struct np2opngen_e1b_control *control,
                                enum np2opngen_e1b_error error)
{
    int expected = NP2_OPNGEN_E1B_ERROR_NONE;
    if (control == 0 || error == NP2_OPNGEN_E1B_ERROR_NONE) {
        return;
    }
    (void)atomic_compare_exchange_strong_explicit(
        &control->first_error, &expected, (int)error, memory_order_acq_rel,
        memory_order_acquire);
}

int np2opngen_e1b_control_first_error(
    const struct np2opngen_e1b_control *control)
{
    return control == 0
               ? NP2_OPNGEN_E1B_ERROR_ARGUMENT
               : atomic_load_explicit(&control->first_error,
                                      memory_order_acquire);
}

void np2opngen_e1b_control_producer_done(
    struct np2opngen_e1b_control *control)
{
    if (control != 0) {
        atomic_store_explicit(&control->producer_done, true,
                              memory_order_release);
    }
}

static void worker_fail(struct np2opngen_e1b_worker *worker,
                        enum np2opngen_e1b_error error, int status)
{
    np2opngen_e1b_control_fail(worker->control, error);
    worker->failure_status = status;
    worker->state = NP2_OPNGEN_E1B_FAILED;
}

int np2opngen_e1b_worker_init_with_sink(
    struct np2opngen_e1b_worker *worker,
    struct np2opngen_spsc_queue *queue,
    struct np2opngen_e1b_control *control,
    uint64_t end_frame, uint64_t expected_first_sequence,
    uint64_t expected_event_count,
    const struct np2opngen_e1b_pcm_sink *sink)
{
    size_t block_samples;
    size_t block_bytes;

    if (worker == 0 || queue == 0 || control == 0 || end_frame == 0U ||
        end_frame > UINT32_MAX || expected_event_count > UINT64_MAX -
                                             expected_first_sequence) {
        if (control != 0) {
            np2opngen_e1b_control_fail(control,
                                       NP2_OPNGEN_E1B_ERROR_ARGUMENT);
        }
        return -1;
    }
    memset(worker, 0, sizeof(*worker));
    worker->queue = queue;
    worker->control = control;
    worker->end_frame = end_frame;
    worker->expected_first_sequence = expected_first_sequence;
    worker->expected_sequence = expected_first_sequence;
    worker->expected_event_count = expected_event_count;
    worker->state = NP2_OPNGEN_E1B_INIT;

    if (NP2_OPNGEN_E1B_RENDER_QUANTUM > SIZE_MAX / E1B_CHANNELS) {
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_ALLOCATION, 0);
        return -1;
    }
    block_samples = (size_t)NP2_OPNGEN_E1B_RENDER_QUANTUM * E1B_CHANNELS;
    if (block_samples > SIZE_MAX / E1B_PCM_BYTES_PER_SAMPLE) {
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_ALLOCATION, 0);
        return -1;
    }
    block_bytes = block_samples * E1B_PCM_BYTES_PER_SAMPLE;
    worker->pcm_block_bytes = block_bytes;
    worker->s32_pcm = (SINT32 *)calloc(block_samples, sizeof(*worker->s32_pcm));
    worker->canonical_pcm = (uint8_t *)calloc(block_bytes, 1U);
    worker->opngen = (OPNGEN)calloc(1U, sizeof(*worker->opngen));
    if (worker->s32_pcm == 0 || worker->canonical_pcm == 0 ||
        worker->opngen == 0) {
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_ALLOCATION, 0);
        return -1;
    }
    if (sink != 0) {
        worker->sink = *sink;
    }
    np2opngen_synth_event_trace_init(&worker->consumer_trace);

    /* This is the only OPNGEN owner in the asynchronous path. */
    opngen_initialize(E1B_RATE_HZ);
    opngen_setvol(128U);
    opngen_reset(worker->opngen);
    opngen_setcfg(worker->opngen, 3U, (UINT32)(OPN_STEREO | 0x003U));
    worker->state = NP2_OPNGEN_E1B_RUNNING;
    return 0;
}

int np2opngen_e1b_worker_init(
    struct np2opngen_e1b_worker *worker,
    struct np2opngen_spsc_queue *queue,
    struct np2opngen_e1b_control *control,
    uint64_t end_frame, uint64_t expected_first_sequence,
    uint64_t expected_event_count)
{
    return np2opngen_e1b_worker_init_with_sink(
        worker, queue, control, end_frame, expected_first_sequence,
        expected_event_count, 0);
}

void np2opngen_e1b_worker_set_observer(
    struct np2opngen_e1b_worker *worker,
    const struct np2opngen_e1b_observer *observer)
{
    if (worker != 0) {
        worker->observer = observer;
    }
}

static int worker_render_until(struct np2opngen_e1b_worker *worker,
                               uint64_t target_frame)
{
    while (worker->cursor < target_frame) {
        uint64_t frame_count = target_frame - worker->cursor;
        const uint64_t frame_offset = worker->cursor;
        size_t canonical_bytes;
        struct np2opngen_pcm_stats stats;
        if (frame_count > NP2_OPNGEN_E1B_RENDER_QUANTUM) {
            frame_count = NP2_OPNGEN_E1B_RENDER_QUANTUM;
        }
        if (worker->observer != 0 &&
            worker->observer->boundary_limiter) {
            const uint64_t in_quantum =
                worker->cursor % NP2_OPNGEN_E1B_RENDER_QUANTUM;
            const uint64_t until_boundary =
                NP2_OPNGEN_E1B_RENDER_QUANTUM - in_quantum;
            if (frame_count > until_boundary) {
                frame_count = until_boundary;
            }
        }
        canonical_bytes = (size_t)frame_count * E1B_CHANNELS *
                          E1B_PCM_BYTES_PER_SAMPLE;
        if (worker->observer != 0 &&
            worker->observer->render_begin != 0) {
            worker->observer->render_begin(worker->observer->context,
                                            frame_offset,
                                            (uint32_t)frame_count);
        }
        /* OPNGEN accumulates into its destination buffer.  Clear only the
         * bounded scratch block before each render, just as E1B's former
         * calloc-backed full buffer was initially zeroed. */
        memset(worker->s32_pcm, 0,
               (size_t)NP2_OPNGEN_E1B_RENDER_QUANTUM * E1B_CHANNELS *
                   sizeof(*worker->s32_pcm));
        if (worker->observer != 0 && worker->observer->opngen_begin != 0) {
            worker->observer->opngen_begin(worker->observer->context,
                                           frame_offset,
                                           (uint32_t)frame_count);
        }
        opngen_getpcm(worker->opngen, worker->s32_pcm, (UINT)frame_count);
        if (worker->observer != 0 && worker->observer->opngen_end != 0) {
            worker->observer->opngen_end(worker->observer->context,
                                         frame_offset,
                                         (uint32_t)frame_count, 0);
        }
        if (np2opngen_pcm_canonicalize_s16le(
                worker->s32_pcm, (size_t)frame_count, E1B_CHANNELS,
                worker->canonical_pcm, worker->pcm_block_bytes, &stats) != 0) {
            if (worker->observer != 0 && worker->observer->render_end != 0) {
                worker->observer->render_end(worker->observer->context,
                                             frame_offset,
                                             (uint32_t)frame_count, -1);
            }
            worker_fail(worker, NP2_OPNGEN_E1B_ERROR_INVARIANT,
                        NP2_SYNTH_EVENT_STATUS_CALLBACK);
            return -1;
        }
        if (worker->sink.write != 0 &&
            worker->sink.write(worker->canonical_pcm, canonical_bytes,
                               worker->cursor, worker->sink.context) != 0) {
            if (worker->observer != 0 && worker->observer->render_end != 0) {
                worker->observer->render_end(worker->observer->context,
                                             frame_offset,
                                             (uint32_t)frame_count, -1);
            }
            worker_fail(worker, NP2_OPNGEN_E1B_ERROR_OUTPUT_SINK, 0);
            return -1;
        }
        if (worker->pcm_bytes > SIZE_MAX - canonical_bytes) {
            if (worker->observer != 0 && worker->observer->render_end != 0) {
                worker->observer->render_end(worker->observer->context,
                                             frame_offset,
                                             (uint32_t)frame_count, -1);
            }
            worker_fail(worker, NP2_OPNGEN_E1B_ERROR_INVARIANT,
                        NP2_SYNTH_EVENT_STATUS_OVERFLOW);
            return -1;
        }
        worker->pcm_bytes += canonical_bytes;
        worker->cursor += frame_count;
        worker->rendered_frames += frame_count;
        if (worker->observer != 0 && worker->observer->render_end != 0) {
            worker->observer->render_end(worker->observer->context,
                                         frame_offset,
                                         (uint32_t)frame_count, 0);
        }
    }
    return 0;
}

static int worker_process_event(struct np2opngen_e1b_worker *worker,
                                const struct np2opngen_synth_event *event)
{
    int status = -1;
    const bool timestamp_zero = event->sample_timestamp == 0U;

    if (worker->observer != 0 && worker->observer->event_begin != 0) {
        worker->observer->event_begin(worker->observer->context, event,
                                      timestamp_zero);
    }

    if (event->sample_timestamp < worker->cursor) {
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_EVENT,
                    NP2_SYNTH_EVENT_STATUS_TIMESTAMP);
        goto event_error;
    }
    status = np2opngen_synth_event_validate(
        event, 1U, worker->end_frame, worker->expected_sequence,
        worker->end_frame);
    if (status != NP2_SYNTH_EVENT_STATUS_OK) {
        if (status == NP2_SYNTH_EVENT_STATUS_SEQUENCE) {
            ++worker->sequence_errors;
        }
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_EVENT, status);
        goto event_error;
    }
    if (np2opngen_synth_event_trace_update(&worker->consumer_trace, event) !=
        NP2_SYNTH_EVENT_STATUS_OK) {
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_EVENT,
                    NP2_SYNTH_EVENT_STATUS_TYPE);
        goto event_error;
    }
    if (worker->observer != 0 && worker->observer->event_end != 0) {
        worker->observer->event_end(worker->observer->context, event, 0);
    }
    if (worker_render_until(worker, event->sample_timestamp) != 0) {
        return -1;
    }
    if (worker->observer != 0 && worker->observer->event_apply_begin != 0) {
        worker->observer->event_apply_begin(worker->observer->context, event);
    }
    if (event->type == NP2_SYNTH_EVENT_REGISTER_WRITE) {
        opngen_setreg(worker->opngen, event->payload.register_write.chbase,
                      event->payload.register_write.reg,
                      event->payload.register_write.value);
    } else if (event->type == NP2_SYNTH_EVENT_KEY_EVENT) {
        opngen_keyon(worker->opngen, event->payload.key_event.channel,
                     event->payload.key_event.value);
    } else {
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_EVENT,
                    NP2_SYNTH_EVENT_STATUS_TYPE);
        if (worker->observer != 0 && worker->observer->event_apply_end != 0) {
            worker->observer->event_apply_end(worker->observer->context, event,
                                              -1);
        }
        return -1;
    }
    if (worker->observer != 0 && worker->observer->event_apply_end != 0) {
        worker->observer->event_apply_end(worker->observer->context, event, 0);
    }
    worker->last_sequence = event->sequence;
    worker->has_last_sequence = true;
    if (worker->expected_sequence == UINT64_MAX) {
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_RENDER,
                    NP2_SYNTH_EVENT_STATUS_OVERFLOW);
        return -1;
    }
    ++worker->expected_sequence;
    ++worker->dequeue_count;
    return 0;

event_error:
    if (worker->observer != 0 && worker->observer->event_end != 0) {
        worker->observer->event_end(worker->observer->context, event,
                                    status == 0 ? -1 : status);
    }
    return -1;
}

static int worker_drain(struct np2opngen_e1b_worker *worker)
{
    if (worker->expected_sequence != worker->expected_first_sequence +
                                         worker->expected_event_count ||
        worker->cursor > worker->end_frame) {
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_INVARIANT,
                    NP2_SYNTH_EVENT_STATUS_SEQUENCE);
        return -1;
    }
    worker->state = NP2_OPNGEN_E1B_DRAIN;
    if (worker_render_until(worker, worker->end_frame) != 0 ||
        worker->rendered_frames != worker->end_frame) {
        if (worker->state != NP2_OPNGEN_E1B_FAILED) {
            worker_fail(worker, NP2_OPNGEN_E1B_ERROR_INVARIANT,
                        NP2_SYNTH_EVENT_STATUS_CALLBACK);
        }
        return -1;
    }
    worker->state = NP2_OPNGEN_E1B_COMPLETE;
    return 0;
}

int np2opngen_e1b_worker_step(struct np2opngen_e1b_worker *worker)
{
    struct np2opngen_synth_event event;
    int status;
    if (worker == 0) {
        return NP2_OPNGEN_E1B_STEP_FAILED;
    }
    if (worker->state == NP2_OPNGEN_E1B_COMPLETE) {
        return NP2_OPNGEN_E1B_STEP_COMPLETE;
    }
    if (worker->state == NP2_OPNGEN_E1B_FAILED ||
        worker->state != NP2_OPNGEN_E1B_RUNNING) {
        return NP2_OPNGEN_E1B_STEP_FAILED;
    }
    if (np2opngen_e1b_control_first_error(worker->control) !=
        NP2_OPNGEN_E1B_ERROR_NONE) {
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_QUEUE, 0);
        return NP2_OPNGEN_E1B_STEP_FAILED;
    }

    if (worker->observer != 0 && worker->observer->dequeue_begin != 0) {
        worker->observer->dequeue_begin(worker->observer->context);
    }
    status = np2opngen_spsc_dequeue(worker->queue, &event);
    if (worker->observer != 0 && worker->observer->dequeue_end != 0) {
        worker->observer->dequeue_end(worker->observer->context, status,
                                      status == NP2_OPNGEN_SPSC_OK ? &event
                                                                    : 0);
    }
    if (status == NP2_OPNGEN_SPSC_OK) {
        return worker_process_event(worker, &event) == 0
                   ? NP2_OPNGEN_E1B_STEP_PROGRESS
                   : NP2_OPNGEN_E1B_STEP_FAILED;
    }
    if (status != NP2_OPNGEN_SPSC_EMPTY) {
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_QUEUE, status);
        return NP2_OPNGEN_E1B_STEP_FAILED;
    }

    ++worker->empty_wait_count;
    if (np2opngen_e1b_control_first_error(worker->control) !=
        NP2_OPNGEN_E1B_ERROR_NONE) {
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_QUEUE, 0);
        return NP2_OPNGEN_E1B_STEP_FAILED;
    }
    if (!atomic_load_explicit(&worker->control->producer_done,
                              memory_order_acquire)) {
        return NP2_OPNGEN_E1B_STEP_WAIT;
    }

    ++worker->empty_recheck_count;
    if (worker->observer != 0 && worker->observer->dequeue_begin != 0) {
        worker->observer->dequeue_begin(worker->observer->context);
    }
    status = np2opngen_spsc_dequeue(worker->queue, &event);
    if (worker->observer != 0 && worker->observer->dequeue_end != 0) {
        worker->observer->dequeue_end(worker->observer->context, status,
                                      status == NP2_OPNGEN_SPSC_OK ? &event
                                                                    : 0);
    }
    if (status == NP2_OPNGEN_SPSC_OK) {
        return worker_process_event(worker, &event) == 0
                   ? NP2_OPNGEN_E1B_STEP_PROGRESS
                   : NP2_OPNGEN_E1B_STEP_FAILED;
    }
    if (status != NP2_OPNGEN_SPSC_EMPTY) {
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_QUEUE, status);
        return NP2_OPNGEN_E1B_STEP_FAILED;
    }
    return worker_drain(worker) == 0 ? NP2_OPNGEN_E1B_STEP_COMPLETE
                                      : NP2_OPNGEN_E1B_STEP_FAILED;
}

void np2opngen_e1b_worker_destroy(struct np2opngen_e1b_worker *worker)
{
    if (worker == 0) {
        return;
    }
    free(worker->canonical_pcm);
    free(worker->s32_pcm);
    free(worker->opngen);
    worker->canonical_pcm = 0;
    worker->s32_pcm = 0;
    worker->opngen = 0;
}

int np2opngen_e1b_worker_event_trace_finish(
    struct np2opngen_e1b_worker *worker, uint64_t *count, uint32_t *crc32,
    uint8_t digest[32])
{
    return worker == 0
               ? NP2_SYNTH_EVENT_STATUS_ARGUMENT
               : np2opngen_synth_event_trace_finish(
                     &worker->consumer_trace, count, crc32, digest);
}
