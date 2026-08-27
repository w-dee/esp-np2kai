#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "np2_crc32.h"
#include "np2_sha256.h"
#include "np2opngen_e1b_stream.h"
#include "np2opngen_spsc.h"
#include "np2opngen_synthetic_workload.h"

#define CHANNELS 2U
#define PCM_BYTES_PER_FRAME (CHANNELS * 2U)
#define MAX_TEST_EVENTS 41127U

struct rolling_pcm {
    uint64_t frames;
    uint64_t bytes;
    uint32_t crc32;
    np2_sha256_context sha256;
    uint64_t expected_offset;
    bool failed;
};

struct timing_stats {
    uint64_t *samples;
    size_t count;
    size_t capacity;
    uint64_t over_5000;
    uint64_t max_consecutive_over_5000;
};

struct sustained_run {
    struct np2opngen_spsc_queue queue;
    struct np2opngen_e1b_control control;
    struct np2opngen_e1b_worker worker;
    struct np2opngen_synthetic_workload workload;
    struct np2opngen_e1b_pcm_sink sink;
    struct rolling_pcm pcm;
    struct np2opngen_synth_event_trace_state producer_trace;
    uint64_t producer_count;
    uint64_t full_waits;
    uint32_t max_occupancy;
    uint64_t expected_events;
    uint64_t end_frame;
    struct timing_stats timing;
};

static uint64_t monotonic_us(void)
{
    struct timespec value;
    (void)clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * UINT64_C(1000000) +
           (uint64_t)value.tv_nsec / UINT64_C(1000);
}

static int rolling_pcm_sink(const uint8_t *pcm, size_t bytes,
                            uint64_t frame_offset, void *context)
{
    struct rolling_pcm *rolling = (struct rolling_pcm *)context;
    if (rolling == 0 || pcm == 0 || bytes % PCM_BYTES_PER_FRAME != 0U ||
        frame_offset != rolling->expected_offset ||
        rolling->frames > UINT64_MAX - bytes / PCM_BYTES_PER_FRAME ||
        rolling->bytes > UINT64_MAX - bytes) {
        if (rolling != 0) {
            rolling->failed = true;
        }
        return -1;
    }
    rolling->crc32 = np2_crc32_iso_hdlc_update(rolling->crc32, pcm, bytes);
    np2_sha256_update(&rolling->sha256, pcm, bytes);
    rolling->frames += bytes / PCM_BYTES_PER_FRAME;
    rolling->bytes += bytes;
    rolling->expected_offset += bytes / PCM_BYTES_PER_FRAME;
    return 0;
}

static int failing_pcm_sink(const uint8_t *pcm, size_t bytes,
                            uint64_t frame_offset, void *context)
{
    (void)pcm;
    (void)bytes;
    (void)frame_offset;
    (void)context;
    return -1;
}

static void timing_record(struct timing_stats *timing, uint64_t elapsed)
{
    if (timing->count < timing->capacity) {
        timing->samples[timing->count++] = elapsed;
    }
}

static void *sustained_worker_thread(void *context)
{
    struct sustained_run *run = (struct sustained_run *)context;
    int step;
    if (np2opngen_e1b_worker_init_with_sink(
            &run->worker, &run->queue, &run->control, run->end_frame, 0U,
            run->expected_events, &run->sink) != 0) {
        return 0;
    }
    do {
        const uint64_t start = monotonic_us();
        step = np2opngen_e1b_worker_step(&run->worker);
        const uint64_t elapsed = monotonic_us() - start;
        if (step == NP2_OPNGEN_E1B_STEP_PROGRESS &&
            run->worker.rendered_frames > NP2_OPNGEN_SYNTHETIC_WARMUP_FRAMES) {
            timing_record(&run->timing, elapsed);
        }
        if (step == NP2_OPNGEN_E1B_STEP_WAIT) {
            sched_yield();
        }
    } while (step != NP2_OPNGEN_E1B_STEP_COMPLETE &&
             step != NP2_OPNGEN_E1B_STEP_FAILED);
    return 0;
}

