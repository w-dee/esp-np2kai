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
    atomic_init(&control->committed_seq, 0U);
    atomic_init(&control->committed_frame_lo, 0U);
    atomic_init(&control->committed_frame_hi, 0U);
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
    uint32_t sequence;
    if (control == NULL || producer == NULL) {
        return NP2_AUDIO86_RUNTIME_HORIZON_ARGUMENT;
    }
    if (frame < producer->committed_frame_owner) {
        return NP2_AUDIO86_RUNTIME_HORIZON_NONMONOTONIC;
    }
    sequence = atomic_load_explicit(&control->committed_seq,
                                    memory_order_seq_cst);
    if ((sequence & 1U) != 0U) {
        return NP2_AUDIO86_RUNTIME_HORIZON_RETRY;
    }
    /* Every snapshot access is seq_cst so the reader's two sequence loads
     * bracket both half loads in the single C atomic order.  An accepted
     * equal/even version therefore cannot contain halves from different
     * publications. */
    atomic_store_explicit(&control->committed_seq, sequence + 1U,
                          memory_order_seq_cst);
    atomic_store_explicit(&control->committed_frame_lo, (uint32_t)frame,
                          memory_order_seq_cst);
    atomic_store_explicit(&control->committed_frame_hi,
                          (uint32_t)(frame >> 32U), memory_order_seq_cst);
    producer->committed_frame_owner = frame;
    atomic_store_explicit(&control->committed_seq, sequence + 2U,
                          memory_order_seq_cst);
    return NP2_AUDIO86_RUNTIME_HORIZON_OK;
}

int np2audio86_runtime_horizon_try_observe(
    const struct np2audio86_runtime_control *control,
    struct np2audio86_runtime_consumer_clock *consumer)
{
    uint32_t sequence_before;
    uint32_t sequence_after;
    uint32_t low;
    uint32_t high;
    uint64_t frame;
    if (control == NULL || consumer == NULL) {
        return NP2_AUDIO86_RUNTIME_HORIZON_ARGUMENT;
    }
    sequence_before = atomic_load_explicit(&control->committed_seq,
                                           memory_order_seq_cst);
    if ((sequence_before & 1U) != 0U) {
        return NP2_AUDIO86_RUNTIME_HORIZON_RETRY;
    }
    low = atomic_load_explicit(&control->committed_frame_lo,
                               memory_order_seq_cst);
    high = atomic_load_explicit(&control->committed_frame_hi,
                                memory_order_seq_cst);
    sequence_after = atomic_load_explicit(&control->committed_seq,
                                          memory_order_seq_cst);
    if (sequence_before != sequence_after || (sequence_after & 1U) != 0U) {
        return NP2_AUDIO86_RUNTIME_HORIZON_RETRY;
    }
    frame = ((uint64_t)high << 32U) | low;
    if (frame < consumer->committed_frame_reconstructed) {
        return NP2_AUDIO86_RUNTIME_HORIZON_NONMONOTONIC;
    }
    consumer->committed_frame_reconstructed = frame;
    return NP2_AUDIO86_RUNTIME_HORIZON_OK;
}

int np2audio86_runtime_horizon_observe(
    const struct np2audio86_runtime_control *control,
    struct np2audio86_runtime_consumer_clock *consumer)
{
    int status;
    do {
        status = np2audio86_runtime_horizon_try_observe(control, consumer);
    } while (status == NP2_AUDIO86_RUNTIME_HORIZON_RETRY);
    return status;
}
