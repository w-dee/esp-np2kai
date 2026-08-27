#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np2opngen_e1b_stream.h"
#include "np2opngen_fixture.h"
#include "np2opngen_s98.h"
#include "np2opngen_synthetic_workload.h"

#define RETRO_PATH "../testdata/s98/retrofm-pocket-demo-strict.s98"
#define PCM_BYTES_PER_FRAME 4U

struct capture {
    uint8_t *pcm;
    size_t pcm_bytes;
    size_t pcm_capacity;
    int failed;
};

struct observer_capture {
    uint64_t render_callbacks;
    uint64_t event_callbacks;
    uint64_t apply_callbacks;
    uint64_t startup_events;
    uint64_t last_sequence;
    uint64_t last_render_end;
    uint64_t last_apply_timestamp;
    uint64_t render_callbacks_at_apply;
    int has_sequence;
    int has_apply_timestamp;
    int boundary_crossed;
    int callback_allocated;
    int failed;
};

enum observer_source_kind {
    SOURCE_S98,
    SOURCE_E1_VECTOR,
    SOURCE_SYNTHETIC,
};

struct harness {
    enum observer_source_kind kind;
    const uint8_t *source_data;
    size_t source_size;
    const struct np2opngen_synth_event *events;
    size_t vector_event_count;
    enum np2opngen_synthetic_profile profile;
    uint32_t duration_seconds;
    uint64_t event_count;
    uint64_t end_frame;
    struct np2opngen_spsc_queue queue;
    struct np2opngen_e1b_control control;
    struct np2opngen_e1b_worker worker;
    struct np2opngen_e1b_observer observer;
    struct observer_capture observation;
    struct capture capture;
    int enabled;
    int worker_status;
    int producer_status;
    pthread_t worker_thread;
    pthread_t producer_thread;
};

static int sink_write(const uint8_t *pcm, size_t bytes, uint64_t offset,
                      void *opaque)
{
    struct capture *capture = (struct capture *)opaque;
    size_t expected = (size_t)offset * PCM_BYTES_PER_FRAME;
    if (capture == NULL || pcm == NULL || expected != capture->pcm_bytes ||
        bytes > SIZE_MAX - expected) {
        if (capture != NULL) capture->failed = 1;
        return -1;
    }
    if (expected + bytes > capture->pcm_capacity) {
        size_t capacity = capture->pcm_capacity == 0U ? 4096U : capture->pcm_capacity;
        while (capacity < expected + bytes) capacity *= 2U;
        uint8_t *grown = (uint8_t *)realloc(capture->pcm, capacity);
        if (grown == NULL) {
            capture->failed = 1;
            return -1;
        }
        capture->pcm = grown;
        capture->pcm_capacity = capacity;
    }
    memcpy(capture->pcm + expected, pcm, bytes);
    capture->pcm_bytes = expected + bytes;
    return 0;
}

static void observe_event_begin(void *opaque,
                                const struct np2opngen_synth_event *event,
                                bool timestamp_zero)
{
    struct observer_capture *capture = (struct observer_capture *)opaque;
    ++capture->event_callbacks;
    if (timestamp_zero) ++capture->startup_events;
    if (capture->has_sequence && event->sequence != capture->last_sequence + 1U)
        capture->failed = 1;
    capture->last_sequence = event->sequence;
    capture->has_sequence = 1;
}

static void observe_apply_begin(void *opaque,
                                const struct np2opngen_synth_event *event)
{
    struct observer_capture *capture = (struct observer_capture *)opaque;
    ++capture->apply_callbacks;
    if (event->sample_timestamp != 0U &&
        event->sample_timestamp % NP2_OPNGEN_E1B_RENDER_QUANTUM == 0U &&
        capture->last_render_end != event->sample_timestamp)
        capture->failed = 1;
    if (capture->has_apply_timestamp &&
        capture->last_apply_timestamp == event->sample_timestamp &&
        capture->render_callbacks_at_apply != capture->render_callbacks)
        capture->failed = 1;
    capture->last_apply_timestamp = event->sample_timestamp;
    capture->render_callbacks_at_apply = capture->render_callbacks;
    capture->has_apply_timestamp = 1;
}

