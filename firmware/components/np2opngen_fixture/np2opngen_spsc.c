#include "np2opngen_spsc.h"

#include <stdatomic.h>

void np2opngen_spsc_init(struct np2opngen_spsc_queue *queue)
{
    if (queue == 0) {
        return;
    }
    atomic_init(&queue->head, 0U);
    atomic_init(&queue->tail, 0U);
}

int np2opngen_spsc_enqueue(struct np2opngen_spsc_queue *queue,
                           const struct np2opngen_synth_event *event)
{
    uint32_t head;
    uint32_t tail;
    if (queue == 0 || event == 0) {
        return NP2_OPNGEN_SPSC_ARGUMENT;
    }
    head = atomic_load_explicit(&queue->head, memory_order_relaxed);
    tail = atomic_load_explicit(&queue->tail, memory_order_acquire);
    if (head - tail == NP2_OPNGEN_SPSC_CAPACITY) {
        return NP2_OPNGEN_SPSC_FULL;
    }
    queue->slots[head & (NP2_OPNGEN_SPSC_CAPACITY - 1U)] = *event;
    atomic_store_explicit(&queue->head, head + 1U, memory_order_release);
    return NP2_OPNGEN_SPSC_OK;
}

int np2opngen_spsc_dequeue(struct np2opngen_spsc_queue *queue,
                           struct np2opngen_synth_event *event)
{
    uint32_t head;
    uint32_t tail;
    if (queue == 0 || event == 0) {
        return NP2_OPNGEN_SPSC_ARGUMENT;
    }
    tail = atomic_load_explicit(&queue->tail, memory_order_relaxed);
    head = atomic_load_explicit(&queue->head, memory_order_acquire);
    if (head == tail) {
        return NP2_OPNGEN_SPSC_EMPTY;
    }
    *event = queue->slots[tail & (NP2_OPNGEN_SPSC_CAPACITY - 1U)];
    atomic_store_explicit(&queue->tail, tail + 1U, memory_order_release);
    return NP2_OPNGEN_SPSC_OK;
}

uint32_t np2opngen_spsc_occupancy(const struct np2opngen_spsc_queue *queue)
{
    uint32_t head;
    uint32_t tail;
    if (queue == 0) {
        return 0U;
    }
    head = atomic_load_explicit(&queue->head, memory_order_acquire);
    tail = atomic_load_explicit(&queue->tail, memory_order_acquire);
    return head - tail;
}
