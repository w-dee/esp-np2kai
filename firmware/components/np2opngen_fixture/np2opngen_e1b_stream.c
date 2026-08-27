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

static void worker_fail(struct np2opngen_e1b_worker *worker,
                        enum np2opngen_e1b_error error, int status)
{
    np2opngen_e1b_control_fail(worker->control, error);
    worker->failure_status = status;
    worker->state = NP2_OPNGEN_E1B_FAILED;
}

int np2opngen_e1b_worker_init(
    struct np2opngen_e1b_worker *worker,
    struct np2opngen_spsc_queue *queue,
    struct np2opngen_e1b_control *control,
    uint64_t end_frame, uint64_t expected_first_sequence,
    uint64_t expected_event_count)
{
    size_t sample_count;
    size_t pcm_bytes;

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

    if (end_frame > SIZE_MAX / E1B_CHANNELS) {
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_ALLOCATION, 0);
        return -1;
    }
    sample_count = (size_t)end_frame * E1B_CHANNELS;
    if (sample_count > SIZE_MAX / E1B_PCM_BYTES_PER_SAMPLE) {
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_ALLOCATION, 0);
        return -1;
    }
    pcm_bytes = sample_count * E1B_PCM_BYTES_PER_SAMPLE;
    worker->pcm_bytes = pcm_bytes;
    worker->s32_pcm = (SINT32 *)calloc(sample_count, sizeof(*worker->s32_pcm));
    worker->canonical_pcm = (uint8_t *)calloc(pcm_bytes, 1U);
    worker->opngen = (OPNGEN)calloc(1U, sizeof(*worker->opngen));
    if (worker->s32_pcm == 0 || worker->canonical_pcm == 0 ||
        worker->opngen == 0) {
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_ALLOCATION, 0);
        return -1;
    }

    /* This is the only OPNGEN owner in the asynchronous path. */
    opngen_initialize(E1B_RATE_HZ);
    opngen_setvol(128U);
    opngen_reset(worker->opngen);
    opngen_setcfg(worker->opngen, 3U, (UINT32)(OPN_STEREO | 0x003U));
    worker->state = NP2_OPNGEN_E1B_RUNNING;
    return 0;
}

static int worker_process_event(struct np2opngen_e1b_worker *worker,
                                const struct np2opngen_synth_event *event)
{
    int status;
    uint64_t frame_count;

    if (event->sample_timestamp < worker->cursor) {
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_EVENT,
                    NP2_SYNTH_EVENT_STATUS_TIMESTAMP);
        return -1;
    }
    status = np2opngen_synth_event_validate(
        event, 1U, worker->end_frame, worker->expected_sequence,
        worker->end_frame);
    if (status != NP2_SYNTH_EVENT_STATUS_OK) {
        if (status == NP2_SYNTH_EVENT_STATUS_SEQUENCE) {
            ++worker->sequence_errors;
        }
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_EVENT, status);
        return -1;
    }
    frame_count = event->sample_timestamp - worker->cursor;
    if (frame_count > UINT32_MAX) {
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_RENDER,
                    NP2_SYNTH_EVENT_STATUS_OVERFLOW);
        return -1;
    }
    /* Call the renderer even for zero frames so same-timestamp events remain
     * individually ordered and never get coalesced. */
    opngen_getpcm(worker->opngen,
                  worker->s32_pcm + (size_t)worker->cursor * E1B_CHANNELS,
                  (UINT)frame_count);
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
        return -1;
    }
    worker->cursor = event->sample_timestamp;
    worker->rendered_frames += frame_count;
    worker->last_sequence = event->sequence;
    worker->has_last_sequence = true;
    if (worker->expected_sequence == UINT64_MAX) {
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_EVENT,
                    NP2_SYNTH_EVENT_STATUS_OVERFLOW);
        return -1;
    }
    ++worker->expected_sequence;
    ++worker->dequeue_count;
    return 0;
}

static int worker_drain(struct np2opngen_e1b_worker *worker)
{
    struct np2opngen_pcm_stats stats;
    uint64_t tail_frames;
    if (worker->expected_sequence != worker->expected_first_sequence +
                                         worker->expected_event_count ||
        worker->cursor > worker->end_frame) {
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_INVARIANT,
                    NP2_SYNTH_EVENT_STATUS_SEQUENCE);
        return -1;
    }
    worker->state = NP2_OPNGEN_E1B_DRAIN;
    tail_frames = worker->end_frame - worker->cursor;
    if (tail_frames > UINT32_MAX) {
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_RENDER,
                    NP2_SYNTH_EVENT_STATUS_OVERFLOW);
        return -1;
    }
    opngen_getpcm(worker->opngen,
                  worker->s32_pcm + (size_t)worker->cursor * E1B_CHANNELS,
                  (UINT)tail_frames);
    worker->rendered_frames += tail_frames;
    if (worker->rendered_frames != worker->end_frame ||
        np2opngen_pcm_canonicalize_s16le(
            worker->s32_pcm, (size_t)worker->end_frame, E1B_CHANNELS,
            worker->canonical_pcm, worker->pcm_bytes, &stats) != 0) {
        worker_fail(worker, NP2_OPNGEN_E1B_ERROR_INVARIANT,
                    NP2_SYNTH_EVENT_STATUS_CALLBACK);
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

    status = np2opngen_spsc_dequeue(worker->queue, &event);
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
    status = np2opngen_spsc_dequeue(worker->queue, &event);
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
