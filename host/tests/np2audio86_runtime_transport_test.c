#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "np2audio86_fixture.h"
#include "np2audio86_runtime_transport.h"
#include "np2opngen_pcm_ring.h"

#define TERMINAL_RESET_FRAME UINT64_C(95761)
#define TERMINAL_FRAME UINT64_C(96000)
#define TERMINAL_RESET_ORDINAL 1U

struct reset_ordinal_probe {
    struct np2audio86_event_ring ring;
    struct np2audio86_runtime_control control;
    uint32_t producer_ordinal;
    uint32_t producer_expected;
    uint32_t worker_snapshot;
    uint32_t worker_ack;
    int producer_status;
    _Atomic bool hook_entered;
    _Atomic bool release_producer;
    _Atomic bool worker_observed;
    _Atomic bool producer_returned;
};

static struct reset_ordinal_probe *g_reset_ordinal_probe;

void np2audio86_reset_ordinal_after_publish_test_hook(uint32_t ordinal)
{
    struct reset_ordinal_probe *probe = g_reset_ordinal_probe;
    if (probe == NULL || ordinal != 1U) return;
    atomic_store_explicit(&probe->hook_entered, true, memory_order_release);
    while (!atomic_load_explicit(&probe->release_producer,
                                 memory_order_acquire)) {
        sched_yield();
    }
}

static void wait_true(const _Atomic bool *value)
{
    while (!atomic_load_explicit(value, memory_order_acquire)) sched_yield();
}

static void *reset_ordinal_producer(void *opaque)
{
    struct reset_ordinal_probe *probe = opaque;
    const struct np2audio86_event event = {
        95761U, 18U, NP2_AUDIO86_EVENT_RESET_BARRIER, 0U};
    probe->producer_status = np2audio86_reset_event_ring_enqueue(
        &probe->ring, &event, &probe->producer_ordinal);
    probe->producer_expected = probe->producer_ordinal;
    atomic_store_explicit(&probe->producer_returned, true,
                          memory_order_release);
    return NULL;
}

static void *reset_ordinal_worker(void *opaque)
{
    struct reset_ordinal_probe *probe = opaque;
    const struct np2audio86_event *event = NULL;
    while (np2audio86_event_ring_peek(&probe->ring, &event) ==
           NP2_AUDIO86_TRANSPORT_EMPTY) {
        sched_yield();
    }
    assert(event != NULL);
    probe->worker_snapshot = event->payload;
    np2audio86_runtime_reset_ack_publish(&probe->control, event->payload);
    probe->worker_ack = np2audio86_runtime_reset_ack(&probe->control);
    assert(np2audio86_event_ring_consume(&probe->ring) ==
           NP2_AUDIO86_TRANSPORT_OK);
    atomic_store_explicit(&probe->worker_observed, true,
                          memory_order_release);
    return NULL;
}

static void observe_ok(struct np2audio86_runtime_control *control,
                       struct np2audio86_runtime_consumer_clock *consumer,
                       uint64_t expected)
{
    assert(np2audio86_runtime_horizon_observe(control, consumer) ==
           NP2_AUDIO86_RUNTIME_HORIZON_OK);
    assert(consumer->committed_frame_reconstructed == expected);
}

