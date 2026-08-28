#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np2opngen_pcm_ring.h"
#include "np2opngen_e1b_stream.h"
#include "np2opngen_synth_event.h"

static void make_pcm(uint8_t *pcm, size_t first_frame, size_t frames)
{
    size_t i;
    for (i = 0U; i < frames; ++i) {
        const uint32_t n = (uint32_t)(first_frame + i);
        pcm[i * 4U] = (uint8_t)n;
        pcm[i * 4U + 1U] = (uint8_t)(n >> 8U);
        pcm[i * 4U + 2U] = (uint8_t)(n ^ 0xa5U);
        pcm[i * 4U + 3U] = (uint8_t)((n >> 8U) ^ 0x5aU);
    }
}

static void check_slot(const struct np2opngen_pcm_ring_slot *slot,
                       uint64_t offset, uint32_t sequence, uint16_t frames,
                       uint16_t flags)
{
    uint8_t expected[NP2_OPNGEN_PCM_RING_SLOT_BYTES];
    assert(slot != NULL);
    assert(slot->frame_offset == offset);
    assert(slot->sequence == sequence);
    assert(slot->valid_frames == frames);
    assert(slot->flags == flags);
    make_pcm(expected, (size_t)offset, frames);
    assert(memcmp(slot->pcm, expected,
                  (size_t)frames * NP2_OPNGEN_PCM_RING_BYTES_PER_FRAME) == 0);
}

static void test_basic_and_capacity(void)
{
    struct np2opngen_pcm_ring ring;
    const struct np2opngen_pcm_ring_slot *slot;
    uint8_t pcm[NP2_OPNGEN_PCM_RING_SLOT_BYTES];
    size_t consumed;
    unsigned i;

    np2opngen_pcm_ring_init(&ring);
    assert(np2opngen_pcm_ring_occupancy(&ring) == 0U);
    assert(np2opngen_pcm_ring_try_peek(&ring, &slot) ==
           NP2_OPNGEN_PCM_RING_EMPTY);
    assert(slot == NULL);
    make_pcm(pcm, 0U, 240U);
    assert(np2opngen_pcm_ring_append(&ring, pcm, 240U, 0U, &consumed) ==
           NP2_OPNGEN_PCM_RING_OK);
    assert(consumed == 240U);
    assert(np2opngen_pcm_ring_occupancy(&ring) == 1U);
    assert(np2opngen_pcm_ring_try_peek(&ring, &slot) ==
           NP2_OPNGEN_PCM_RING_OK);
    check_slot(slot, 0U, 0U, 240U, 0U);
    assert(np2opngen_pcm_ring_consume(&ring) == NP2_OPNGEN_PCM_RING_OK);
    assert(np2opngen_pcm_ring_occupancy(&ring) == 0U);
    assert(np2opngen_pcm_ring_consume(&ring) == NP2_OPNGEN_PCM_RING_EMPTY);

    np2opngen_pcm_ring_init(&ring);
    for (i = 0U; i < NP2_OPNGEN_PCM_RING_CAPACITY; ++i) {
        make_pcm(pcm, (size_t)i * 240U, 240U);
        assert(np2opngen_pcm_ring_append(&ring, pcm, 240U,
                                         (uint64_t)i * 240U, &consumed) ==
               NP2_OPNGEN_PCM_RING_OK);
        assert(consumed == 240U);
    }
    assert(np2opngen_pcm_ring_occupancy(&ring) ==
           NP2_OPNGEN_PCM_RING_CAPACITY);
    assert(np2opngen_pcm_ring_append(&ring, pcm, 1U, 1920U, &consumed) ==
           NP2_OPNGEN_PCM_RING_FULL);
    assert(consumed == 0U);
    assert(np2opngen_pcm_ring_consume(&ring) == NP2_OPNGEN_PCM_RING_OK);
    make_pcm(pcm, 1920U, 240U);
    assert(np2opngen_pcm_ring_append(&ring, pcm, 240U, 1920U, &consumed) ==
           NP2_OPNGEN_PCM_RING_OK);
    assert(consumed == 240U);
}

