#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "np2_crc32.h"
#include "np2_sha256.h"
#include "np2opngen_e1b_stream.h"
#include "np2opngen_fixture.h"

#define E1B_EXPECTED_PCM_BYTES 115200U
#define E1B_EXPECTED_EVENT_COUNT 64U
#define E1B_EXPECTED_END_FRAME 28800U

static const uint8_t E1B_EVENT_TRACE_SHA256[NP2_SHA256_DIGEST_SIZE] = {
    0xf2, 0x2c, 0xb0, 0x63, 0x62, 0x88, 0x94, 0x70,
    0x38, 0x5d, 0xe7, 0x29, 0xb8, 0x07, 0xda, 0x50,
    0xdc, 0xc6, 0x8d, 0x09, 0xc3, 0x77, 0x39, 0x24,
    0x2e, 0x3c, 0x33, 0x58, 0x9e, 0x27, 0x2a, 0xf5,
};
static const uint8_t E1B_PCM_SHA256[NP2_SHA256_DIGEST_SIZE] = {
    0xa1, 0x9c, 0x7d, 0x65, 0x1f, 0x9d, 0x3b, 0xf0,
    0xaa, 0x6d, 0x49, 0x3e, 0xb8, 0x0d, 0x66, 0xf0,
    0x54, 0x34, 0x64, 0xf8, 0xc9, 0x37, 0x79, 0x60,
    0x47, 0xfb, 0xc9, 0x62, 0xf5, 0x50, 0xa2, 0xe9,
};

_Static_assert(sizeof(struct np2opngen_synth_event) == 32U,
               "SPSC slots must carry the native 32-byte SynthEvent");
_Static_assert(NP2_OPNGEN_SPSC_CAPACITY == 8U,
               "E1B SPSC capacity is part of the deterministic contract");

struct reference_capture {
    uint8_t pcm[E1B_EXPECTED_PCM_BYTES];
    size_t pcm_bytes;
    uint32_t sample_rate_hz;
    uint16_t channels;
    uint16_t bits_per_sample;
};

struct full_pcm_capture {
    uint8_t pcm[E1B_EXPECTED_PCM_BYTES];
    size_t pcm_bytes;
};

enum e1b_perturbation_mode {
    E1B_MODE_BASIC = 0,
    E1B_MODE_FAST_PRODUCER,
    E1B_MODE_FAST_CONSUMER,
    E1B_MODE_ALTERNATING,
    E1B_MODE_WRAP_PRESSURE,
};

struct harness {
    struct np2opngen_spsc_queue queue;
    struct np2opngen_e1b_control control;
    struct np2opngen_e1b_worker worker;
    const struct np2opngen_synth_event *events;
    size_t event_count;
    uint64_t end_frame;
    uint64_t enqueue_count;
    uint64_t full_wait_count;
    uint32_t max_occupancy;
    enum e1b_perturbation_mode mode;
    const struct np2opngen_e1b_pcm_sink *sink;
    atomic_bool *producer_wait_observed;
    atomic_bool *worker_start_gate;
    atomic_bool *producer_start_gate;
    atomic_bool *worker_wait_observed;
};

struct worker_abort_probe {
    struct np2opngen_spsc_queue queue;
    struct np2opngen_e1b_control control;
    struct np2opngen_e1b_worker worker;
    uint64_t end_frame;
    atomic_bool started;
    atomic_bool init_failed;
    atomic_bool observed_empty_wait;
};

static struct np2opngen_synth_event test_event(uint64_t sequence)
{
    struct np2opngen_synth_event event = {
        sequence * 3U,
        sequence,
        NP2_SYNTH_EVENT_REGISTER_WRITE,
        { .register_write = {
              .chbase = 0U, .reg = 0x30U, .value = (uint8_t)sequence } },
    };
    return event;
}

