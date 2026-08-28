#include "np2opngen_pcm_ring.h"

#include <limits.h>
#include <string.h>

static uint32_t ring_head(const struct np2opngen_pcm_ring *ring)
{
    return atomic_load_explicit(&ring->head, memory_order_relaxed);
}

static uint32_t ring_tail(const struct np2opngen_pcm_ring *ring)
{
    return atomic_load_explicit(&ring->tail, memory_order_acquire);
}

void np2opngen_pcm_ring_init(struct np2opngen_pcm_ring *ring)
{
    if (ring == 0) {
        return;
    }
    memset(ring->slots, 0, sizeof(ring->slots));
    ring->next_frame_offset = 0U;
    ring->next_sequence = 0U;
    ring->partial_valid_frames = 0U;
    ring->finalized = false;
    atomic_init(&ring->head, 0U);
    atomic_init(&ring->tail, 0U);
}

uint32_t np2opngen_pcm_ring_occupancy(
    const struct np2opngen_pcm_ring *ring)
{
    uint32_t head;
    uint32_t tail;
    if (ring == 0) {
        return 0U;
    }
    head = atomic_load_explicit(&ring->head, memory_order_acquire);
    tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    return head - tail;
}

uint16_t np2opngen_pcm_ring_producer_partial_valid_frames(
    const struct np2opngen_pcm_ring *ring)
{
    return ring == 0 ? 0U : ring->partial_valid_frames;
}

int np2opngen_pcm_ring_append(struct np2opngen_pcm_ring *ring,
                              const uint8_t *canonical_pcm,
                              size_t frame_count, uint64_t frame_offset,
                              size_t *frames_consumed)
{
    size_t consumed = 0U;
    uint32_t head;

    if (frames_consumed == 0 || ring == 0) {
        return NP2_OPNGEN_PCM_RING_ARGUMENT;
    }
    *frames_consumed = 0U;
    if ((frame_count != 0U && canonical_pcm == 0) ||
        frame_count > SIZE_MAX / NP2_OPNGEN_PCM_RING_BYTES_PER_FRAME) {
        return NP2_OPNGEN_PCM_RING_ARGUMENT;
    }
    if (ring->finalized) {
        return NP2_OPNGEN_PCM_RING_FINALIZED;
    }
    if (frame_offset != ring->next_frame_offset) {
        return NP2_OPNGEN_PCM_RING_OFFSET;
    }
    if ((uint64_t)frame_count > UINT64_MAX - frame_offset) {
        return NP2_OPNGEN_PCM_RING_ARGUMENT;
    }
    head = ring_head(ring);
    while (consumed < frame_count) {
        struct np2opngen_pcm_ring_slot *slot;
        uint32_t tail;
        size_t available;
        size_t chunk;
        size_t byte_offset;

        tail = ring_tail(ring);
        if (head - tail > NP2_OPNGEN_PCM_RING_CAPACITY) {
            *frames_consumed = consumed;
            return NP2_OPNGEN_PCM_RING_INVARIANT;
        }
        if (head - tail == NP2_OPNGEN_PCM_RING_CAPACITY) {
            *frames_consumed = consumed;
            return NP2_OPNGEN_PCM_RING_FULL;
        }
        slot = &ring->slots[head & (NP2_OPNGEN_PCM_RING_CAPACITY - 1U)];
        if (ring->partial_valid_frames == 0U) {
            slot->frame_offset = ring->next_frame_offset;
            slot->sequence = ring->next_sequence;
            slot->flags = 0U;
        }
        available = NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES -
                    ring->partial_valid_frames;
        chunk = frame_count - consumed;
        if (chunk > available) {
            chunk = available;
        }
        byte_offset = (size_t)ring->partial_valid_frames *
                      NP2_OPNGEN_PCM_RING_BYTES_PER_FRAME;
        memcpy(slot->pcm + byte_offset,
               canonical_pcm + consumed * NP2_OPNGEN_PCM_RING_BYTES_PER_FRAME,
               chunk * NP2_OPNGEN_PCM_RING_BYTES_PER_FRAME);
        ring->partial_valid_frames = (uint16_t)(ring->partial_valid_frames +
                                                chunk);
        ring->next_frame_offset += chunk;
        consumed += chunk;
        if (ring->partial_valid_frames == NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES) {
            slot->valid_frames = NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES;
            slot->flags = 0U;
            atomic_store_explicit(&ring->head, head + 1U,
                                  memory_order_release);
            head++;
            ring->next_sequence++;
            ring->partial_valid_frames = 0U;
        }
    }
    *frames_consumed = consumed;
    return NP2_OPNGEN_PCM_RING_OK;
}