static void test_partitions(void)
{
    static const size_t partitions[][4] = {{1U, 239U, 0U, 0U},
                                           {239U, 1U, 0U, 0U},
                                           {17U, 31U, 79U, 113U}};
    unsigned p;
    for (p = 0U; p < sizeof(partitions) / sizeof(partitions[0]); ++p) {
        struct np2opngen_pcm_ring ring;
        uint8_t pcm[240U * 4U];
        size_t offset = 0U;
        size_t i;
        np2opngen_pcm_ring_init(&ring);
        make_pcm(pcm, 0U, 240U);
        for (i = 0U; i < 4U && partitions[p][i] != 0U; ++i) {
            size_t consumed = 0U;
            const struct np2opngen_pcm_ring_slot *slot = NULL;
            assert(np2opngen_pcm_ring_append(
                       &ring, pcm + offset * 4U, partitions[p][i], offset,
                       &consumed) == NP2_OPNGEN_PCM_RING_OK);
            assert(consumed == partitions[p][i]);
            offset += consumed;
            if (offset < 240U)
                assert(np2opngen_pcm_ring_try_peek(&ring, &slot) ==
                       NP2_OPNGEN_PCM_RING_EMPTY);
        }
        assert(np2opngen_pcm_ring_occupancy(&ring) == 1U);
        {
            const struct np2opngen_pcm_ring_slot *slot = NULL;
            assert(np2opngen_pcm_ring_try_peek(&ring, &slot) ==
                   NP2_OPNGEN_PCM_RING_OK);
            check_slot(slot, 0U, 0U, 240U, 0U);
        }
    }
}

static void test_cross_slot_and_retry(void)
{
    struct np2opngen_pcm_ring ring;
    uint8_t pcm[500U * 4U];
    size_t consumed;
    const struct np2opngen_pcm_ring_slot *slot;
    unsigned i;

    np2opngen_pcm_ring_init(&ring);
    make_pcm(pcm, 0U, 500U);
    assert(np2opngen_pcm_ring_append(&ring, pcm, 500U, 0U, &consumed) ==
           NP2_OPNGEN_PCM_RING_OK);
    assert(consumed == 500U);
    assert(np2opngen_pcm_ring_occupancy(&ring) == 2U);
    assert(np2opngen_pcm_ring_producer_partial_valid_frames(&ring) == 20U);
    assert(np2opngen_pcm_ring_try_peek(&ring, &slot) ==
           NP2_OPNGEN_PCM_RING_OK);
    check_slot(slot, 0U, 0U, 240U, 0U);
    assert(np2opngen_pcm_ring_consume(&ring) == NP2_OPNGEN_PCM_RING_OK);
    assert(np2opngen_pcm_ring_try_peek(&ring, &slot) ==
           NP2_OPNGEN_PCM_RING_OK);
    check_slot(slot, 240U, 1U, 240U, 0U);

    /* Seven committed slots plus an unpublished partial slot. */
    np2opngen_pcm_ring_init(&ring);
    for (i = 0U; i < 7U; ++i) {
        make_pcm(pcm, (size_t)i * 240U, 240U);
        assert(np2opngen_pcm_ring_append(&ring, pcm, 240U,
                                         (uint64_t)i * 240U, &consumed) ==
               NP2_OPNGEN_PCM_RING_OK);
    }
    make_pcm(pcm, 1680U, 100U);
    assert(np2opngen_pcm_ring_append(&ring, pcm, 100U, 1680U, &consumed) ==
           NP2_OPNGEN_PCM_RING_OK);
    make_pcm(pcm, 1780U, 500U);
    assert(np2opngen_pcm_ring_append(&ring, pcm, 500U, 1780U, &consumed) ==
           NP2_OPNGEN_PCM_RING_FULL);
    assert(consumed == 140U);
    assert(np2opngen_pcm_ring_occupancy(&ring) == 8U);
    assert(np2opngen_pcm_ring_producer_partial_valid_frames(&ring) == 0U);
    assert(np2opngen_pcm_ring_consume(&ring) == NP2_OPNGEN_PCM_RING_OK);
    assert(np2opngen_pcm_ring_append(&ring, pcm + 140U * 4U, 360U, 1920U,
                                     &consumed) == NP2_OPNGEN_PCM_RING_FULL);
    assert(consumed == 240U);
    assert(np2opngen_pcm_ring_consume(&ring) == NP2_OPNGEN_PCM_RING_OK);
    assert(np2opngen_pcm_ring_append(&ring, pcm + 380U * 4U, 120U, 2160U,
                                     &consumed) == NP2_OPNGEN_PCM_RING_OK);
    assert(consumed == 120U);
}