static void *sustained_producer_thread(void *context)
{
    struct sustained_run *run = (struct sustained_run *)context;
    for (;;) {
        struct np2opngen_synth_event event;
        int status = np2opngen_synthetic_workload_peek(&run->workload, &event);
        if (status == 0) {
            break;
        }
        if (status < 0) {
            np2opngen_e1b_control_fail(&run->control,
                                       NP2_OPNGEN_E1B_ERROR_GENERATOR);
            return 0;
        }
        for (;;) {
            if (np2opngen_e1b_control_first_error(&run->control) !=
                NP2_OPNGEN_E1B_ERROR_NONE) {
                return 0;
            }
            status = np2opngen_spsc_enqueue(&run->queue, &event);
            if (status == NP2_OPNGEN_SPSC_OK) {
                const uint32_t occupancy =
                    np2opngen_spsc_occupancy(&run->queue);
                if (np2opngen_synth_event_trace_update(&run->producer_trace,
                                                       &event) !=
                    NP2_SYNTH_EVENT_STATUS_OK ||
                    np2opngen_synthetic_workload_commit(&run->workload) != 0) {
                    np2opngen_e1b_control_fail(
                        &run->control, NP2_OPNGEN_E1B_ERROR_GENERATOR);
                    return 0;
                }
                ++run->producer_count;
                if (occupancy > run->max_occupancy) {
                    run->max_occupancy = occupancy;
                }
                break;
            }
            if (status != NP2_OPNGEN_SPSC_FULL) {
                np2opngen_e1b_control_fail(&run->control,
                                           NP2_OPNGEN_E1B_ERROR_QUEUE);
                return 0;
            }
            ++run->full_waits;
            sched_yield();
        }
    }
    if (np2opngen_e1b_control_first_error(&run->control) ==
        NP2_OPNGEN_E1B_ERROR_NONE) {
        atomic_store_explicit(&run->control.producer_done, true,
                              memory_order_release);
    }
    return 0;
}

static int parse_profile(const char *name,
                         enum np2opngen_synthetic_profile *profile)
{
    if (strcmp(name, "SYNTHETIC-LIGHT") == 0) {
        *profile = NP2_OPNGEN_SYNTHETIC_LIGHT;
    } else if (strcmp(name, "SYNTHETIC-HEAVY") == 0) {
        *profile = NP2_OPNGEN_SYNTHETIC_HEAVY;
    } else if (strcmp(name, "STRESS") == 0) {
        *profile = NP2_OPNGEN_SYNTHETIC_STRESS;
    } else {
        return -1;
    }
    return 0;
}

static void print_sha256(const uint8_t digest[NP2_SHA256_DIGEST_SIZE])
{
    size_t i;
    for (i = 0U; i < NP2_SHA256_DIGEST_SIZE; ++i) {
        printf("%02x", digest[i]);
    }
}

