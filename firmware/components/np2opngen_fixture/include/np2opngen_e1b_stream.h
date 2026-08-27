#ifndef NP2_OPNGEN_E1B_STREAM_H
#define NP2_OPNGEN_E1B_STREAM_H

#include <stdbool.h>
#ifdef __cplusplus
#include <atomic>
#define NP2_OPNGEN_E1B_ATOMIC(type) std::atomic<type>
#else
#include <stdatomic.h>
#define NP2_OPNGEN_E1B_ATOMIC(type) _Atomic type
#endif
#include <stddef.h>
#include <stdint.h>

#include <compiler.h>
#include <sound/opngen.h>

#include "np2opngen_pcm_canonical.h"
#include "np2opngen_spsc.h"

#ifdef __cplusplus
extern "C" {
#endif

enum np2opngen_e1b_state {
    NP2_OPNGEN_E1B_INIT = 0,
    NP2_OPNGEN_E1B_RUNNING,
    NP2_OPNGEN_E1B_DRAIN,
    NP2_OPNGEN_E1B_COMPLETE,
    NP2_OPNGEN_E1B_FAILED,
};

enum np2opngen_e1b_error {
    NP2_OPNGEN_E1B_ERROR_NONE = 0,
    NP2_OPNGEN_E1B_ERROR_ARGUMENT,
    NP2_OPNGEN_E1B_ERROR_ALLOCATION,
    NP2_OPNGEN_E1B_ERROR_THREAD,
    NP2_OPNGEN_E1B_ERROR_QUEUE,
    NP2_OPNGEN_E1B_ERROR_EVENT,
    NP2_OPNGEN_E1B_ERROR_RENDER,
    NP2_OPNGEN_E1B_ERROR_INVARIANT,
    NP2_OPNGEN_E1B_ERROR_OUTPUT_SINK,
    NP2_OPNGEN_E1B_ERROR_GENERATOR,
};

#define NP2_OPNGEN_E1B_RENDER_QUANTUM 240U

typedef int (*np2opngen_e1b_pcm_sink_fn)(
    const uint8_t *canonical_pcm, size_t pcm_bytes, uint64_t frame_offset,
    void *context);

struct np2opngen_e1b_pcm_sink {
    np2opngen_e1b_pcm_sink_fn write;
    void *context;
};

/* Optional, caller-owned observation seam for bounded benchmark
 * instrumentation.  The worker never allocates, logs, or timestamps on
 * behalf of an observer.  Existing callers leave this unset and retain the
 * original behaviour.  Callback state must remain valid until worker
 * destruction; callbacks run synchronously on the worker thread. */
struct np2opngen_e1b_observer {
    void (*dequeue_begin)(void *context);
    void (*dequeue_end)(void *context, int status,
                        const struct np2opngen_synth_event *event);
    void (*event_begin)(void *context,
                        const struct np2opngen_synth_event *event,
                        bool timestamp_zero);
    void (*event_end)(void *context,
                      const struct np2opngen_synth_event *event,
                      int status);
    void (*event_apply_begin)(void *context,
                              const struct np2opngen_synth_event *event);
    void (*event_apply_end)(void *context,
                            const struct np2opngen_synth_event *event,
                            int status);
    void (*render_begin)(void *context, uint64_t frame_offset,
                         uint32_t frame_count);
    void (*opngen_begin)(void *context, uint64_t frame_offset,
                         uint32_t frame_count);
    void (*opngen_end)(void *context, uint64_t frame_offset,
                       uint32_t frame_count, int status);
    void (*render_end)(void *context, uint64_t frame_offset,
                       uint32_t frame_count, int status);
    void *context;
    bool boundary_limiter;
};

enum np2opngen_e1b_step {
    NP2_OPNGEN_E1B_STEP_PROGRESS = 0,
    NP2_OPNGEN_E1B_STEP_WAIT,
    NP2_OPNGEN_E1B_STEP_COMPLETE,
    NP2_OPNGEN_E1B_STEP_FAILED,
};

struct np2opngen_e1b_control {
    NP2_OPNGEN_E1B_ATOMIC(int) first_error;
    NP2_OPNGEN_E1B_ATOMIC(bool) producer_done;
};

struct np2opngen_e1b_worker {
    struct np2opngen_spsc_queue *queue;
    struct np2opngen_e1b_control *control;
    OPNGEN opngen;
    SINT32 *s32_pcm;
    uint8_t *canonical_pcm;
    size_t pcm_block_bytes;
    size_t pcm_bytes;
    struct np2opngen_e1b_pcm_sink sink;
    const struct np2opngen_e1b_observer *observer;
    uint64_t end_frame;
    uint64_t expected_first_sequence;
    uint64_t expected_sequence;
    uint64_t expected_event_count;
    uint64_t cursor;
    uint64_t rendered_frames;
    uint64_t last_sequence;
    uint64_t dequeue_count;
    uint64_t empty_wait_count;
    uint64_t empty_recheck_count;
    uint64_t sequence_errors;
    struct np2opngen_synth_event_trace_state consumer_trace;
    bool has_last_sequence;
    int failure_status;
    enum np2opngen_e1b_state state;
};

void np2opngen_e1b_control_init(struct np2opngen_e1b_control *control);

void np2opngen_e1b_control_fail(struct np2opngen_e1b_control *control,
                                enum np2opngen_e1b_error error);

int np2opngen_e1b_control_first_error(
    const struct np2opngen_e1b_control *control);

void np2opngen_e1b_control_producer_done(
    struct np2opngen_e1b_control *control);

int np2opngen_e1b_worker_init(
    struct np2opngen_e1b_worker *worker,
    struct np2opngen_spsc_queue *queue,
    struct np2opngen_e1b_control *control,
    uint64_t end_frame, uint64_t expected_first_sequence,
    uint64_t expected_event_count);

void np2opngen_e1b_worker_set_observer(
    struct np2opngen_e1b_worker *worker,
    const struct np2opngen_e1b_observer *observer);

int np2opngen_e1b_worker_init_with_sink(
    struct np2opngen_e1b_worker *worker,
    struct np2opngen_spsc_queue *queue,
    struct np2opngen_e1b_control *control,
    uint64_t end_frame, uint64_t expected_first_sequence,
    uint64_t expected_event_count,
    const struct np2opngen_e1b_pcm_sink *sink);

int np2opngen_e1b_worker_step(struct np2opngen_e1b_worker *worker);

void np2opngen_e1b_worker_destroy(struct np2opngen_e1b_worker *worker);

int np2opngen_e1b_worker_event_trace_finish(
    struct np2opngen_e1b_worker *worker, uint64_t *count, uint32_t *crc32,
    uint8_t digest[32]);

#ifdef __cplusplus
}
#endif

#undef NP2_OPNGEN_E1B_ATOMIC

#endif /* NP2_OPNGEN_E1B_STREAM_H */
