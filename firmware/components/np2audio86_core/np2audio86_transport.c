#include "np2audio86_core.h"

#include <limits.h>
#include <string.h>

void np2audio86_event_ring_init(struct np2audio86_event_ring *ring)
{
    if (ring == NULL)
        return;
    memset(ring->slots, 0, sizeof(ring->slots));
    atomic_init(&ring->head, 0U);
    atomic_init(&ring->tail, 0U);
}

uint32_t np2audio86_event_ring_occupancy(
    const struct np2audio86_event_ring *ring)
{
    uint32_t head;
    uint32_t tail;
    if (ring == NULL)
        return 0U;
    head = atomic_load_explicit(&ring->head, memory_order_acquire);
    tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    return head - tail;
}

int np2audio86_event_ring_peek(const struct np2audio86_event_ring *ring,
                               const struct np2audio86_event **event)
{
    uint32_t head;
    uint32_t tail;
    if (ring == NULL || event == NULL)
        return NP2_AUDIO86_TRANSPORT_ARGUMENT;
    *event = NULL;
    tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    head = atomic_load_explicit(&ring->head, memory_order_acquire);
    if (head - tail > NP2_AUDIO86_ASYNC_EVENT_CAPACITY)
        return NP2_AUDIO86_TRANSPORT_INVARIANT;
    if (head == tail)
        return NP2_AUDIO86_TRANSPORT_EMPTY;
    *event = &ring->slots[tail & (NP2_AUDIO86_ASYNC_EVENT_CAPACITY - 1U)];
    return NP2_AUDIO86_TRANSPORT_OK;
}

int np2audio86_event_ring_consume(struct np2audio86_event_ring *ring)
{
    uint32_t head;
    uint32_t tail;
    if (ring == NULL)
        return NP2_AUDIO86_TRANSPORT_ARGUMENT;
    tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    head = atomic_load_explicit(&ring->head, memory_order_acquire);
    if (head - tail > NP2_AUDIO86_ASYNC_EVENT_CAPACITY)
        return NP2_AUDIO86_TRANSPORT_INVARIANT;
    if (head == tail)
        return NP2_AUDIO86_TRANSPORT_EMPTY;
    atomic_store_explicit(&ring->tail, tail + 1U, memory_order_release);
    return NP2_AUDIO86_TRANSPORT_OK;
}

int np2audio86_event_ring_enqueue(struct np2audio86_event_ring *ring,
                                  const struct np2audio86_event *event)
{
    uint32_t head;
    uint32_t tail;
    if (ring == NULL || event == NULL)
        return NP2_AUDIO86_TRANSPORT_ARGUMENT;
    head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    if (head - tail > NP2_AUDIO86_ASYNC_EVENT_CAPACITY)
        return NP2_AUDIO86_TRANSPORT_INVARIANT;
    if (head - tail == NP2_AUDIO86_ASYNC_EVENT_CAPACITY)
        return NP2_AUDIO86_TRANSPORT_FULL;
    ring->slots[head & (NP2_AUDIO86_ASYNC_EVENT_CAPACITY - 1U)] = *event;
#if defined(NP2AUDIO86_GUEST_TEST) && \
    defined(NP2_AUDIO86_GUEST_ASYNC_HARDENING_TEST)
    if (np2audio86_guest_async_hardening_cutpoint(
            NP2_AUDIO86_ASYNC_CP_EVENT_SLOT_BEFORE_HEAD, head, tail,
            0U, 0U, event->sequence) != 0)
        return NP2_AUDIO86_TRANSPORT_INVARIANT;
    if ((head & (NP2_AUDIO86_ASYNC_EVENT_CAPACITY - 1U)) ==
        NP2_AUDIO86_ASYNC_EVENT_CAPACITY - 1U)
        np2audio86_guest_async_hardening_event_wrap(head);
#endif
    atomic_store_explicit(&ring->head, head + 1U, memory_order_release);
#if defined(NP2_AUDIO86_RESET_ORDINAL_TEST)
    if (event->opcode == NP2_AUDIO86_EVENT_RESET_BARRIER)
        np2audio86_reset_ordinal_after_publish_test_hook(event->payload);
#endif
    return NP2_AUDIO86_TRANSPORT_OK;
}

int np2audio86_reset_event_ring_enqueue(
    struct np2audio86_event_ring *ring,
    const struct np2audio86_event *event,
    uint32_t *producer_reset_ordinal)
{
    struct np2audio86_event published;
    uint32_t ordinal;
    int status;
    if (ring == NULL || event == NULL || producer_reset_ordinal == NULL ||
        event->opcode != NP2_AUDIO86_EVENT_RESET_BARRIER ||
        *producer_reset_ordinal == UINT32_MAX)
        return NP2_AUDIO86_TRANSPORT_ARGUMENT;
    ordinal = *producer_reset_ordinal + 1U;
    published = *event;
    published.payload = ordinal;
    status = np2audio86_event_ring_enqueue(ring, &published);
    if (status == NP2_AUDIO86_TRANSPORT_OK)
        *producer_reset_ordinal = ordinal;
    return status;
}