static const char *mode_name(enum e1b_perturbation_mode mode)
{
    switch (mode) {
    case E1B_MODE_BASIC:
        return "BASIC";
    case E1B_MODE_FAST_PRODUCER:
        return "FAST_PRODUCER";
    case E1B_MODE_FAST_CONSUMER:
        return "FAST_CONSUMER";
    case E1B_MODE_ALTERNATING:
        return "ALTERNATING";
    case E1B_MODE_WRAP_PRESSURE:
        return "WRAP_PRESSURE";
    }
    return "INVALID";
}

static int parse_mode(int argc, char **argv, enum e1b_perturbation_mode *mode)
{
    if (mode == 0) {
        return -1;
    }
    *mode = E1B_MODE_BASIC;
    if (argc == 1) {
        return 0;
    }
    if (argc != 3 || strcmp(argv[1], "--mode") != 0) {
        return -1;
    }
    if (strcmp(argv[2], "FAST_PRODUCER") == 0) {
        *mode = E1B_MODE_FAST_PRODUCER;
    } else if (strcmp(argv[2], "FAST_CONSUMER") == 0) {
        *mode = E1B_MODE_FAST_CONSUMER;
    } else if (strcmp(argv[2], "ALTERNATING") == 0) {
        *mode = E1B_MODE_ALTERNATING;
    } else if (strcmp(argv[2], "WRAP_PRESSURE") == 0) {
        *mode = E1B_MODE_WRAP_PRESSURE;
    } else {
        return -1;
    }
    return 0;
}

static void test_spsc_queue(void)
{
    struct np2opngen_spsc_queue queue;
    struct np2opngen_synth_event event;
    struct np2opngen_synth_event snapshot[NP2_OPNGEN_SPSC_CAPACITY];
    uint32_t head_before;
    uint32_t tail_before;
    unsigned i;

    np2opngen_spsc_init(&queue);
    assert(np2opngen_spsc_dequeue(&queue, &event) == NP2_OPNGEN_SPSC_EMPTY);
    for (i = 0U; i < NP2_OPNGEN_SPSC_CAPACITY; ++i) {
        event = test_event(i);
        assert(np2opngen_spsc_enqueue(&queue, &event) == NP2_OPNGEN_SPSC_OK);
    }
    assert(np2opngen_spsc_occupancy(&queue) == 8U);
    memcpy(snapshot, queue.slots, sizeof(snapshot));
    head_before = atomic_load_explicit(&queue.head, memory_order_relaxed);
    tail_before = atomic_load_explicit(&queue.tail, memory_order_relaxed);
    event = test_event(99U);
    assert(np2opngen_spsc_enqueue(&queue, &event) == NP2_OPNGEN_SPSC_FULL);
    assert(atomic_load_explicit(&queue.head, memory_order_relaxed) ==
           head_before);
    assert(atomic_load_explicit(&queue.tail, memory_order_relaxed) ==
           tail_before);
    assert(memcmp(snapshot, queue.slots, sizeof(snapshot)) == 0);

    for (i = 0U; i < 4U; ++i) {
        assert(np2opngen_spsc_dequeue(&queue, &event) == NP2_OPNGEN_SPSC_OK);
        assert(event.sequence == i);
    }
    for (i = 8U; i < 12U; ++i) {
        event = test_event(i);
        assert(np2opngen_spsc_enqueue(&queue, &event) == NP2_OPNGEN_SPSC_OK);
    }
    for (i = 4U; i < 12U; ++i) {
        assert(np2opngen_spsc_dequeue(&queue, &event) == NP2_OPNGEN_SPSC_OK);
        assert(event.sequence == i);
    }
    assert(np2opngen_spsc_dequeue(&queue, &event) == NP2_OPNGEN_SPSC_EMPTY);

    /* Repeatedly wrap the physical slots while preserving FIFO order. */
    for (i = 0U; i < 100U; ++i) {
        unsigned j;
        for (j = 0U; j < NP2_OPNGEN_SPSC_CAPACITY; ++j) {
            event = test_event((uint64_t)i * 8U + j);
            assert(np2opngen_spsc_enqueue(&queue, &event) ==
                   NP2_OPNGEN_SPSC_OK);
        }
        for (j = 0U; j < NP2_OPNGEN_SPSC_CAPACITY; ++j) {
            assert(np2opngen_spsc_dequeue(&queue, &event) ==
                   NP2_OPNGEN_SPSC_OK);
            assert(event.sequence == (uint64_t)i * 8U + j);
        }
    }
}