static void observe_render_begin(void *opaque, uint64_t offset, uint32_t count)
{
    struct observer_capture *capture = (struct observer_capture *)opaque;
    ++capture->render_callbacks;
    capture->last_render_end = offset + count;
    if (offset % NP2_OPNGEN_E1B_RENDER_QUANTUM + count >
        NP2_OPNGEN_E1B_RENDER_QUANTUM)
        capture->boundary_crossed = 1;
}

static int load_file(const char *path, uint8_t **data_out, size_t *size_out)
{
    FILE *file = fopen(path, "rb");
    long length;
    uint8_t *data;
    if (file == NULL || fseek(file, 0L, SEEK_END) != 0) return -1;
    length = ftell(file);
    if (length < 0L || fseek(file, 0L, SEEK_SET) != 0) return -1;
    data = (uint8_t *)malloc((size_t)length);
    if (data == NULL || fread(data, 1U, (size_t)length, file) != (size_t)length)
        return -1;
    fclose(file);
    *data_out = data;
    *size_out = (size_t)length;
    return 0;
}

static void *worker_main(void *opaque)
{
    struct harness *harness = (struct harness *)opaque;
    for (;;) {
        const int step = np2opngen_e1b_worker_step(&harness->worker);
        if (step == NP2_OPNGEN_E1B_STEP_COMPLETE ||
            step == NP2_OPNGEN_E1B_STEP_FAILED) {
            harness->worker_status = step;
            return NULL;
        }
        sched_yield();
    }
}

static void *producer_main(void *opaque)
{
    struct harness *harness = (struct harness *)opaque;
    struct np2opngen_synth_event event;
    int result;
    harness->producer_status = -1;
    if (harness->kind == SOURCE_S98) {
        struct np2opngen_s98_parser parser;
        if (np2opngen_s98_parser_init(&parser, harness->source_data,
                                      harness->source_size) != 0)
            return NULL;
        for (;;) {
            result = np2opngen_s98_parser_next(&parser, &event);
            if (result == NP2_OPNGEN_S98_NEXT_END) break;
            if (result != NP2_OPNGEN_S98_NEXT_EVENT) return NULL;
            while ((result = np2opngen_spsc_enqueue(&harness->queue, &event)) ==
                   NP2_OPNGEN_SPSC_FULL)
                sched_yield();
            if (result != NP2_OPNGEN_SPSC_OK) return NULL;
        }
    } else if (harness->kind == SOURCE_E1_VECTOR) {
        for (size_t i = 0; i < harness->vector_event_count; ++i) {
            event = harness->events[i];
            while ((result = np2opngen_spsc_enqueue(&harness->queue, &event)) ==
                   NP2_OPNGEN_SPSC_FULL)
                sched_yield();
            if (result != NP2_OPNGEN_SPSC_OK) return NULL;
        }
    } else {
        struct np2opngen_synthetic_workload workload;
        if (np2opngen_synthetic_workload_init(
                &workload, harness->profile, harness->duration_seconds) != 0)
            return NULL;
        for (;;) {
            result = np2opngen_synthetic_workload_peek(&workload, &event);
            if (result == 0) break;
            if (result < 0) return NULL;
            while ((result = np2opngen_spsc_enqueue(&harness->queue, &event)) ==
                   NP2_OPNGEN_SPSC_FULL)
                sched_yield();
            if (result != NP2_OPNGEN_SPSC_OK ||
                np2opngen_synthetic_workload_commit(&workload) != 0)
                return NULL;
        }
    }
    harness->producer_status = 0;
    np2opngen_e1b_control_producer_done(&harness->control);
    return NULL;
}