static void test_finish_and_errors(void)
{
    struct np2opngen_pcm_ring ring;
    uint8_t pcm[20U * 4U];
    uint8_t snapshot[sizeof(ring.slots)];
    const struct np2opngen_pcm_ring_slot *slot;
    size_t consumed;

    np2opngen_pcm_ring_init(&ring);
    make_pcm(pcm, 0U, 20U);
    assert(np2opngen_pcm_ring_append(&ring, pcm, 20U, 0U, &consumed) ==
           NP2_OPNGEN_PCM_RING_OK);
    assert(np2opngen_pcm_ring_try_peek(&ring, &slot) ==
           NP2_OPNGEN_PCM_RING_EMPTY);
    assert(np2opngen_pcm_ring_finish(&ring, 19U) ==
           NP2_OPNGEN_PCM_RING_OFFSET);
    assert(np2opngen_pcm_ring_finish(&ring, 20U) == NP2_OPNGEN_PCM_RING_OK);
    assert(np2opngen_pcm_ring_occupancy(&ring) == 1U);
    assert(np2opngen_pcm_ring_try_peek(&ring, &slot) ==
           NP2_OPNGEN_PCM_RING_OK);
    check_slot(slot, 0U, 0U, 20U, NP2_OPNGEN_PCM_RING_FLAG_FINAL_PARTIAL);
    memcpy(snapshot, ring.slots, sizeof(snapshot));
    assert(np2opngen_pcm_ring_append(&ring, pcm, 1U, 20U, &consumed) ==
           NP2_OPNGEN_PCM_RING_FINALIZED);
    assert(np2opngen_pcm_ring_finish(&ring, 20U) ==
           NP2_OPNGEN_PCM_RING_FINALIZED);
    assert(memcmp(snapshot, ring.slots, sizeof(snapshot)) == 0);

    np2opngen_pcm_ring_init(&ring);
    assert(np2opngen_pcm_ring_append(&ring, NULL, 1U, 0U, &consumed) ==
           NP2_OPNGEN_PCM_RING_ARGUMENT);
    assert(np2opngen_pcm_ring_append(&ring, pcm, 1U, 1U, &consumed) ==
           NP2_OPNGEN_PCM_RING_OFFSET);
    assert(np2opngen_pcm_ring_finish(&ring, 0U) ==
           NP2_OPNGEN_PCM_RING_OK);
    assert(np2opngen_pcm_ring_occupancy(&ring) == 0U);
    assert(np2opngen_pcm_ring_finish(NULL, 0U) ==
           NP2_OPNGEN_PCM_RING_ARGUMENT);
    assert(np2opngen_pcm_ring_try_peek(&ring, NULL) ==
           NP2_OPNGEN_PCM_RING_ARGUMENT);
}

struct thread_case {
    struct np2opngen_pcm_ring ring;
    uint8_t *pcm;
    size_t frames;
    atomic_bool producer_done;
    atomic_bool failed;
};

static void *thread_producer(void *opaque)
{
    struct thread_case *test = (struct thread_case *)opaque;
    size_t sent = 0U;
    size_t chunk = 1U;
    while (sent < test->frames) {
        size_t requested = chunk;
        size_t consumed = 0U;
        int status;
        if (requested > test->frames - sent)
            requested = test->frames - sent;
        status = np2opngen_pcm_ring_append(
            &test->ring, test->pcm + sent * 4U, requested, sent, &consumed);
        if (status != NP2_OPNGEN_PCM_RING_OK &&
            status != NP2_OPNGEN_PCM_RING_FULL) {
            atomic_store(&test->failed, true);
            return NULL;
        }
        sent += consumed;
        if (consumed == 0U)
            sched_yield();
        chunk = chunk % 503U + 1U;
    }
    while (np2opngen_pcm_ring_finish(&test->ring, test->frames) ==
           NP2_OPNGEN_PCM_RING_FULL)
        sched_yield();
    atomic_store_explicit(&test->producer_done, true, memory_order_release);
    return NULL;
}