static int capture_reference(const uint8_t *pcm, size_t pcm_bytes,
                             uint32_t sample_rate_hz, uint16_t channels,
                             uint16_t bits_per_sample, void *context)
{
    struct reference_capture *capture = (struct reference_capture *)context;
    if (capture == 0 || pcm == 0 || pcm_bytes != E1B_EXPECTED_PCM_BYTES ||
        sample_rate_hz != 48000U || channels != 2U || bits_per_sample != 16U) {
        return -1;
    }
    memcpy(capture->pcm, pcm, pcm_bytes);
    capture->pcm_bytes = pcm_bytes;
    capture->sample_rate_hz = sample_rate_hz;
    capture->channels = channels;
    capture->bits_per_sample = bits_per_sample;
    return 0;
}

static int capture_e1b_block(const uint8_t *pcm, size_t pcm_bytes,
                             uint64_t frame_offset, void *context)
{
    struct full_pcm_capture *capture = (struct full_pcm_capture *)context;
    size_t expected_offset;
    if (capture == 0 || pcm == 0 || pcm_bytes > sizeof(capture->pcm) ||
        capture->pcm_bytes > sizeof(capture->pcm) - pcm_bytes ||
        frame_offset > SIZE_MAX / 4U) {
        return -1;
    }
    expected_offset = (size_t)frame_offset * 4U;
    if (expected_offset != capture->pcm_bytes) {
        return -1;
    }
    memcpy(capture->pcm + capture->pcm_bytes, pcm, pcm_bytes);
    capture->pcm_bytes += pcm_bytes;
    return 0;
}

static void *worker_thread_main(void *context)
{
    struct harness *harness = (struct harness *)context;
    uint64_t schedule_counter = 0U;
    int step;
    if (np2opngen_e1b_worker_init_with_sink(
            &harness->worker, &harness->queue, &harness->control,
            harness->end_frame, 0U, harness->event_count, harness->sink) != 0) {
        return 0;
    }
    if (harness->worker_start_gate != 0) {
        while (!atomic_load_explicit(harness->worker_start_gate,
                                     memory_order_acquire)) {
            if (np2opngen_e1b_control_first_error(&harness->control) !=
                NP2_OPNGEN_E1B_ERROR_NONE) {
                return 0;
            }
            sched_yield();
        }
    }
    do {
        step = np2opngen_e1b_worker_step(&harness->worker);
        if (step == NP2_OPNGEN_E1B_STEP_WAIT) {
            if (harness->worker_wait_observed != 0) {
                atomic_store_explicit(harness->worker_wait_observed, true,
                                      memory_order_release);
            }
            sched_yield();
        } else {
            ++schedule_counter;
            if ((harness->mode == E1B_MODE_FAST_PRODUCER &&
                 schedule_counter % 4U == 0U) ||
                (harness->mode == E1B_MODE_ALTERNATING &&
                 schedule_counter % 5U == 0U)) {
                sched_yield();
            }
        }
    } while (step != NP2_OPNGEN_E1B_STEP_COMPLETE &&
             step != NP2_OPNGEN_E1B_STEP_FAILED);
    return 0;
}