static void test_horizon(void)
{
    struct np2audio86_runtime_control control;
    struct np2audio86_runtime_producer_clock producer = {0U};
    struct np2audio86_runtime_consumer_clock consumer = {0U};
    np2audio86_runtime_control_init(&control);

    assert(np2audio86_runtime_horizon_try_observe(&control, &consumer) ==
           NP2_AUDIO86_RUNTIME_HORIZON_RETRY);
    assert(np2audio86_runtime_horizon_publish(&control, &producer, 16U) ==
           NP2_AUDIO86_RUNTIME_HORIZON_OK);
    assert(np2audio86_runtime_horizon_pending(&control));
    assert(np2audio86_runtime_horizon_publish(&control, &producer, 16U) ==
           NP2_AUDIO86_RUNTIME_HORIZON_RETRY);
    observe_ok(&control, &consumer, 16U);
    assert(!np2audio86_runtime_horizon_pending(&control));

    assert(np2audio86_runtime_horizon_publish(
               &control, &producer, UINT64_C(0xffffffff)) ==
           NP2_AUDIO86_RUNTIME_HORIZON_OK);
    observe_ok(&control, &consumer, UINT64_C(0xffffffff));
    assert(np2audio86_runtime_horizon_publish(
               &control, &producer, UINT64_C(0x100000000)) ==
           NP2_AUDIO86_RUNTIME_HORIZON_OK);
    observe_ok(&control, &consumer, UINT64_C(0x100000000));

    assert(np2audio86_runtime_horizon_publish(
               &control, &producer, UINT64_C(0x300000020)) ==
           NP2_AUDIO86_RUNTIME_HORIZON_OK);
    observe_ok(&control, &consumer, UINT64_C(0x300000020));
    assert(np2audio86_runtime_horizon_publish(
               &control, &producer, UINT64_C(0x800000030)) ==
           NP2_AUDIO86_RUNTIME_HORIZON_OK);
    observe_ok(&control, &consumer, UINT64_C(0x800000030));
    assert(np2audio86_runtime_horizon_publish(
               &control, &producer, UINT64_MAX - 1U) ==
           NP2_AUDIO86_RUNTIME_HORIZON_OK);
    observe_ok(&control, &consumer, UINT64_MAX - 1U);
    assert(np2audio86_runtime_horizon_publish(
               &control, &producer, UINT64_MAX) ==
           NP2_AUDIO86_RUNTIME_HORIZON_OK);
    observe_ok(&control, &consumer, UINT64_MAX);
    assert(np2audio86_runtime_horizon_publish(
               &control, &producer, UINT64_MAX - 2U) ==
           NP2_AUDIO86_RUNTIME_HORIZON_NONMONOTONIC);
}

static void test_mailbox_ownership(void)
{
    struct np2audio86_runtime_control control;
    struct np2audio86_runtime_producer_clock producer = {0U};
    struct np2audio86_runtime_consumer_clock consumer = {0U};
    np2audio86_runtime_control_init(&control);
    assert(np2audio86_runtime_horizon_publish(
               &control, &producer, UINT64_C(0x1122334455667788)) ==
           NP2_AUDIO86_RUNTIME_HORIZON_OK);
    assert(control.horizon.horizon_frame_lo == UINT32_C(0x55667788));
    assert(control.horizon.horizon_frame_hi == UINT32_C(0x11223344));
    assert(np2audio86_runtime_horizon_publish(
               &control, &producer, UINT64_C(0x99aabbccddeeff00)) ==
           NP2_AUDIO86_RUNTIME_HORIZON_RETRY);
    observe_ok(&control, &consumer, UINT64_C(0x1122334455667788));
    assert(np2audio86_runtime_horizon_try_observe(&control, &consumer) ==
           NP2_AUDIO86_RUNTIME_HORIZON_RETRY);

    assert(np2audio86_runtime_horizon_publish(
               &control, &producer, UINT64_C(0x99aabbccddeeff00)) ==
           NP2_AUDIO86_RUNTIME_HORIZON_OK);
    observe_ok(&control, &consumer, UINT64_C(0x99aabbccddeeff00));
}

static void test_old_seq_wrap_regression(void)
{
    const uint32_t initial_even = 2U;
    const uint32_t after_half_range_publications =
        initial_even + UINT32_C(0x80000000) * 2U;
    assert(after_half_range_publications == initial_even);
}

