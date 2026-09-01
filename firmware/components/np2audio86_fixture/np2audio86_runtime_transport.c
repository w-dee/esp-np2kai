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
    atomic_init(&control->committed_frame_low32, 0U);
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
    uint64_t delta;
    if (control == NULL || producer == NULL) {
        return NP2_AUDIO86_RUNTIME_HORIZON_ARGUMENT;
    }
    if (frame < producer->committed_frame_owner) {
        return NP2_AUDIO86_RUNTIME_HORIZON_NONMONOTONIC;
    }
    delta = frame - producer->committed_frame_owner;
    if (delta >= UINT64_C(0x80000000)) {
        return NP2_AUDIO86_RUNTIME_HORIZON_AMBIGUOUS;
    }
    producer->committed_frame_owner = frame;
    atomic_store_explicit(&control->committed_frame_low32, (uint32_t)frame,
                          memory_order_release);
    return NP2_AUDIO86_RUNTIME_HORIZON_OK;
}

int np2audio86_runtime_horizon_observe(
    const struct np2audio86_runtime_control *control,
    struct np2audio86_runtime_consumer_clock *consumer)
{
    uint32_t low;
    uint32_t delta;
    if (control == NULL || consumer == NULL) {
        return NP2_AUDIO86_RUNTIME_HORIZON_ARGUMENT;
    }
    low = atomic_load_explicit(&control->committed_frame_low32,
                               memory_order_acquire);
    delta = (uint32_t)(low - consumer->previous_low32);
    if (delta >= UINT32_C(0x80000000)) {
        return NP2_AUDIO86_RUNTIME_HORIZON_AMBIGUOUS;
    }
    if (UINT64_MAX - consumer->committed_frame_reconstructed < delta) {
        return NP2_AUDIO86_RUNTIME_HORIZON_OVERFLOW;
    }
    consumer->committed_frame_reconstructed += delta;
    consumer->previous_low32 = low;
    return NP2_AUDIO86_RUNTIME_HORIZON_OK;
}
