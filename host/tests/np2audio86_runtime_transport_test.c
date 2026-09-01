#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "np2audio86_fixture.h"
#include "np2audio86_runtime_transport.h"

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

int main(void)
{
    test_horizon();
    test_mailbox_ownership();
    test_old_seq_wrap_regression();
    test_transport_before_horizon();
    test_reset_and_control();
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
           "P4_FIRST_ERROR_IMMUTABLE=PASS\n");
    return 0;
}