static void *producer_thread_main(void *context)
{
    struct harness *harness = (struct harness *)context;
    uint64_t schedule_counter = 0U;
    size_t i;

    if (harness->producer_start_gate != 0) {
        while (!atomic_load_explicit(harness->producer_start_gate,
                                     memory_order_acquire)) {
            if (np2opngen_e1b_control_first_error(&harness->control) !=
                NP2_OPNGEN_E1B_ERROR_NONE) {
                return 0;
            }
            sched_yield();
        }
    }
    for (i = 0U; i < harness->event_count; ++i) {
        int status;
        for (;;) {
            if (np2opngen_e1b_control_first_error(&harness->control) !=
                NP2_OPNGEN_E1B_ERROR_NONE) {
                return 0;
            }
            status = np2opngen_spsc_enqueue(&harness->queue,
                                             &harness->events[i]);
            if (status == NP2_OPNGEN_SPSC_OK) {
                uint32_t occupancy;
                ++harness->enqueue_count;
                occupancy = np2opngen_spsc_occupancy(&harness->queue);
                if (occupancy > harness->max_occupancy) {
                    harness->max_occupancy = occupancy;
                }
                ++schedule_counter;
                if ((harness->mode == E1B_MODE_FAST_CONSUMER &&
                     schedule_counter % 2U == 0U) ||
                    (harness->mode == E1B_MODE_ALTERNATING &&
                     schedule_counter % 3U == 0U)) {
                    sched_yield();
                }
                break;
            }
            if (status != NP2_OPNGEN_SPSC_FULL) {
                np2opngen_e1b_control_fail(&harness->control,
                                           NP2_OPNGEN_E1B_ERROR_QUEUE);
                return 0;
            }
            ++harness->full_wait_count;
            if (harness->producer_wait_observed != 0) {
                atomic_store_explicit(harness->producer_wait_observed, true,
                                      memory_order_release);
            }
            sched_yield();
        }
    }
    if (np2opngen_e1b_control_first_error(&harness->control) ==
        NP2_OPNGEN_E1B_ERROR_NONE) {
        atomic_store_explicit(&harness->control.producer_done, true,
                              memory_order_release);
    }
    return 0;
}

static void *worker_abort_thread_main(void *context)
{
    struct worker_abort_probe *probe = (struct worker_abort_probe *)context;
    int step;
    if (np2opngen_e1b_worker_init(&probe->worker, &probe->queue,
                                  &probe->control, probe->end_frame, 0U,
                                  E1B_EXPECTED_EVENT_COUNT) != 0) {
        atomic_store_explicit(&probe->init_failed, true,
                              memory_order_release);
        atomic_store_explicit(&probe->started, true, memory_order_release);
        return 0;
    }
    atomic_store_explicit(&probe->started, true, memory_order_release);
    do {
        step = np2opngen_e1b_worker_step(&probe->worker);
        if (step == NP2_OPNGEN_E1B_STEP_WAIT) {
            atomic_store_explicit(&probe->observed_empty_wait, true,
                                  memory_order_release);
            sched_yield();
        }
    } while (step != NP2_OPNGEN_E1B_STEP_COMPLETE &&
             step != NP2_OPNGEN_E1B_STEP_FAILED);
    return 0;
}

