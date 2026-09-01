#include "np2audio86_runtime_transport.h"

#include <limits.h>

void np2audio86_runtime_control_init(
    struct np2audio86_runtime_control *control)
{
    if (control == NULL) {
        return;
    }
    atomic_init(&control->first_error, 0U);
    atomic_init(&control->stop, 0U);
    atomic_init(&control->producer_done, 0U);
    atomic_init(&control->reset_ack_ordinal, 0U);
    atomic_init(&control->horizon.horizon_state,
                NP2_AUDIO86_RUNTIME_HORIZON_EMPTY);
    control->horizon.horizon_frame_lo = 0U;
    control->horizon.horizon_frame_hi = 0U;
}

bool np2audio86_runtime_first_error_publish(
    struct np2audio86_runtime_control *control, uint32_t error)
{
    uint32_t expected = 0U;
    if (control == NULL || error == 0U) {
        return false;
    }
    return atomic_compare_exchange_strong_explicit(
        &control->first_error, &expected, error, memory_order_acq_rel,
        memory_order_acquire);
}

uint32_t np2audio86_runtime_first_error(
    const struct np2audio86_runtime_control *control)
{
    return control == NULL
               ? UINT32_MAX
               : atomic_load_explicit(&control->first_error,
                                      memory_order_acquire);
}

void np2audio86_runtime_stop_publish(
    struct np2audio86_runtime_control *control)
{
    if (control != NULL) {
        atomic_store_explicit(&control->stop, 1U, memory_order_release);
    }
}

bool np2audio86_runtime_stop_requested(
    const struct np2audio86_runtime_control *control)
{
    return control == NULL ||
           atomic_load_explicit(&control->stop, memory_order_acquire) != 0U;
}

void np2audio86_runtime_producer_done_publish(
    struct np2audio86_runtime_control *control)
{
    if (control != NULL) {
        atomic_store_explicit(&control->producer_done, 1U,
                              memory_order_release);
    }
}

bool np2audio86_runtime_producer_done(
    const struct np2audio86_runtime_control *control)
{
    return control != NULL &&
           atomic_load_explicit(&control->producer_done,
                                memory_order_acquire) != 0U;
}

void np2audio86_runtime_reset_ack_publish(
    struct np2audio86_runtime_control *control, uint32_t ordinal)
{
    if (control != NULL) {
        atomic_store_explicit(&control->reset_ack_ordinal, ordinal,
                              memory_order_release);
    }
}

uint32_t np2audio86_runtime_reset_ack(
    const struct np2audio86_runtime_control *control)
{
    return control == NULL
               ? 0U
               : atomic_load_explicit(&control->reset_ack_ordinal,
                                      memory_order_acquire);
}

int np2audio86_runtime_horizon_publish(
    struct np2audio86_runtime_control *control,
    struct np2audio86_runtime_producer_clock *producer, uint64_t frame)
{
    if (control == NULL || producer == NULL) {
        return NP2_AUDIO86_RUNTIME_HORIZON_ARGUMENT;
    }
    if (frame < producer->committed_frame_owner) {
        return NP2_AUDIO86_RUNTIME_HORIZON_NONMONOTONIC;
    }
    if (atomic_load_explicit(&control->horizon.horizon_state,
                             memory_order_acquire) !=
        NP2_AUDIO86_RUNTIME_HORIZON_EMPTY) {
        return NP2_AUDIO86_RUNTIME_HORIZON_RETRY;
    }
    /* EMPTY grants the sole producer exclusive payload ownership.  These
     * plain stores happen-before a consumer which acquires FULL. */
    control->horizon.horizon_frame_lo = (uint32_t)frame;
    control->horizon.horizon_frame_hi = (uint32_t)(frame >> 32U);
    producer->committed_frame_owner = frame;
    atomic_store_explicit(&control->horizon.horizon_state,
                          NP2_AUDIO86_RUNTIME_HORIZON_FULL,
                          memory_order_release);
    return NP2_AUDIO86_RUNTIME_HORIZON_OK;
}

int np2audio86_runtime_horizon_try_observe(
    struct np2audio86_runtime_control *control,
    struct np2audio86_runtime_consumer_clock *consumer)
{
    uint32_t low;
    uint32_t high;
    uint64_t frame;
    if (control == NULL || consumer == NULL) {
        return NP2_AUDIO86_RUNTIME_HORIZON_ARGUMENT;
    }
    if (atomic_load_explicit(&control->horizon.horizon_state,
                             memory_order_acquire) !=
        NP2_AUDIO86_RUNTIME_HORIZON_FULL) {
        return NP2_AUDIO86_RUNTIME_HORIZON_RETRY;
    }
    /* FULL grants the sole consumer exclusive payload ownership.  EMPTY is
     * not released until both plain loads have completed. */
    low = control->horizon.horizon_frame_lo;
    high = control->horizon.horizon_frame_hi;
    frame = ((uint64_t)high << 32U) | low;
    if (frame < consumer->committed_frame_reconstructed) {
        return NP2_AUDIO86_RUNTIME_HORIZON_NONMONOTONIC;
    }
    consumer->committed_frame_reconstructed = frame;
    atomic_store_explicit(&control->horizon.horizon_state,
                          NP2_AUDIO86_RUNTIME_HORIZON_EMPTY,
                          memory_order_release);
    return NP2_AUDIO86_RUNTIME_HORIZON_OK;
}

int np2audio86_runtime_horizon_observe(
    struct np2audio86_runtime_control *control,
    struct np2audio86_runtime_consumer_clock *consumer)
{
    int status;
    do {
        status = np2audio86_runtime_horizon_try_observe(control, consumer);
    } while (status == NP2_AUDIO86_RUNTIME_HORIZON_RETRY);
    return status;
}

bool np2audio86_runtime_horizon_pending(
    const struct np2audio86_runtime_control *control)
{
    return control != NULL &&
           atomic_load_explicit(&control->horizon.horizon_state,
                                memory_order_acquire) ==
               NP2_AUDIO86_RUNTIME_HORIZON_FULL;
}