static int compare_u64(const void *left, const void *right)
{
    const uint64_t a = *(const uint64_t *)left;
    const uint64_t b = *(const uint64_t *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static uint64_t percentile(const uint64_t *samples, size_t count,
                           unsigned percent)
{
    size_t rank = (count * percent + 99U) / 100U;
    if (rank == 0U) {
        rank = 1U;
    }
    return samples[rank - 1U];
}

static void finish_timing(struct timing_stats *timing)
{
    size_t i;
    uint64_t current = 0U;
    if (timing->count == 0U) {
        return;
    }
    for (i = 0U; i < timing->count; ++i) {
        if (timing->samples[i] > 5000U) {
            ++timing->over_5000;
            ++current;
            if (current > timing->max_consecutive_over_5000) {
                timing->max_consecutive_over_5000 = current;
            }
        } else {
            current = 0U;
        }
    }
    qsort(timing->samples, timing->count, sizeof(*timing->samples),
          compare_u64);
}

static void make_test_event(struct np2opngen_synth_event *event,
                            uint64_t timestamp, uint64_t sequence)
{
    *event = (struct np2opngen_synth_event){
        timestamp,
        sequence,
        NP2_SYNTH_EVENT_REGISTER_WRITE,
        { .register_write = { 0U, (uint16_t)(0x30U + (sequence % 4U) * 4U),
                              (uint8_t)(sequence + 1U) } },
    };
}

static void test_counter_wrap(void)
{
    struct np2opngen_spsc_queue queue;
    struct np2opngen_synth_event event;
    const uint32_t near_wrap = UINT32_MAX - 3U;
    unsigned i;
    np2opngen_spsc_init(&queue);
    atomic_store_explicit(&queue.head, near_wrap, memory_order_relaxed);
    atomic_store_explicit(&queue.tail, near_wrap, memory_order_relaxed);
    for (i = 0U; i < NP2_OPNGEN_SPSC_CAPACITY; ++i) {
        make_test_event(&event, 0U, i);
        assert(np2opngen_spsc_enqueue(&queue, &event) == NP2_OPNGEN_SPSC_OK);
    }
    assert(np2opngen_spsc_occupancy(&queue) == 8U);
    make_test_event(&event, 0U, 99U);
    assert(np2opngen_spsc_enqueue(&queue, &event) == NP2_OPNGEN_SPSC_FULL);
    for (i = 0U; i < 4U; ++i) {
        assert(np2opngen_spsc_dequeue(&queue, &event) == NP2_OPNGEN_SPSC_OK);
        assert(event.sequence == i);
    }
    for (i = 8U; i < 12U; ++i) {
        make_test_event(&event, 0U, i);
        assert(np2opngen_spsc_enqueue(&queue, &event) == NP2_OPNGEN_SPSC_OK);
    }
    for (i = 4U; i < 12U; ++i) {
        assert(np2opngen_spsc_dequeue(&queue, &event) == NP2_OPNGEN_SPSC_OK);
        assert(event.sequence == i);
    }
    assert(np2opngen_spsc_dequeue(&queue, &event) == NP2_OPNGEN_SPSC_EMPTY);
    printf("E1C_PATH_COUNTER_WRAP=PASS\n");
}

static void test_done_boundaries(void)
{
    unsigned occupancy;
    for (occupancy = 0U; occupancy <= NP2_OPNGEN_SPSC_CAPACITY;
         occupancy = occupancy == 0U ? 1U : occupancy == 1U ? 7U : 8U) {
        struct np2opngen_spsc_queue queue;
        struct np2opngen_e1b_control control;
        struct np2opngen_e1b_worker worker = {0};
        struct np2opngen_synth_event event;
        unsigned i;
        np2opngen_spsc_init(&queue);
        np2opngen_e1b_control_init(&control);
        for (i = 0U; i < occupancy; ++i) {
            make_test_event(&event, 0U, i);
            assert(np2opngen_spsc_enqueue(&queue, &event) ==
                   NP2_OPNGEN_SPSC_OK);
        }
        atomic_store_explicit(&control.producer_done, true,
                              memory_order_release);
        assert(np2opngen_e1b_worker_init(
                   &worker, &queue, &control, 240U, 0U, occupancy) == 0);
        for (;;) {
            const int step = np2opngen_e1b_worker_step(&worker);
            assert(step != NP2_OPNGEN_E1B_STEP_FAILED);
            if (step == NP2_OPNGEN_E1B_STEP_COMPLETE) {
                break;
            }
        }
        assert(worker.dequeue_count == occupancy);
        assert(worker.rendered_frames == 240U);
        np2opngen_e1b_worker_destroy(&worker);
        if (occupancy == 8U) {
            break;
        }
    }
    printf("E1C_PATH_PRODUCER_DONE_BOUNDARIES=PASS\n");
}

static void test_same_timestamp_burst(void)
{
    struct np2opngen_spsc_queue queue;
    struct np2opngen_e1b_control control;
    struct np2opngen_e1b_worker worker = {0};
    struct rolling_pcm pcm = {0};
    struct np2opngen_e1b_pcm_sink sink = { rolling_pcm_sink, &pcm };
    struct np2opngen_synth_event_trace_state producer_trace;
    struct np2opngen_synth_event event;
    uint8_t producer_sha[NP2_SHA256_DIGEST_SIZE];
    uint8_t consumer_sha[NP2_SHA256_DIGEST_SIZE];
    uint64_t producer_count;
    uint64_t consumer_count;
    uint32_t producer_crc;
    uint32_t consumer_crc;
    unsigned i;
    np2opngen_spsc_init(&queue);
    np2opngen_e1b_control_init(&control);
    np2opngen_synth_event_trace_init(&producer_trace);
    for (i = 0U; i < 8U; ++i) {
        make_test_event(&event, 0U, i);
        assert(np2opngen_spsc_enqueue(&queue, &event) == NP2_OPNGEN_SPSC_OK);
        assert(np2opngen_synth_event_trace_update(&producer_trace, &event) ==
               NP2_SYNTH_EVENT_STATUS_OK);
    }
    atomic_store_explicit(&control.producer_done, true, memory_order_release);
    assert(np2opngen_e1b_worker_init_with_sink(
               &worker, &queue, &control, 240U, 0U, 8U, &sink) == 0);
    for (i = 0U; i < 8U; ++i) {
        assert(np2opngen_e1b_worker_step(&worker) ==
               NP2_OPNGEN_E1B_STEP_PROGRESS);
        assert(worker.cursor == 0U);
        assert(worker.rendered_frames == 0U);
    }
    assert(np2opngen_e1b_worker_step(&worker) ==
           NP2_OPNGEN_E1B_STEP_COMPLETE);
    assert(pcm.frames == 240U);
    assert(np2opngen_synth_event_trace_finish(
               &producer_trace, &producer_count, &producer_crc, producer_sha) ==
           NP2_SYNTH_EVENT_STATUS_OK);
    assert(np2opngen_e1b_worker_event_trace_finish(
               &worker, &consumer_count, &consumer_crc, consumer_sha) ==
           NP2_SYNTH_EVENT_STATUS_OK);
    assert(producer_count == consumer_count && producer_crc == consumer_crc &&
           memcmp(producer_sha, consumer_sha, sizeof(producer_sha)) == 0);
    np2opngen_e1b_worker_destroy(&worker);
    printf("E1C_PATH_SAME_TIMESTAMP_BURST=PASS\n");
}

struct abort_case {
    struct np2opngen_spsc_queue queue;
    struct np2opngen_e1b_control control;
    struct np2opngen_e1b_worker worker;
    struct np2opngen_e1b_pcm_sink sink;
    struct rolling_pcm pcm;
    struct np2opngen_synth_event events[9];
    atomic_bool wait_observed;
    atomic_bool start_gate;
    uint64_t expected_events;
    uint64_t end_frame;
};

static void *abort_worker_thread(void *context)
{
    struct abort_case *test = (struct abort_case *)context;
    int step;
    if (np2opngen_e1b_worker_init_with_sink(
            &test->worker, &test->queue, &test->control, test->end_frame, 0U,
            test->expected_events, &test->sink) != 0) {
        atomic_store_explicit(&test->wait_observed, true,
                              memory_order_release);
        return 0;
    }
    while (!atomic_load_explicit(&test->start_gate, memory_order_acquire)) {
        sched_yield();
    }
    do {
        step = np2opngen_e1b_worker_step(&test->worker);
        if (step == NP2_OPNGEN_E1B_STEP_WAIT) {
            atomic_store_explicit(&test->wait_observed, true,
                                  memory_order_release);
            sched_yield();
        }
    } while (step != NP2_OPNGEN_E1B_STEP_COMPLETE &&
             step != NP2_OPNGEN_E1B_STEP_FAILED);
    return 0;
}

static void *generator_failure_thread(void *context)
{
    struct abort_case *test = (struct abort_case *)context;
    while (!atomic_load_explicit(&test->wait_observed,
                                 memory_order_acquire)) {
        sched_yield();
    }
    np2opngen_e1b_control_fail(&test->control,
                               NP2_OPNGEN_E1B_ERROR_GENERATOR);
    return 0;
}

static void *sink_failure_producer_thread(void *context)
{
    struct abort_case *test = (struct abort_case *)context;
    unsigned i;
    for (i = 0U; i < 9U; ++i) {
        for (;;) {
            const int status = np2opngen_spsc_enqueue(&test->queue,
                                                       &test->events[i]);
            if (status == NP2_OPNGEN_SPSC_OK) {
                break;
            }
            if (status != NP2_OPNGEN_SPSC_FULL) {
                np2opngen_e1b_control_fail(&test->control,
                                           NP2_OPNGEN_E1B_ERROR_QUEUE);
                return 0;
            }
            atomic_store_explicit(&test->wait_observed, true,
                                  memory_order_release);
            if (np2opngen_e1b_control_first_error(&test->control) !=
                NP2_OPNGEN_E1B_ERROR_NONE) {
                return 0;
            }
            sched_yield();
        }
    }
    atomic_store_explicit(&test->control.producer_done, true,
                          memory_order_release);
    return 0;
}

static void test_abort_protocol(void)
{
    struct abort_case generator = {0};
    struct abort_case sink = {0};
    pthread_t worker_thread;
    pthread_t producer_thread;
    unsigned i;

    np2opngen_spsc_init(&generator.queue);
    np2opngen_e1b_control_init(&generator.control);
    generator.end_frame = 480U;
    generator.expected_events = 1U;
    generator.sink.write = 0;
    atomic_init(&generator.wait_observed, false);
    atomic_init(&generator.start_gate, true);
    assert(pthread_create(&worker_thread, 0, abort_worker_thread, &generator) ==
           0);
    assert(pthread_create(&producer_thread, 0, generator_failure_thread,
                          &generator) == 0);
    while (!atomic_load_explicit(&generator.wait_observed,
                                 memory_order_acquire)) {
        sched_yield();
    }
    np2opngen_e1b_control_fail(&generator.control,
                               NP2_OPNGEN_E1B_ERROR_GENERATOR);
    assert(pthread_join(producer_thread, 0) == 0);
    assert(pthread_join(worker_thread, 0) == 0);
    assert(generator.worker.state == NP2_OPNGEN_E1B_FAILED);
    assert(np2opngen_e1b_control_first_error(&generator.control) ==
           NP2_OPNGEN_E1B_ERROR_GENERATOR);
    np2opngen_e1b_worker_destroy(&generator.worker);

    np2opngen_spsc_init(&sink.queue);
    np2opngen_e1b_control_init(&sink.control);
    sink.end_frame = 480U;
    sink.expected_events = 9U;
    sink.sink.write = failing_pcm_sink;
    sink.sink.context = &sink.pcm;
    atomic_init(&sink.wait_observed, false);
    atomic_init(&sink.start_gate, false);
    for (i = 0U; i < 9U; ++i) {
        make_test_event(&sink.events[i], 240U, i);
    }
    assert(pthread_create(&worker_thread, 0, abort_worker_thread, &sink) == 0);
    assert(pthread_create(&producer_thread, 0, sink_failure_producer_thread,
                          &sink) == 0);
    while (!atomic_load_explicit(&sink.wait_observed, memory_order_acquire)) {
        sched_yield();
    }
    atomic_store_explicit(&sink.start_gate, true, memory_order_release);
    assert(pthread_join(producer_thread, 0) == 0);
    assert(pthread_join(worker_thread, 0) == 0);
    assert(sink.worker.state == NP2_OPNGEN_E1B_FAILED);
    assert(np2opngen_e1b_control_first_error(&sink.control) ==
           NP2_OPNGEN_E1B_ERROR_OUTPUT_SINK);
    np2opngen_e1b_worker_destroy(&sink.worker);
    printf("E1C_PATH_ABORT_PROTOCOL=PASS generator=PASS sink=PASS\n");
}

static int run_sustained(enum np2opngen_synthetic_profile profile,
                         uint32_t duration_seconds)
{
    struct sustained_run run = {0};
    pthread_t worker_thread;
    pthread_t producer_thread;
    uint8_t producer_sha[NP2_SHA256_DIGEST_SIZE];
    uint8_t consumer_sha[NP2_SHA256_DIGEST_SIZE];
    uint64_t producer_count;
    uint64_t consumer_count;
    uint32_t producer_crc;
    uint32_t consumer_crc;
    uint64_t timing_sum = 0U;
    size_t i;
    const uint64_t expected_frames = (uint64_t)duration_seconds *
                                     NP2_OPNGEN_SYNTHETIC_RATE_HZ;
    run.expected_events = np2opngen_synthetic_workload_expected_events(
        profile, duration_seconds);
    if (run.expected_events == UINT64_MAX || run.expected_events > MAX_TEST_EVENTS ||
        np2opngen_synthetic_workload_init(&run.workload, profile,
                                          duration_seconds) != 0) {
        return 1;
    }
    run.end_frame = expected_frames;
    run.timing.capacity = (size_t)run.expected_events;
    run.timing.samples = (uint64_t *)calloc(run.timing.capacity,
                                            sizeof(*run.timing.samples));
    if (run.timing.samples == 0) {
        return 1;
    }
    np2opngen_spsc_init(&run.queue);
    np2opngen_e1b_control_init(&run.control);
    run.pcm.crc32 = np2_crc32_iso_hdlc_init();
    np2_sha256_init(&run.pcm.sha256);
    np2opngen_synth_event_trace_init(&run.producer_trace);
    run.sink.write = rolling_pcm_sink;
    run.sink.context = &run.pcm;
    assert(pthread_create(&worker_thread, 0, sustained_worker_thread, &run) ==
           0);
    assert(pthread_create(&producer_thread, 0, sustained_producer_thread, &run) ==
           0);
    assert(pthread_join(producer_thread, 0) == 0);
    assert(pthread_join(worker_thread, 0) == 0);
    assert(run.worker.state == NP2_OPNGEN_E1B_COMPLETE);
    assert(run.producer_count == run.expected_events);
    assert(run.worker.dequeue_count == run.expected_events);
    assert(run.worker.sequence_errors == 0U);
    assert(run.worker.rendered_frames == expected_frames);
    assert(run.pcm.frames == expected_frames);
    assert(run.pcm.bytes == expected_frames * PCM_BYTES_PER_FRAME);
    assert(!run.pcm.failed);
    assert(np2opngen_synth_event_trace_finish(
               &run.producer_trace, &producer_count, &producer_crc,
               producer_sha) == NP2_SYNTH_EVENT_STATUS_OK);
    assert(np2opngen_e1b_worker_event_trace_finish(
               &run.worker, &consumer_count, &consumer_crc, consumer_sha) ==
           NP2_SYNTH_EVENT_STATUS_OK);
    assert(producer_count == run.expected_events &&
           consumer_count == run.expected_events && producer_crc == consumer_crc &&
           memcmp(producer_sha, consumer_sha, sizeof(producer_sha)) == 0);
    finish_timing(&run.timing);
    for (i = 0U; i < run.timing.count; ++i) {
        timing_sum += run.timing.samples[i];
    }
    printf("E1C_WORKLOAD_META version=%u profile=%s sample_rate=%u"
           " duration_frames=%" PRIu64 " warmup_frames=%u quantum=%u\n",
           NP2_OPNGEN_SYNTHETIC_WORKLOAD_VERSION,
           np2opngen_synthetic_profile_name(profile),
           NP2_OPNGEN_SYNTHETIC_RATE_HZ, expected_frames,
           NP2_OPNGEN_SYNTHETIC_WARMUP_FRAMES,
           NP2_OPNGEN_SYNTHETIC_QUANTUM);
    printf("E1C_EVENTS produced=%" PRIu64 " consumed=%" PRIu64
           " producer_crc32=0x%08" PRIx32 " producer_sha256=",
           producer_count, consumer_count, producer_crc);
    print_sha256(producer_sha);
    printf(" consumer_crc32=0x%08" PRIx32 " consumer_sha256=", consumer_crc);
    print_sha256(consumer_sha);
    printf(" sequence_errors=%" PRIu64 "\n", run.worker.sequence_errors);
    printf("E1C_PCM frames=%" PRIu64 " bytes=%" PRIu64
           " crc32=0x%08" PRIx32 " sha256=",
           run.pcm.frames, run.pcm.bytes,
           np2_crc32_iso_hdlc_finish(run.pcm.crc32));
    np2_sha256_final(&run.pcm.sha256, consumer_sha);
    print_sha256(consumer_sha);
    printf("\nE1C_QUEUE capacity=%u enqueue=%" PRIu64 " dequeue=%" PRIu64
           " max_occupancy=%u full_waits=%" PRIu64 " empty_waits=%" PRIu64
           "\n",
           NP2_OPNGEN_SPSC_CAPACITY, run.producer_count,
           run.worker.dequeue_count, run.max_occupancy, run.full_waits,
           run.worker.empty_wait_count);
    if (run.timing.count != 0U) {
        printf("E1C_TIMING diagnostic=1 active_service_count=%zu min_us=%" PRIu64
               " mean_us=%" PRIu64 " p50_us=%" PRIu64 " p95_us=%" PRIu64
               " p99_us=%" PRIu64 " max_us=%" PRIu64
               " over_5000us=%" PRIu64 " max_consecutive_over_5000=%" PRIu64
               " pure_opngen=DEFERRED\n",
               run.timing.count, run.timing.samples[0],
               timing_sum / run.timing.count,
               percentile(run.timing.samples, run.timing.count, 50U),
               percentile(run.timing.samples, run.timing.count, 95U),
               percentile(run.timing.samples, run.timing.count, 99U),
               run.timing.samples[run.timing.count - 1U], run.timing.over_5000,
               run.timing.max_consecutive_over_5000);
    } else {
        printf("E1C_TIMING diagnostic=1 active_service_count=0"
               " pure_opngen=DEFERRED\n");
    }
    printf("E1C_RESULT=PASS\n");
    np2opngen_e1b_worker_destroy(&run.worker);
    free(run.timing.samples);
    return 0;
}

int main(int argc, char **argv)
{
    enum np2opngen_synthetic_profile profile;
    uint32_t duration_seconds;
    char *end;
    if (argc != 5 || strcmp(argv[1], "--profile") != 0 ||
        strcmp(argv[3], "--duration-seconds") != 0 ||
        parse_profile(argv[2], &profile) != 0) {
        fprintf(stderr, "usage: %s --profile SYNTHETIC-LIGHT|SYNTHETIC-HEAVY|STRESS "
                        "--duration-seconds N\n", argv[0]);
        return 2;
    }
    duration_seconds = (uint32_t)strtoul(argv[4], &end, 10);
    if (*argv[4] == '\0' || *end != '\0' || duration_seconds == 0U ||
        duration_seconds > 60U) {
        fprintf(stderr, "duration must be 1..60 seconds\n");
        return 2;
    }
    test_counter_wrap();
    test_done_boundaries();
    test_same_timestamp_burst();
    test_abort_protocol();
    if (run_sustained(profile, duration_seconds) != 0) {
        fprintf(stderr, "E1C_RESULT=FAIL reason=setup\n");
        return 1;
    }
    return 0;
}