static int run_case(enum observer_source_kind kind, const uint8_t *source_data,
                    size_t source_size,
                    const struct np2opngen_synth_event *events,
                    size_t vector_event_count,
                    enum np2opngen_synthetic_profile profile,
                    uint32_t duration_seconds, uint64_t expected_events,
                    uint64_t end_frame, int enabled, struct capture *out,
                    struct observer_capture *observation)
{
    struct harness harness;
    const struct np2opngen_e1b_pcm_sink sink = {sink_write, &harness.capture};
    memset(&harness, 0, sizeof(harness));
    harness.kind = kind;
    harness.source_data = source_data;
    harness.source_size = source_size;
    harness.events = events;
    harness.vector_event_count = vector_event_count;
    harness.profile = profile;
    harness.duration_seconds = duration_seconds;
    harness.event_count = expected_events;
    harness.end_frame = end_frame;
    harness.enabled = enabled;
    np2opngen_spsc_init(&harness.queue);
    np2opngen_e1b_control_init(&harness.control);
    if (np2opngen_e1b_worker_init_with_sink(
            &harness.worker, &harness.queue, &harness.control, end_frame, 0U,
            expected_events, &sink) != 0)
        return -1;
    if (enabled) {
        harness.observer.event_begin = observe_event_begin;
        harness.observer.event_apply_begin = observe_apply_begin;
        harness.observer.render_begin = observe_render_begin;
        harness.observer.context = &harness.observation;
        harness.observer.boundary_limiter = true;
        np2opngen_e1b_worker_set_observer(&harness.worker, &harness.observer);
    }
    if (pthread_create(&harness.worker_thread, NULL, worker_main, &harness) != 0 ||
        pthread_create(&harness.producer_thread, NULL, producer_main, &harness) != 0) {
        np2opngen_e1b_control_fail(&harness.control,
                                   NP2_OPNGEN_E1B_ERROR_THREAD);
        np2opngen_e1b_control_producer_done(&harness.control);
        pthread_join(harness.worker_thread, NULL);
        np2opngen_e1b_worker_destroy(&harness.worker);
        return -1;
    }
    pthread_join(harness.producer_thread, NULL);
    pthread_join(harness.worker_thread, NULL);
    const int ok = harness.worker_status == NP2_OPNGEN_E1B_STEP_COMPLETE &&
                   harness.producer_status == 0 && harness.capture.failed == 0;
    if (out != NULL) *out = harness.capture;
    if (observation != NULL) *observation = harness.observation;
    np2opngen_e1b_worker_destroy(&harness.worker);
    return ok ? 0 : -1;
}