static void test_failure_termination(
    const struct np2opngen_synth_event *events, size_t event_count,
    uint64_t end_frame)
{
    struct harness producer_probe = {0};
    struct harness worker_failure_probe = {0};
    struct worker_abort_probe worker_probe = {0};
    pthread_t producer_thread;
    pthread_t worker_thread;
    pthread_t worker_failure_thread;
    atomic_bool producer_wait_observed;
    atomic_bool worker_start_gate;
    struct np2opngen_synth_event invalid_event;
    unsigned i;

    /* A full queue leaves the producer in its full/retry loop until the
     * shared first-error publication unblocks it. */
    producer_probe.events = events;
    producer_probe.event_count = event_count;
    np2opngen_spsc_init(&producer_probe.queue);
    np2opngen_e1b_control_init(&producer_probe.control);
    for (i = 0U; i < NP2_OPNGEN_SPSC_CAPACITY; ++i) {
        assert(np2opngen_spsc_enqueue(&producer_probe.queue, &events[i]) ==
               NP2_OPNGEN_SPSC_OK);
    }
    assert(pthread_create(&producer_thread, 0, producer_thread_main,
                          &producer_probe) == 0);
    np2opngen_e1b_control_fail(&producer_probe.control,
                               NP2_OPNGEN_E1B_ERROR_QUEUE);
    assert(pthread_join(producer_thread, 0) == 0);
    assert(producer_probe.enqueue_count == 0U);

    /* Keep the producer in a full-queue wait, then let a worker consume an
     * invalid event.  The worker's first-error publication must terminate the
     * producer retry loop without dropping it into an infinite wait. */
    worker_failure_probe.events = events;
    worker_failure_probe.event_count = event_count;
    worker_failure_probe.end_frame = end_frame;
    np2opngen_spsc_init(&worker_failure_probe.queue);
    np2opngen_e1b_control_init(&worker_failure_probe.control);
    invalid_event = test_event(99U);
    assert(np2opngen_spsc_enqueue(&worker_failure_probe.queue,
                                  &invalid_event) == NP2_OPNGEN_SPSC_OK);
    for (i = 1U; i < NP2_OPNGEN_SPSC_CAPACITY; ++i) {
        invalid_event = test_event(100U + i);
        assert(np2opngen_spsc_enqueue(&worker_failure_probe.queue,
                                      &invalid_event) ==
               NP2_OPNGEN_SPSC_OK);
    }
    atomic_init(&producer_wait_observed, false);
    atomic_init(&worker_start_gate, false);
    worker_failure_probe.producer_wait_observed = &producer_wait_observed;
    worker_failure_probe.worker_start_gate = &worker_start_gate;
    assert(pthread_create(&worker_failure_thread, 0, worker_thread_main,
                          &worker_failure_probe) == 0);
    assert(pthread_create(&producer_thread, 0, producer_thread_main,
                          &worker_failure_probe) == 0);
    while (!atomic_load_explicit(&producer_wait_observed,
                                 memory_order_acquire)) {
        sched_yield();
    }
    atomic_store_explicit(&worker_start_gate, true, memory_order_release);
    assert(pthread_join(producer_thread, 0) == 0);
    assert(pthread_join(worker_failure_thread, 0) == 0);
    assert(worker_failure_probe.worker.state == NP2_OPNGEN_E1B_FAILED);
    assert(np2opngen_e1b_control_first_error(&worker_failure_probe.control) ==
           NP2_OPNGEN_E1B_ERROR_EVENT);
    assert(worker_failure_probe.full_wait_count != 0U);
    np2opngen_e1b_worker_destroy(&worker_failure_probe.worker);

    /* An empty worker first reaches its WAIT state; a main-side abort then
     * causes the next poll to terminate the worker without a producer. */
    np2opngen_spsc_init(&worker_probe.queue);
    np2opngen_e1b_control_init(&worker_probe.control);
    worker_probe.end_frame = end_frame;
    atomic_init(&worker_probe.started, false);
    atomic_init(&worker_probe.init_failed, false);
    atomic_init(&worker_probe.observed_empty_wait, false);
    assert(pthread_create(&worker_thread, 0, worker_abort_thread_main,
                          &worker_probe) == 0);
    while (!atomic_load_explicit(&worker_probe.started,
                                 memory_order_acquire)) {
        sched_yield();
    }
    while (!atomic_load_explicit(&worker_probe.observed_empty_wait,
                                 memory_order_acquire) &&
           !atomic_load_explicit(&worker_probe.init_failed,
                                 memory_order_acquire)) {
        sched_yield();
    }
    assert(!atomic_load_explicit(&worker_probe.init_failed,
                                 memory_order_acquire));
    np2opngen_e1b_control_fail(&worker_probe.control,
                               NP2_OPNGEN_E1B_ERROR_THREAD);
    assert(pthread_join(worker_thread, 0) == 0);
    assert(worker_probe.worker.state == NP2_OPNGEN_E1B_FAILED);
    np2opngen_e1b_worker_destroy(&worker_probe.worker);
}