static void test_transport_before_horizon(void)
{
    struct np2audio86_runtime_control control;
    struct np2audio86_runtime_producer_clock producer = {0U};
    struct np2audio86_runtime_consumer_clock consumer = {0U};
    struct np2audio86_event_ring ring;
    struct np2audio86_event event = {16U, 0U, NP2_AUDIO86_EVENT_FM_KEY, 1U};
    const struct np2audio86_event *observed = NULL;
    np2audio86_runtime_control_init(&control);
    np2audio86_event_ring_init(&ring);

    assert(np2audio86_event_ring_enqueue(&ring, &event) ==
           NP2_AUDIO86_TRANSPORT_OK);
    assert(np2audio86_runtime_horizon_try_observe(&control, &consumer) ==
           NP2_AUDIO86_RUNTIME_HORIZON_RETRY);
    assert(event.frame_timestamp > consumer.committed_frame_reconstructed);
    assert(np2audio86_runtime_horizon_publish(&control, &producer, 16U) ==
           NP2_AUDIO86_RUNTIME_HORIZON_OK);
    observe_ok(&control, &consumer, 16U);
    assert(np2audio86_event_ring_peek(&ring, &observed) ==
           NP2_AUDIO86_TRANSPORT_OK);
    assert(observed != NULL && observed->sequence == 0U &&
           observed->frame_timestamp <=
               consumer.committed_frame_reconstructed);
}

static void test_reset_and_control(void)
{
    struct np2audio86_runtime_control control;
    np2audio86_runtime_control_init(&control);

    np2audio86_runtime_reset_ack_publish(&control, 4U);
    assert(np2audio86_runtime_reset_ack(&control) == 4U);
    assert(np2audio86_runtime_reset_ack(&control) != 5U); /* stale ACK */
    np2audio86_runtime_reset_ack_publish(&control, UINT32_MAX);
    assert(np2audio86_runtime_reset_ack(&control) == UINT32_MAX);
    np2audio86_runtime_reset_ack_publish(&control, 0U);
    assert(np2audio86_runtime_reset_ack(&control) == 0U); /* wrap */

    assert(np2audio86_runtime_first_error_publish(&control, 7U));
    assert(!np2audio86_runtime_first_error_publish(&control, 9U));
    assert(np2audio86_runtime_first_error(&control) == 7U);
    np2audio86_runtime_stop_publish(&control);
    np2audio86_runtime_producer_done_publish(&control);
    assert(np2audio86_runtime_stop_requested(&control));
    assert(np2audio86_runtime_producer_done(&control));
}