int np2audio86_event_ring_dequeue(struct np2audio86_event_ring *ring,
                                  struct np2audio86_event *event)
{
    const struct np2audio86_event *slot;
    int status;
    if (event == NULL)
        return NP2_AUDIO86_TRANSPORT_ARGUMENT;
    status = np2audio86_event_ring_peek(ring, &slot);
    if (status != NP2_AUDIO86_TRANSPORT_OK)
        return status;
    *event = *slot;
    return np2audio86_event_ring_consume(ring);
}

void np2audio86_byte_ring_init(struct np2audio86_byte_ring *ring)
{
    if (ring == NULL)
        return;
    memset(ring->bytes, 0, sizeof(ring->bytes));
    atomic_init(&ring->head, 0U);
    atomic_init(&ring->tail, 0U);
}

uint32_t np2audio86_byte_ring_occupancy(
    const struct np2audio86_byte_ring *ring)
{
    uint32_t head;
    uint32_t tail;
    if (ring == NULL)
        return 0U;
    head = atomic_load_explicit(&ring->head, memory_order_acquire);
    tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    return head - tail;
}

int np2audio86_byte_ring_copy(const struct np2audio86_byte_ring *ring,
                              uint8_t *bytes, size_t count)
{
    uint32_t head;
    uint32_t tail;
    size_t first;
    if (ring == NULL || (count != 0U && bytes == NULL) ||
        count > NP2_AUDIO86_ASYNC_BYTE_CAPACITY)
        return NP2_AUDIO86_TRANSPORT_ARGUMENT;
    tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    head = atomic_load_explicit(&ring->head, memory_order_acquire);
    if (head - tail > NP2_AUDIO86_ASYNC_BYTE_CAPACITY)
        return NP2_AUDIO86_TRANSPORT_INVARIANT;
    if (count > head - tail)
        return NP2_AUDIO86_TRANSPORT_EMPTY;
    first = NP2_AUDIO86_ASYNC_BYTE_CAPACITY -
            (tail & (NP2_AUDIO86_ASYNC_BYTE_CAPACITY - 1U));
    if (first > count)
        first = count;
    if (first != 0U)
        memcpy(bytes, ring->bytes +
                          (tail & (NP2_AUDIO86_ASYNC_BYTE_CAPACITY - 1U)),
               first);
    if (count > first)
        memcpy(bytes + first, ring->bytes, count - first);
    return NP2_AUDIO86_TRANSPORT_OK;
}

int np2audio86_byte_ring_consume(struct np2audio86_byte_ring *ring,
                                 size_t count)
{
    uint32_t head;
    uint32_t tail;
    if (ring == NULL || count > NP2_AUDIO86_ASYNC_BYTE_CAPACITY)
        return NP2_AUDIO86_TRANSPORT_ARGUMENT;
    tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    head = atomic_load_explicit(&ring->head, memory_order_acquire);
    if (head - tail > NP2_AUDIO86_ASYNC_BYTE_CAPACITY)
        return NP2_AUDIO86_TRANSPORT_INVARIANT;
    if (count > head - tail)
        return NP2_AUDIO86_TRANSPORT_EMPTY;
    atomic_store_explicit(&ring->tail, tail + (uint32_t)count,
                          memory_order_release);
    return NP2_AUDIO86_TRANSPORT_OK;
}

int np2audio86_byte_ring_push(struct np2audio86_byte_ring *ring,
                             const uint8_t *bytes, size_t count)
{
    uint32_t head;
    uint32_t tail;
    size_t first;
    if (ring == NULL || (count != 0U && bytes == NULL) ||
        count > NP2_AUDIO86_ASYNC_BYTE_CAPACITY)
        return NP2_AUDIO86_TRANSPORT_ARGUMENT;
    head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    if (head - tail > NP2_AUDIO86_ASYNC_BYTE_CAPACITY)
        return NP2_AUDIO86_TRANSPORT_INVARIANT;
    if (count > NP2_AUDIO86_ASYNC_BYTE_CAPACITY - (head - tail))
        return NP2_AUDIO86_TRANSPORT_FULL;
    first = NP2_AUDIO86_ASYNC_BYTE_CAPACITY -
            (head & (NP2_AUDIO86_ASYNC_BYTE_CAPACITY - 1U));
    if (first > count)
        first = count;
    if (first != 0U)
        memcpy(ring->bytes +
                   (head & (NP2_AUDIO86_ASYNC_BYTE_CAPACITY - 1U)),
               bytes, first);
    if (count > first)
        memcpy(ring->bytes, bytes + first, count - first);
#if defined(NP2AUDIO86_GUEST_TEST) && \
    defined(NP2_AUDIO86_GUEST_ASYNC_HARDENING_TEST)
    if (np2audio86_guest_async_hardening_cutpoint(
            NP2_AUDIO86_ASYNC_CP_BYTE_COPY_BEFORE_HEAD, 0U, 0U, head, tail,
            (uint64_t)count) != 0)
        return NP2_AUDIO86_TRANSPORT_INVARIANT;
#endif
    atomic_store_explicit(&ring->head, head + (uint32_t)count,
                          memory_order_release);
    return NP2_AUDIO86_TRANSPORT_OK;
}

int np2audio86_byte_ring_pop(struct np2audio86_byte_ring *ring,
                            uint8_t *bytes, size_t count)
{
    int status = np2audio86_byte_ring_copy(ring, bytes, count);
    if (status != NP2_AUDIO86_TRANSPORT_OK)
        return status;
    return np2audio86_byte_ring_consume(ring, count);
}