static void print_sha256(const uint8_t digest[NP2_SHA256_DIGEST_SIZE])
{
    size_t i;
    for (i = 0U; i < NP2_SHA256_DIGEST_SIZE; ++i) {
        printf("%02x", digest[i]);
    }
}

int main(int argc, char **argv)
{
    static struct reference_capture reference;
    static struct full_pcm_capture async_capture;
    const struct np2opngen_e1b_pcm_sink async_sink = {
        capture_e1b_block, &async_capture
    };
    struct harness harness = {0};
    const struct np2opngen_synth_event *events = 0;
    size_t event_count = 0U;
    uint64_t end_frame = 0U;
    uint32_t event_trace_crc32 = 0U;
    uint8_t event_trace_sha256[NP2_SHA256_DIGEST_SIZE];
    uint32_t async_pcm_crc32;
    uint8_t async_pcm_sha256[NP2_SHA256_DIGEST_SIZE];
    np2_sha256_context sha;
    pthread_t worker_thread;
    pthread_t producer_thread;
    atomic_bool producer_start_gate;
    atomic_bool worker_wait_observed;
    atomic_bool worker_start_gate;
    atomic_bool producer_full_wait_observed;
    enum e1b_perturbation_mode mode;
    uint32_t producer_head;
    uint32_t consumer_tail;
    size_t same_timestamp_adjacent = 0U;
    size_t i;
    int status;
    int worker_created;
    int producer_created;
    int first_error;
    bool reference_match;
    bool event_trace_match;
    bool ordering;
    bool physical_wrap;
    bool mode_pressure;

    if (parse_mode(argc, argv, &mode) != 0) {
        fprintf(stderr,
                "usage: %s [--mode FAST_PRODUCER|FAST_CONSUMER|ALTERNATING|WRAP_PRESSURE]\n",
                argv[0]);
        return 2;
    }

    test_spsc_queue();
    if (np2opngen_fixture_get_e1_events(&events, &event_count, &end_frame) !=
            0 || events == 0 || event_count != E1B_EXPECTED_EVENT_COUNT ||
        end_frame != E1B_EXPECTED_END_FRAME ||
        np2opngen_synth_event_validate(events, event_count, end_frame, 0U,
                                       end_frame) != NP2_SYNTH_EVENT_STATUS_OK ||
        np2opngen_synth_event_trace(events, event_count, &event_trace_crc32,
                                    event_trace_sha256) !=
            NP2_SYNTH_EVENT_STATUS_OK) {
        fprintf(stderr, "E1B_SPSC_RESULT=FAIL reason=preflight\n");
        return 1;
    }
    status = np2opngen_fixture_run_with_sink(0, 0, capture_reference,
                                             &reference);
    if (status != 0) {
        fprintf(stderr, "E1B_SPSC_RESULT=FAIL reason=reference\n");
        return 1;
    }

    test_failure_termination(events, event_count, end_frame);

    for (i = 1U; i < event_count; ++i) {
        if (events[i - 1U].sample_timestamp == events[i].sample_timestamp) {
            ++same_timestamp_adjacent;
        }
    }

    harness.events = events;
    harness.event_count = event_count;
    harness.end_frame = end_frame;
    harness.sink = &async_sink;
    harness.mode = mode;
    atomic_init(&producer_start_gate, mode != E1B_MODE_FAST_CONSUMER);
    atomic_init(&worker_wait_observed, false);
    atomic_init(&worker_start_gate, mode != E1B_MODE_WRAP_PRESSURE);
    atomic_init(&producer_full_wait_observed, false);
    if (mode == E1B_MODE_FAST_CONSUMER) {
        harness.producer_start_gate = &producer_start_gate;
        harness.worker_wait_observed = &worker_wait_observed;
    }
    if (mode == E1B_MODE_WRAP_PRESSURE) {
        harness.worker_start_gate = &worker_start_gate;
        harness.producer_wait_observed = &producer_full_wait_observed;
    }
    np2opngen_spsc_init(&harness.queue);
    np2opngen_e1b_control_init(&harness.control);

    worker_created = pthread_create(&worker_thread, 0, worker_thread_main,
                                    &harness) == 0;
    if (!worker_created) {
        np2opngen_e1b_control_fail(&harness.control,
                                   NP2_OPNGEN_E1B_ERROR_THREAD);
        fprintf(stderr, "E1B_SPSC_RESULT=FAIL reason=worker_create\n");
        return 1;
    }
    producer_created = pthread_create(&producer_thread, 0,
                                      producer_thread_main, &harness) == 0;
    if (!producer_created) {
        np2opngen_e1b_control_fail(&harness.control,
                                   NP2_OPNGEN_E1B_ERROR_THREAD);
        (void)pthread_join(worker_thread, 0);
        fprintf(stderr, "E1B_SPSC_RESULT=FAIL reason=producer_create\n");
        np2opngen_e1b_worker_destroy(&harness.worker);
        return 1;
    }
    if (mode == E1B_MODE_FAST_CONSUMER) {
        while (!atomic_load_explicit(&worker_wait_observed,
                                     memory_order_acquire) &&
               np2opngen_e1b_control_first_error(&harness.control) ==
                   NP2_OPNGEN_E1B_ERROR_NONE) {
            sched_yield();
        }
        atomic_store_explicit(&producer_start_gate, true,
                              memory_order_release);
    }
    if (mode == E1B_MODE_WRAP_PRESSURE) {
        while (!atomic_load_explicit(&producer_full_wait_observed,
                                     memory_order_acquire) &&
               np2opngen_e1b_control_first_error(&harness.control) ==
                   NP2_OPNGEN_E1B_ERROR_NONE) {
            sched_yield();
        }
        atomic_store_explicit(&worker_start_gate, true, memory_order_release);
    }
    (void)pthread_join(producer_thread, 0);
    (void)pthread_join(worker_thread, 0);

    producer_head = atomic_load_explicit(&harness.queue.head,
                                         memory_order_relaxed);
    consumer_tail = atomic_load_explicit(&harness.queue.tail,
                                         memory_order_relaxed);

    reference_match = harness.worker.state == NP2_OPNGEN_E1B_COMPLETE &&
                      async_capture.pcm_bytes == reference.pcm_bytes &&
                      memcmp(async_capture.pcm, reference.pcm,
                             reference.pcm_bytes) == 0;
    if (harness.worker.state == NP2_OPNGEN_E1B_COMPLETE &&
        async_capture.pcm_bytes != 0U) {
        async_pcm_crc32 = np2_crc32_iso_hdlc(
            async_capture.pcm, async_capture.pcm_bytes);
        np2_sha256_init(&sha);
        np2_sha256_update(&sha, async_capture.pcm,
                          async_capture.pcm_bytes);
        np2_sha256_final(&sha, async_pcm_sha256);
    } else {
        async_pcm_crc32 = 0U;
        memset(async_pcm_sha256, 0, sizeof(async_pcm_sha256));
    }
    event_trace_match = event_trace_crc32 == UINT32_C(0x807a514e) &&
                        memcmp(event_trace_sha256, E1B_EVENT_TRACE_SHA256,
                               NP2_SHA256_DIGEST_SIZE) == 0;
    first_error = np2opngen_e1b_control_first_error(&harness.control);
    ordering = harness.enqueue_count == E1B_EXPECTED_EVENT_COUNT &&
               harness.worker.dequeue_count == E1B_EXPECTED_EVENT_COUNT &&
               harness.worker.sequence_errors == 0U &&
               harness.worker.has_last_sequence &&
               harness.worker.last_sequence == E1B_EXPECTED_EVENT_COUNT - 1U &&
               harness.worker.expected_sequence == E1B_EXPECTED_EVENT_COUNT &&
               harness.worker.rendered_frames == E1B_EXPECTED_END_FRAME &&
               first_error == NP2_OPNGEN_E1B_ERROR_NONE;
    physical_wrap = producer_head == E1B_EXPECTED_EVENT_COUNT &&
                    consumer_tail == E1B_EXPECTED_EVENT_COUNT &&
                    producer_head / NP2_OPNGEN_SPSC_CAPACITY != 0U &&
                    consumer_tail / NP2_OPNGEN_SPSC_CAPACITY != 0U;
    mode_pressure = (mode != E1B_MODE_FAST_CONSUMER ||
                     harness.worker.empty_wait_count != 0U) &&
                    (mode != E1B_MODE_WRAP_PRESSURE ||
                     harness.full_wait_count != 0U);

    printf("E1B_SPSC_META version=1 mode=%s capacity=%u events=%zu end_frame=%" PRIu64
           "\n",
           mode_name(mode), NP2_OPNGEN_SPSC_CAPACITY, event_count, end_frame);
    printf("E1B_SPSC_QUEUE enqueue=%" PRIu64 " dequeue=%" PRIu64
           " sequence_errors=%" PRIu64 " rendered_frames=%" PRIu64
           " max_occupancy=%u full_waits=%" PRIu64 " empty_waits=%" PRIu64
           " empty_rechecks=%" PRIu64 " final_sequence=%" PRIu64
           " first_error=%d head_counter=%" PRIu32 " tail_counter=%" PRIu32
           " producer_slot_wraps=%" PRIu32 " consumer_slot_wraps=%" PRIu32
           "\n",
           harness.enqueue_count, harness.worker.dequeue_count,
           harness.worker.sequence_errors, harness.worker.rendered_frames,
           harness.max_occupancy, harness.full_wait_count,
           harness.worker.empty_wait_count,
           harness.worker.empty_recheck_count,
           harness.worker.has_last_sequence ? harness.worker.last_sequence
                                             : UINT64_MAX,
           first_error, producer_head, consumer_tail,
           producer_head / NP2_OPNGEN_SPSC_CAPACITY,
           consumer_tail / NP2_OPNGEN_SPSC_CAPACITY);
    printf("E1B_SPSC_EVENTS count=%zu record_bytes=24 crc32=0x%08" PRIx32
           " sha256=",
           event_count, event_trace_crc32);
    print_sha256(event_trace_sha256);
    printf(" same_timestamp_adjacent=%zu\n", same_timestamp_adjacent);
    printf("E1B_SPSC_PCM bytes=%zu crc32=0x%08" PRIx32 " sha256=",
           async_capture.pcm_bytes, async_pcm_crc32);
    print_sha256(async_pcm_sha256);
    printf("\n");
    printf("E1B_SPSC_INVARIANTS reference_match=%s event_trace=%s ordering=%s"
           " wrap=%s mode_pressure=%s\n",
           reference_match ? "PASS" : "FAIL",
           event_trace_match ? "PASS" : "FAIL", ordering ? "PASS" : "FAIL",
           physical_wrap ? "PASS" : "FAIL",
           mode_pressure ? "PASS" : "FAIL");
    status = reference_match && event_trace_match && ordering && physical_wrap &&
             mode_pressure &&
             async_pcm_crc32 == UINT32_C(0x17496602) &&
             memcmp(async_pcm_sha256, E1B_PCM_SHA256,
                    NP2_SHA256_DIGEST_SIZE) == 0;
    printf("E1B_SPSC_RESULT=%s\n", status ? "PASS" : "FAIL");
    np2opngen_e1b_worker_destroy(&harness.worker);
    return status ? 0 : 1;
}