static void *thread_consumer(void *opaque)
{
    struct thread_case *test = (struct thread_case *)opaque;
    uint32_t expected_sequence = 0U;
    uint64_t expected_offset = 0U;
    for (;;) {
        const struct np2opngen_pcm_ring_slot *slot = NULL;
        int status = np2opngen_pcm_ring_try_peek(&test->ring, &slot);
        if (status == NP2_OPNGEN_PCM_RING_EMPTY) {
            if (atomic_load_explicit(&test->producer_done, memory_order_acquire) &&
                np2opngen_pcm_ring_occupancy(&test->ring) == 0U)
                break;
            sched_yield();
            continue;
        }
        if (status != NP2_OPNGEN_PCM_RING_OK || slot == NULL ||
            slot->sequence != expected_sequence ||
            slot->frame_offset != expected_offset || slot->valid_frames == 0U ||
            slot->valid_frames > NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES ||
            memcmp(slot->pcm, test->pcm + expected_offset * 4U,
                   (size_t)slot->valid_frames * 4U) != 0 ||
            ((slot->flags & NP2_OPNGEN_PCM_RING_FLAG_FINAL_PARTIAL) != 0U &&
             slot->valid_frames == NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES)) {
            atomic_store(&test->failed, true);
            return NULL;
        }
        expected_sequence++;
        expected_offset += slot->valid_frames;
        if (np2opngen_pcm_ring_consume(&test->ring) !=
            NP2_OPNGEN_PCM_RING_OK) {
            atomic_store(&test->failed, true);
            return NULL;
        }
    }
    if (expected_offset != test->frames)
        atomic_store(&test->failed, true);
    return NULL;
}

static void test_threaded_and_wrap(void)
{
    struct thread_case test;
    pthread_t producer;
    pthread_t consumer;
    struct np2opngen_pcm_ring ring;
    uint8_t pcm[240U * 4U];
    size_t consumed;
    unsigned i;

    memset(&test, 0, sizeof(test));
    test.frames = 120123U;
    test.pcm = (uint8_t *)malloc(test.frames * 4U);
    assert(test.pcm != NULL);
    make_pcm(test.pcm, 0U, test.frames);
    np2opngen_pcm_ring_init(&test.ring);
    atomic_init(&test.producer_done, false);
    atomic_init(&test.failed, false);
    assert(pthread_create(&producer, NULL, thread_producer, &test) == 0);
    assert(pthread_create(&consumer, NULL, thread_consumer, &test) == 0);
    assert(pthread_join(producer, NULL) == 0);
    assert(pthread_join(consumer, NULL) == 0);
    assert(!atomic_load(&test.failed));
    assert(np2opngen_pcm_ring_occupancy(&test.ring) == 0U);
    free(test.pcm);

    /* Exercise uint32 head/tail wrap without a test-only production hook. */
    np2opngen_pcm_ring_init(&ring);
    atomic_store(&ring.head, UINT32_MAX - 3U);
    atomic_store(&ring.tail, UINT32_MAX - 3U);
    for (i = 0U; i < 16U; ++i) {
        make_pcm(pcm, (size_t)i * 240U, 240U);
        assert(np2opngen_pcm_ring_append(&ring, pcm, 240U,
                                         (uint64_t)i * 240U, &consumed) ==
               NP2_OPNGEN_PCM_RING_OK);
        assert(consumed == 240U);
        assert(np2opngen_pcm_ring_consume(&ring) == NP2_OPNGEN_PCM_RING_OK);
    }
}

struct finish_capture {
    unsigned writes;
    unsigned finishes;
    int finish_failure;
    int write_failure;
    enum np2opngen_e1b_state state_at_finish;
    struct np2opngen_e1b_worker *worker;
};

static int finish_sink_write(const uint8_t *pcm, size_t bytes,
                             uint64_t offset, void *opaque)
{
    struct finish_capture *capture = (struct finish_capture *)opaque;
    (void)pcm;
    (void)offset;
    if (capture->write_failure)
        return -1;
    assert(bytes != 0U);
    capture->writes++;
    return 0;
}