int np2opngen_pcm_ring_try_peek(
    const struct np2opngen_pcm_ring *ring,
    const struct np2opngen_pcm_ring_slot **slot)
{
    uint32_t head;
    uint32_t tail;
    if (ring == 0 || slot == 0) {
        return NP2_OPNGEN_PCM_RING_ARGUMENT;
    }
    *slot = 0;
    tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    head = atomic_load_explicit(&ring->head, memory_order_acquire);
    if (head - tail > NP2_OPNGEN_PCM_RING_CAPACITY) {
        return NP2_OPNGEN_PCM_RING_INVARIANT;
    }
    if (head == tail) {
        return NP2_OPNGEN_PCM_RING_EMPTY;
    }
    *slot = &ring->slots[tail & (NP2_OPNGEN_PCM_RING_CAPACITY - 1U)];
    return NP2_OPNGEN_PCM_RING_OK;
}

int np2opngen_pcm_ring_consume(struct np2opngen_pcm_ring *ring)
{
    uint32_t head;
    uint32_t tail;
    if (ring == 0) {
        return NP2_OPNGEN_PCM_RING_ARGUMENT;
    }
    tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    head = atomic_load_explicit(&ring->head, memory_order_acquire);
    if (head - tail > NP2_OPNGEN_PCM_RING_CAPACITY) {
        return NP2_OPNGEN_PCM_RING_INVARIANT;
    }
    if (head == tail) {
        return NP2_OPNGEN_PCM_RING_EMPTY;
    }
    atomic_store_explicit(&ring->tail, tail + 1U, memory_order_release);
    return NP2_OPNGEN_PCM_RING_OK;
}

int np2opngen_pcm_ring_finish(struct np2opngen_pcm_ring *ring,
                              uint64_t final_frame)
{
    uint32_t head;
    uint32_t tail;
    struct np2opngen_pcm_ring_slot *slot;

    if (ring == 0) {
        return NP2_OPNGEN_PCM_RING_ARGUMENT;
    }
    if (ring->finalized) {
        return NP2_OPNGEN_PCM_RING_FINALIZED;
    }
    if (final_frame != ring->next_frame_offset) {
        return NP2_OPNGEN_PCM_RING_OFFSET;
    }
    if (ring->partial_valid_frames == 0U) {
        ring->finalized = true;
        return NP2_OPNGEN_PCM_RING_OK;
    }
    head = ring_head(ring);
    tail = ring_tail(ring);
    if (head - tail > NP2_OPNGEN_PCM_RING_CAPACITY) {
        return NP2_OPNGEN_PCM_RING_INVARIANT;
    }
    if (head - tail == NP2_OPNGEN_PCM_RING_CAPACITY) {
        return NP2_OPNGEN_PCM_RING_FULL;
    }
    slot = &ring->slots[head & (NP2_OPNGEN_PCM_RING_CAPACITY - 1U)];
    slot->valid_frames = ring->partial_valid_frames;
    slot->flags = NP2_OPNGEN_PCM_RING_FLAG_FINAL_PARTIAL;
    atomic_store_explicit(&ring->head, head + 1U, memory_order_release);
    ring->next_sequence++;
    ring->partial_valid_frames = 0U;
    ring->finalized = true;
    return NP2_OPNGEN_PCM_RING_OK;
}