int main(void)
{
    uint8_t *source = NULL;
    size_t source_size = 0U;
    struct np2opngen_s98_parser parser;
    struct np2opngen_synth_event event;
    const struct np2opngen_synth_event *e1_events = NULL;
    size_t e1_event_count = 0U;
    uint64_t e1_end_frame = 0U;
    const uint64_t synthetic_events =
        np2opngen_synthetic_workload_expected_events(
            NP2_OPNGEN_SYNTHETIC_LIGHT, 1U);
    uint64_t event_count = 0U;
    struct capture disabled = {0};
    struct capture enabled = {0};
    struct observer_capture observation = {0};
    struct capture e1_disabled = {0};
    struct capture e1_enabled = {0};
    struct observer_capture e1_observation = {0};
    struct capture synthetic_disabled = {0};
    struct capture synthetic_enabled = {0};
    struct observer_capture synthetic_observation = {0};
    int result;

    assert(load_file(RETRO_PATH, &source, &source_size) == 0);
    assert(np2opngen_s98_parser_init(&parser, source, source_size) == 0);
    do {
        result = np2opngen_s98_parser_next(&parser, &event);
        if (result == NP2_OPNGEN_S98_NEXT_EVENT) ++event_count;
    } while (result == NP2_OPNGEN_S98_NEXT_EVENT);
    assert(result == NP2_OPNGEN_S98_NEXT_END);
    assert(event_count == 1047U && parser.metadata.end_frame == 576960U);
    assert(np2opngen_fixture_get_e1_events(
               &e1_events, &e1_event_count, &e1_end_frame) == 0);
    assert(e1_events != NULL && e1_event_count != 0U && e1_end_frame != 0U);
    assert(synthetic_events != UINT64_MAX);

    assert(run_case(SOURCE_S98, source, source_size, NULL, 0U,
                    NP2_OPNGEN_SYNTHETIC_LIGHT, 0U, event_count,
                    parser.metadata.end_frame, 0, &disabled, NULL) == 0);
    assert(run_case(SOURCE_S98, source, source_size, NULL, 0U,
                    NP2_OPNGEN_SYNTHETIC_LIGHT, 0U, event_count,
                    parser.metadata.end_frame, 1, &enabled, &observation) == 0);
    assert(disabled.pcm_bytes == enabled.pcm_bytes &&
           memcmp(disabled.pcm, enabled.pcm, disabled.pcm_bytes) == 0);
    assert(observation.event_callbacks == event_count &&
           observation.apply_callbacks == event_count &&
           observation.startup_events == 99U && observation.render_callbacks > 0U &&
           observation.boundary_crossed == 0 && observation.callback_allocated == 0 &&
           observation.failed == 0);

    assert(run_case(SOURCE_E1_VECTOR, NULL, 0U, e1_events, e1_event_count,
                    NP2_OPNGEN_SYNTHETIC_LIGHT, 0U, e1_event_count,
                    e1_end_frame, 0, &e1_disabled, NULL) == 0);
    assert(run_case(SOURCE_E1_VECTOR, NULL, 0U, e1_events, e1_event_count,
                    NP2_OPNGEN_SYNTHETIC_LIGHT, 0U, e1_event_count,
                    e1_end_frame, 1, &e1_enabled, &e1_observation) == 0);
    assert(e1_disabled.pcm_bytes == e1_enabled.pcm_bytes &&
           memcmp(e1_disabled.pcm, e1_enabled.pcm, e1_disabled.pcm_bytes) == 0);
    assert(e1_observation.event_callbacks == e1_event_count &&
           e1_observation.apply_callbacks == e1_event_count &&
           e1_observation.startup_events == 0U &&
           e1_observation.boundary_crossed == 0 &&
           e1_observation.callback_allocated == 0 && e1_observation.failed == 0);

    assert(run_case(SOURCE_SYNTHETIC, NULL, 0U, NULL, 0U,
                    NP2_OPNGEN_SYNTHETIC_LIGHT, 1U, synthetic_events,
                    48000U, 0, &synthetic_disabled, NULL) == 0);
    assert(run_case(SOURCE_SYNTHETIC, NULL, 0U, NULL, 0U,
                    NP2_OPNGEN_SYNTHETIC_LIGHT, 1U, synthetic_events,
                    48000U, 1, &synthetic_enabled, &synthetic_observation) == 0);
    assert(synthetic_disabled.pcm_bytes == synthetic_enabled.pcm_bytes &&
           memcmp(synthetic_disabled.pcm, synthetic_enabled.pcm,
                  synthetic_disabled.pcm_bytes) == 0);
    assert(synthetic_observation.event_callbacks == synthetic_events &&
           synthetic_observation.apply_callbacks == synthetic_events &&
           synthetic_observation.startup_events == 84U &&
           synthetic_observation.boundary_crossed == 0 &&
           synthetic_observation.callback_allocated == 0 &&
           synthetic_observation.failed == 0);
    printf("P4_AUDIO_HOST_TEST A_disabled_compat=PASS B_enabled_pcm=PASS"
           " C_no_boundary_cross=PASS D_event_boundary=PASS E_same_timestamp_order=PASS"
           " F_timestamp_zero_startup=PASS G_identity_unchanged=PASS H_no_hot_alloc=PASS\n");
    free(disabled.pcm);
    free(enabled.pcm);
    free(e1_disabled.pcm);
    free(e1_enabled.pcm);
    free(synthetic_disabled.pcm);
    free(synthetic_enabled.pcm);
    free(source);
    return 0;
}