static int finish_sink_finish(uint64_t final_frame, void *opaque)
{
    struct finish_capture *capture = (struct finish_capture *)opaque;
    assert(final_frame == 240U);
    capture->finishes++;
    capture->state_at_finish = capture->worker->state;
    return capture->finish_failure ? -1 : 0;
}

static int run_worker_finish_case(int use_finish, int finish_failure,
                                  int write_failure, unsigned *finish_calls,
                                  enum np2opngen_e1b_state *state_out,
                                  int *error_out)
{
    struct np2opngen_spsc_queue queue;
    struct np2opngen_e1b_control control;
    struct np2opngen_e1b_worker worker;
    struct np2opngen_synth_event event = {0};
    struct finish_capture capture = {0};
    struct np2opngen_e1b_pcm_sink sink = {finish_sink_write, &capture, NULL};
    int step;

    event.type = NP2_SYNTH_EVENT_KEY_EVENT;
    event.sequence = 0U;
    event.sample_timestamp = 0U;
    event.payload.key_event.channel = 0U;
    np2opngen_spsc_init(&queue);
    np2opngen_e1b_control_init(&control);
    capture.finish_failure = finish_failure;
    capture.write_failure = write_failure;
    if (use_finish)
        sink.finish = finish_sink_finish;
    capture.worker = &worker;
    assert(np2opngen_spsc_enqueue(&queue, &event) == NP2_OPNGEN_SPSC_OK);
    np2opngen_e1b_control_producer_done(&control);
    assert(np2opngen_e1b_worker_init_with_sink(
               &worker, &queue, &control, 240U, 0U, 1U, &sink) == 0);
    do {
        step = np2opngen_e1b_worker_step(&worker);
    } while (step == NP2_OPNGEN_E1B_STEP_PROGRESS ||
             step == NP2_OPNGEN_E1B_STEP_WAIT);
    *finish_calls = capture.finishes;
    *state_out = worker.state;
    *error_out = np2opngen_e1b_control_first_error(&control);
    if (use_finish && !finish_failure && !write_failure)
        assert(capture.state_at_finish == NP2_OPNGEN_E1B_DRAIN);
    np2opngen_e1b_worker_destroy(&worker);
    return step;
}

static void test_worker_finish_seam(void)
{
    unsigned finish_calls;
    enum np2opngen_e1b_state state;
    int error;
    assert(run_worker_finish_case(0, 0, 0, &finish_calls, &state, &error) ==
           NP2_OPNGEN_E1B_STEP_COMPLETE);
    assert(finish_calls == 0U && state == NP2_OPNGEN_E1B_COMPLETE && error == 0);
    assert(run_worker_finish_case(1, 0, 0, &finish_calls, &state, &error) ==
           NP2_OPNGEN_E1B_STEP_COMPLETE);
    assert(finish_calls == 1U && state == NP2_OPNGEN_E1B_COMPLETE && error == 0);
    assert(run_worker_finish_case(1, 1, 0, &finish_calls, &state, &error) ==
           NP2_OPNGEN_E1B_STEP_FAILED);
    assert(finish_calls == 1U && state == NP2_OPNGEN_E1B_FAILED &&
           error == NP2_OPNGEN_E1B_ERROR_OUTPUT_SINK);
    assert(run_worker_finish_case(1, 0, 1, &finish_calls, &state, &error) ==
           NP2_OPNGEN_E1B_STEP_FAILED);
    assert(finish_calls == 0U && state == NP2_OPNGEN_E1B_FAILED &&
           error == NP2_OPNGEN_E1B_ERROR_OUTPUT_SINK);
}

int main(void)
{
    printf("PCM_RING_META capacity=%u quantum_frames=%u slot_bytes=%u sizeof_slot=%zu sizeof_ring=%zu head_tail=uint32_atomic\n",
           NP2_OPNGEN_PCM_RING_CAPACITY, NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES,
           NP2_OPNGEN_PCM_RING_SLOT_BYTES,
           sizeof(struct np2opngen_pcm_ring_slot),
           sizeof(struct np2opngen_pcm_ring));
    test_basic_and_capacity();
    test_partitions();
    test_cross_slot_and_retry();
    test_finish_and_errors();
    test_threaded_and_wrap();
    test_worker_finish_seam();
    puts("PCM_RING_RESULT=PASS");
    return 0;
}