static void test_reset_ordinal_event_publication(void)
{
    struct reset_ordinal_probe probe;
    const struct np2audio86_event second = {
        96000U, 19U, NP2_AUDIO86_EVENT_RESET_BARRIER, 0U};
    const struct np2audio86_event *observed = NULL;
    pthread_t producer;
    pthread_t worker;
    memset(&probe, 0, sizeof(probe));
    np2audio86_event_ring_init(&probe.ring);
    np2audio86_runtime_control_init(&probe.control);
    atomic_init(&probe.hook_entered, false);
    atomic_init(&probe.release_producer, false);
    atomic_init(&probe.worker_observed, false);
    atomic_init(&probe.producer_returned, false);
    g_reset_ordinal_probe = &probe;
    assert(pthread_create(&worker, NULL, reset_ordinal_worker, &probe) == 0);
    assert(pthread_create(&producer, NULL, reset_ordinal_producer, &probe) == 0);
    wait_true(&probe.hook_entered);
    wait_true(&probe.worker_observed);
    assert(!atomic_load_explicit(&probe.producer_returned,
                                 memory_order_acquire));
    assert(probe.worker_snapshot == 1U && probe.worker_ack == 1U);
    atomic_store_explicit(&probe.release_producer, true, memory_order_release);
    assert(pthread_join(producer, NULL) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(probe.producer_status == NP2_AUDIO86_TRANSPORT_OK);
    assert(probe.producer_expected == 1U && probe.producer_ordinal == 1U);
    assert(np2audio86_runtime_reset_ack(&probe.control) == 1U);

    assert(np2audio86_reset_event_ring_enqueue(
               &probe.ring, &second, &probe.producer_ordinal) ==
           NP2_AUDIO86_TRANSPORT_OK);
    assert(np2audio86_event_ring_peek(&probe.ring, &observed) ==
           NP2_AUDIO86_TRANSPORT_OK);
    assert(observed != NULL && observed->payload == 2U);
    np2audio86_runtime_reset_ack_publish(&probe.control, observed->payload);
    assert(np2audio86_event_ring_consume(&probe.ring) ==
           NP2_AUDIO86_TRANSPORT_OK);
    assert(probe.producer_ordinal == 2U &&
           np2audio86_runtime_reset_ack(&probe.control) == 2U);
    g_reset_ordinal_probe = NULL;
}

struct terminal_probe {
    struct np2audio86_event_ring events;
    struct np2audio86_runtime_control control;
    struct np2audio86_runtime_producer_clock producer_clock;
    struct np2audio86_runtime_consumer_clock consumer_clock;
    struct np2opngen_pcm_ring pcm_ring;
    bool inject_post_pcm_failure;
    _Atomic bool reset_ack;
    _Atomic bool q399_visible;
    _Atomic bool pcm_done;
    _Atomic bool release_continuation;
    _Atomic bool producer_continued;
    _Atomic bool guest_done;
    _Atomic bool lifecycle_failed;
};

static void prepare_q399_partial(struct terminal_probe *probe)
{
    uint8_t pcm[NP2_OPNGEN_PCM_RING_SLOT_BYTES] = {0U};
    size_t consumed;
    const struct np2opngen_pcm_ring_slot *slot = NULL;
    uint32_t sequence;
    np2opngen_pcm_ring_init(&probe->pcm_ring);
    for (sequence = 0U; sequence < 399U; ++sequence) {
        assert(np2opngen_pcm_ring_append(
                   &probe->pcm_ring, pcm,
                   NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES,
                   (uint64_t)sequence * NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES,
                   &consumed) == NP2_OPNGEN_PCM_RING_OK);
        assert(consumed == NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES);
        assert(np2opngen_pcm_ring_try_peek(&probe->pcm_ring, &slot) ==
               NP2_OPNGEN_PCM_RING_OK);
        assert(slot != NULL && slot->sequence == sequence &&
               slot->frame_offset ==
                   (uint64_t)sequence * NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES);
        assert(np2opngen_pcm_ring_consume(&probe->pcm_ring) ==
               NP2_OPNGEN_PCM_RING_OK);
    }
    assert(probe->pcm_ring.next_frame_offset == UINT64_C(95760));
    assert(np2opngen_pcm_ring_append(&probe->pcm_ring, pcm, 1U,
                                     UINT64_C(95760), &consumed) ==
           NP2_OPNGEN_PCM_RING_OK);
    assert(consumed == 1U);
    assert(np2opngen_pcm_ring_occupancy(&probe->pcm_ring) == 0U);
    assert(np2opngen_pcm_ring_producer_partial_valid_frames(
               &probe->pcm_ring) == 1U);
}

static void terminal_probe_init(struct terminal_probe *probe,
                                bool inject_post_pcm_failure)
{
    memset(probe, 0, sizeof(*probe));
    np2audio86_event_ring_init(&probe->events);
    np2audio86_runtime_control_init(&probe->control);
    prepare_q399_partial(probe);
    probe->inject_post_pcm_failure = inject_post_pcm_failure;
    atomic_init(&probe->reset_ack, false);
    atomic_init(&probe->q399_visible, false);
    atomic_init(&probe->pcm_done, false);
    atomic_init(&probe->release_continuation, false);
    atomic_init(&probe->producer_continued, false);
    atomic_init(&probe->guest_done, false);
    atomic_init(&probe->lifecycle_failed, false);
}

static void *terminal_producer(void *opaque)
{
    struct terminal_probe *probe = opaque;
    const struct np2audio86_event reset = {
        TERMINAL_RESET_FRAME, 18U, NP2_AUDIO86_EVENT_RESET_BARRIER, 0U};
    uint32_t ordinal = 0U;
    assert(np2audio86_reset_event_ring_enqueue(
               &probe->events, &reset, &ordinal) ==
           NP2_AUDIO86_TRANSPORT_OK);
    assert(ordinal == TERMINAL_RESET_ORDINAL);
    assert(np2audio86_runtime_terminal_horizon_publish(
               &probe->control, &probe->producer_clock, TERMINAL_FRAME,
               TERMINAL_FRAME, ordinal) == NP2_AUDIO86_RUNTIME_HORIZON_OK);
    while (np2audio86_runtime_reset_ack(&probe->control) < ordinal)
        sched_yield();
    atomic_store_explicit(&probe->reset_ack, true, memory_order_release);
    wait_true(&probe->release_continuation);
    atomic_store_explicit(&probe->producer_continued, true,
                          memory_order_release);
    if (probe->inject_post_pcm_failure)
        atomic_store_explicit(&probe->lifecycle_failed, true,
                              memory_order_release);
    atomic_store_explicit(&probe->guest_done, true, memory_order_release);
    return NULL;
}

static void *terminal_worker_detail(void *opaque)
{
    struct terminal_probe *probe = opaque;
    struct np2audio86_runtime_horizon_observation observation;
    const struct np2audio86_event *event = NULL;
    const struct np2opngen_pcm_ring_slot *slot = NULL;
    uint8_t pcm[239U * NP2_OPNGEN_PCM_RING_BYTES_PER_FRAME] = {0U};
    size_t consumed = 0U;
    int status;
    do {
        status = np2audio86_runtime_horizon_try_observe_detail(
            &probe->control, &probe->consumer_clock, &observation);
        if (status == NP2_AUDIO86_RUNTIME_HORIZON_RETRY) sched_yield();
    } while (status == NP2_AUDIO86_RUNTIME_HORIZON_RETRY);
    assert(status == NP2_AUDIO86_RUNTIME_HORIZON_OK);
    assert(observation.frame == TERMINAL_FRAME);
    assert(observation.flags == NP2_AUDIO86_RUNTIME_HORIZON_FLAG_TERMINAL);
    assert(observation.terminal_reset_ordinal == TERMINAL_RESET_ORDINAL);

    /* Acquiring the terminal mailbox must make the earlier RESET publication
     * visible.  Rendering beyond RESET is not authorized until it is applied. */
    assert(np2audio86_event_ring_peek(&probe->events, &event) ==
           NP2_AUDIO86_TRANSPORT_OK);
    assert(event != NULL && event->frame_timestamp == TERMINAL_RESET_FRAME &&
           event->payload == observation.terminal_reset_ordinal);
    np2audio86_runtime_reset_ack_publish(&probe->control, event->payload);
    assert(np2audio86_event_ring_consume(&probe->events) ==
           NP2_AUDIO86_TRANSPORT_OK);

    assert(np2opngen_pcm_ring_append(
               &probe->pcm_ring, pcm, 239U, TERMINAL_RESET_FRAME,
               &consumed) == NP2_OPNGEN_PCM_RING_OK);
    assert(consumed == 239U);
    assert(np2opngen_pcm_ring_try_peek(&probe->pcm_ring, &slot) ==
           NP2_OPNGEN_PCM_RING_OK);
    assert(slot != NULL && slot->sequence == 399U &&
           slot->frame_offset == UINT64_C(95760) &&
           slot->valid_frames == NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES);
    atomic_store_explicit(&probe->q399_visible, true, memory_order_release);
    assert(np2opngen_pcm_ring_finish(&probe->pcm_ring, TERMINAL_FRAME) ==
           NP2_OPNGEN_PCM_RING_OK);
    atomic_store_explicit(&probe->pcm_done, true, memory_order_release);
    return NULL;
}

static void run_delayed_continuation_probe(bool inject_post_pcm_failure)
{
    struct terminal_probe probe;
    pthread_t producer;
    pthread_t worker;
    terminal_probe_init(&probe, inject_post_pcm_failure);
    assert(pthread_create(&worker, NULL, terminal_worker_detail, &probe) == 0);
    assert(pthread_create(&producer, NULL, terminal_producer, &probe) == 0);
    wait_true(&probe.reset_ack);
    wait_true(&probe.q399_visible);
    wait_true(&probe.pcm_done);
    assert(!atomic_load_explicit(&probe.producer_continued,
                                 memory_order_acquire));
    assert(!atomic_load_explicit(&probe.guest_done, memory_order_acquire));
    atomic_store_explicit(&probe.release_continuation, true,
                          memory_order_release);
    assert(pthread_join(producer, NULL) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(atomic_load_explicit(&probe.guest_done, memory_order_acquire));
    assert(atomic_load_explicit(&probe.lifecycle_failed,
                                memory_order_acquire) ==
           inject_post_pcm_failure);
    assert((atomic_load_explicit(&probe.pcm_done, memory_order_acquire) &&
            atomic_load_explicit(&probe.guest_done, memory_order_acquire) &&
            !atomic_load_explicit(&probe.lifecycle_failed,
                                  memory_order_acquire)) ==
           !inject_post_pcm_failure);
}

static void test_terminal_change_sensitivity(void)
{
    struct np2audio86_runtime_control control;
    struct np2audio86_runtime_producer_clock producer = {0U, 0U};
    struct np2audio86_runtime_consumer_clock consumer = {0U};
    struct np2audio86_runtime_horizon_observation observation;
    np2audio86_runtime_control_init(&control);
    assert(np2audio86_runtime_terminal_horizon_publish(
               &control, &producer, TERMINAL_FRAME, TERMINAL_FRAME, 0U) ==
           NP2_AUDIO86_RUNTIME_HORIZON_RESET_REQUIRED);
    assert(np2audio86_runtime_terminal_horizon_publish(
               &control, &producer, TERMINAL_FRAME + 1U, TERMINAL_FRAME,
               TERMINAL_RESET_ORDINAL) == NP2_AUDIO86_RUNTIME_HORIZON_BOUNDS);
    assert(np2audio86_runtime_horizon_publish(
               &control, &producer, TERMINAL_RESET_FRAME) ==
           NP2_AUDIO86_RUNTIME_HORIZON_OK);
    observe_ok(&control, &consumer, TERMINAL_RESET_FRAME);
    assert(np2audio86_runtime_terminal_horizon_publish(
               &control, &producer, TERMINAL_RESET_FRAME - 1U,
               TERMINAL_FRAME, TERMINAL_RESET_ORDINAL) ==
           NP2_AUDIO86_RUNTIME_HORIZON_NONMONOTONIC);
    assert(np2audio86_runtime_terminal_horizon_publish(
               &control, &producer, TERMINAL_FRAME, TERMINAL_FRAME,
               TERMINAL_RESET_ORDINAL) == NP2_AUDIO86_RUNTIME_HORIZON_OK);
    assert(np2audio86_runtime_horizon_try_observe_detail(
               &control, &consumer, &observation) ==
           NP2_AUDIO86_RUNTIME_HORIZON_OK);
    assert(observation.flags == NP2_AUDIO86_RUNTIME_HORIZON_FLAG_TERMINAL &&
           observation.terminal_reset_ordinal == TERMINAL_RESET_ORDINAL);
    assert(!np2audio86_runtime_semantic_event_permitted(&producer));
    assert(np2audio86_runtime_terminal_horizon_publish(
               &control, &producer, TERMINAL_FRAME - 1U, TERMINAL_FRAME,
               TERMINAL_RESET_ORDINAL) ==
           NP2_AUDIO86_RUNTIME_HORIZON_TERMINATED);
    assert(np2audio86_runtime_horizon_publish(
               &control, &producer, TERMINAL_FRAME) ==
           NP2_AUDIO86_RUNTIME_HORIZON_TERMINATED);
}

static void test_terminal_missing_event_rejected(void)
{
    struct np2audio86_runtime_control control;
    struct np2audio86_runtime_producer_clock producer = {0U, 0U};
    struct np2audio86_runtime_consumer_clock consumer = {0U};
    struct np2audio86_runtime_horizon_observation observation;
    struct np2audio86_event_ring events;
    const struct np2audio86_event *event = NULL;
    np2audio86_runtime_control_init(&control);
    np2audio86_event_ring_init(&events);
    assert(np2audio86_runtime_terminal_horizon_publish(
               &control, &producer, TERMINAL_FRAME, TERMINAL_FRAME,
               TERMINAL_RESET_ORDINAL) == NP2_AUDIO86_RUNTIME_HORIZON_OK);
    assert(np2audio86_runtime_horizon_try_observe_detail(
               &control, &consumer, &observation) ==
           NP2_AUDIO86_RUNTIME_HORIZON_OK);
    assert(np2audio86_event_ring_peek(&events, &event) ==
           NP2_AUDIO86_TRANSPORT_EMPTY);
    assert(event == NULL);
    /* The worker's corresponding-reset predicate remains unsatisfied. */
    assert(np2audio86_runtime_reset_ack(&control) !=
           observation.terminal_reset_ordinal);
}

int main(void)
{
    test_horizon();
    test_mailbox_ownership();
    test_old_seq_wrap_regression();
    test_transport_before_horizon();
    test_reset_and_control();
    test_reset_ordinal_event_publication();
    test_terminal_change_sensitivity();
    test_terminal_missing_event_rejected();
    run_delayed_continuation_probe(false);
    run_delayed_continuation_probe(true);
    printf("AUDIO86_RUNTIME_TRANSPORT abi=%zu event_ring=%zu byte_ring=%zu mailbox=%zu mailbox_align=%zu control=%zu control_align=%zu producer_clock=%zu consumer_clock=%zu\n",
           sizeof(struct np2audio86_event),
           sizeof(struct np2audio86_event_ring),
           sizeof(struct np2audio86_byte_ring),
           sizeof(struct np2audio86_runtime_horizon_mailbox),
           _Alignof(struct np2audio86_runtime_horizon_mailbox),
           sizeof(struct np2audio86_runtime_control),
           _Alignof(struct np2audio86_runtime_control),
           sizeof(struct np2audio86_runtime_producer_clock),
           sizeof(struct np2audio86_runtime_consumer_clock));
    printf("AUDIO86_RUNTIME_HORIZON_TESTS=PASS\n"
           "OLD_HORIZON_SEQ_WRAP_ABA_REPRODUCED=PASS\n"
           "HORIZON_VERSION_PROTOCOL_REMOVED=PASS\n"
           "HORIZON_MAILBOX_C11_PROOF=PASS\n"
           "HORIZON_INDEFINITE_PUBLICATION=PASS\n"
           "HORIZON_FULL_WAIT_RETRY=PASS\n"
           "COMMITTED_HORIZON_AFTER_TRANSPORT_PUBLICATION=PASS\n"
           "RESET_ACK_U32_PROTOCOL=PASS\n"
           "RESET_ORDINAL_FROZEN_BEFORE_WORKER_VISIBILITY=PASS\n"
           "RESET_ORDINAL_ADVERSARIAL_INTERLEAVING_TEST=PASS\n"
           "RESET_ORDINAL_MONOTONIC_EVENT_IDENTITY=PASS\n"
           "P4_FIRST_ERROR_IMMUTABLE=PASS\n");
    printf("TERMINAL_EVENT_BEFORE_TERMINAL_HORIZON=PASS\n"
           "DELAYED_RESET_ACK_Q399_INTEGRATION=PASS\n"
           "Q399_AVAILABLE_BEFORE_PRODUCER_POST_ACK_CONTINUATION=PASS\n"
           "PCM_DONE_BEFORE_GUEST_DONE_TEST=PASS\n"
           "RESET_EVENT_VISIBILITY_PRECEDES_POST_RESET_RENDER=PASS\n"
           "TERMINAL_HORIZON_MISSING_EVENT_REJECTED=PASS\n"
           "TERMINAL_HORIZON_CHANGE_SENSITIVITY=PASS\n"
           "NONTERMINAL_HORIZON_NONREGRESSION=PASS\n"
           "POST_PCM_PRODUCER_FAILURE_REMAINS_FATAL=PASS\n");
    return 0;
}
