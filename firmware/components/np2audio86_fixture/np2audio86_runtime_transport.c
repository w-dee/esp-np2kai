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
    control->horizon.horizon_flags = NP2_AUDIO86_RUNTIME_HORIZON_FLAG_NONE;
    control->horizon.terminal_reset_ordinal = 0U;
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

static int horizon_publish(
    struct np2audio86_runtime_control *control,
    struct np2audio86_runtime_producer_clock *producer, uint64_t frame,
    uint32_t flags, uint32_t terminal_reset_ordinal)
{
    if (control == NULL || producer == NULL) {
        return NP2_AUDIO86_RUNTIME_HORIZON_ARGUMENT;
    }
    if (producer->terminal_published_owner != 0U) {
        return NP2_AUDIO86_RUNTIME_HORIZON_TERMINATED;
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
    control->horizon.horizon_flags = flags;
    control->horizon.terminal_reset_ordinal = terminal_reset_ordinal;
    producer->committed_frame_owner = frame;
    if (flags == NP2_AUDIO86_RUNTIME_HORIZON_FLAG_TERMINAL) {
        producer->terminal_published_owner = 1U;
    }
    atomic_store_explicit(&control->horizon.horizon_state,
                          NP2_AUDIO86_RUNTIME_HORIZON_FULL,
                          memory_order_release);
    return NP2_AUDIO86_RUNTIME_HORIZON_OK;
}

int np2audio86_runtime_horizon_publish(
    struct np2audio86_runtime_control *control,
    struct np2audio86_runtime_producer_clock *producer, uint64_t frame)
{
    return horizon_publish(control, producer, frame,
                           NP2_AUDIO86_RUNTIME_HORIZON_FLAG_NONE, 0U);
}

int np2audio86_runtime_terminal_horizon_publish(
    struct np2audio86_runtime_control *control,
    struct np2audio86_runtime_producer_clock *producer, uint64_t frame,
    uint64_t workload_bound, uint32_t reset_ordinal)
{
    if (control == NULL || producer == NULL) {
        return NP2_AUDIO86_RUNTIME_HORIZON_ARGUMENT;
    }
    if (reset_ordinal == 0U) {
        return NP2_AUDIO86_RUNTIME_HORIZON_RESET_REQUIRED;
    }
    if (frame > workload_bound) {
        return NP2_AUDIO86_RUNTIME_HORIZON_BOUNDS;
    }
    return horizon_publish(control, producer, frame,
                           NP2_AUDIO86_RUNTIME_HORIZON_FLAG_TERMINAL,
                           reset_ordinal);
}

bool np2audio86_runtime_semantic_event_permitted(
    const struct np2audio86_runtime_producer_clock *producer)
{
    return producer != NULL && producer->terminal_published_owner == 0U;
}

int np2audio86_runtime_horizon_try_observe_detail(
    struct np2audio86_runtime_control *control,
    struct np2audio86_runtime_consumer_clock *consumer,
    struct np2audio86_runtime_horizon_observation *observation)
{
    uint32_t low;
    uint32_t high;
    uint64_t frame;
    uint32_t flags;
    uint32_t reset_ordinal;
    if (control == NULL || consumer == NULL || observation == NULL) {
        return NP2_AUDIO86_RUNTIME_HORIZON_ARGUMENT;
    }
    if (atomic_load_explicit(&control->horizon.horizon_state,
                             memory_order_acquire) !=
        NP2_AUDIO86_RUNTIME_HORIZON_FULL) {
        return NP2_AUDIO86_RUNTIME_HORIZON_RETRY;
    }
    /* FULL grants the sole consumer exclusive payload ownership.  EMPTY is
     * not released until all plain payload loads have completed. */
    low = control->horizon.horizon_frame_lo;
    high = control->horizon.horizon_frame_hi;
    flags = control->horizon.horizon_flags;
    reset_ordinal = control->horizon.terminal_reset_ordinal;
    frame = ((uint64_t)high << 32U) | low;
    if (frame < consumer->committed_frame_reconstructed) {
        return NP2_AUDIO86_RUNTIME_HORIZON_NONMONOTONIC;
    }
    consumer->committed_frame_reconstructed = frame;
    observation->frame = frame;
    observation->flags = flags;
    observation->terminal_reset_ordinal = reset_ordinal;
    atomic_store_explicit(&control->horizon.horizon_state,
                          NP2_AUDIO86_RUNTIME_HORIZON_EMPTY,
                          memory_order_release);
    return NP2_AUDIO86_RUNTIME_HORIZON_OK;
}

int np2audio86_runtime_horizon_try_observe(
    struct np2audio86_runtime_control *control,
    struct np2audio86_runtime_consumer_clock *consumer)
{
    struct np2audio86_runtime_horizon_observation observation;
    return np2audio86_runtime_horizon_try_observe_detail(
        control, consumer, &observation);
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
