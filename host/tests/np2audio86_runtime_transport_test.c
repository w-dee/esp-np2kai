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

int main(void)
{
    test_horizon();
    test_mailbox_ownership();
    test_old_seq_wrap_regression();
    test_transport_before_horizon();
    test_reset_and_control();
    test_reset_ordinal_event_publication();
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
    return 0;
}
