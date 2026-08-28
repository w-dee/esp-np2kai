#ifndef NP2_OPNGEN_PCM_RING_H
#define NP2_OPNGEN_PCM_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include <atomic>
#define NP2_OPNGEN_PCM_RING_ATOMIC(type) std::atomic<type>
extern "C" {
#else
#include <stdatomic.h>
#define NP2_OPNGEN_PCM_RING_ATOMIC(type) _Atomic type
#endif

#define NP2_OPNGEN_PCM_RING_CAPACITY 8U
#define NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES 240U
#define NP2_OPNGEN_PCM_RING_BYTES_PER_FRAME 4U
#define NP2_OPNGEN_PCM_RING_SLOT_BYTES \
    (NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES * NP2_OPNGEN_PCM_RING_BYTES_PER_FRAME)
#define NP2_OPNGEN_PCM_RING_FLAG_FINAL_PARTIAL 0x0001U

enum np2opngen_pcm_ring_status {
    NP2_OPNGEN_PCM_RING_OK = 0,
    NP2_OPNGEN_PCM_RING_EMPTY,
    NP2_OPNGEN_PCM_RING_FULL,
    NP2_OPNGEN_PCM_RING_ARGUMENT,
    NP2_OPNGEN_PCM_RING_OFFSET,
    NP2_OPNGEN_PCM_RING_FINALIZED,
    NP2_OPNGEN_PCM_RING_INVARIANT,
};

struct np2opngen_pcm_ring_slot {
    uint64_t frame_offset;
    uint32_t sequence;
    uint16_t valid_frames;
    uint16_t flags;
    uint8_t pcm[NP2_OPNGEN_PCM_RING_SLOT_BYTES];
};

struct np2opngen_pcm_ring {
    struct np2opngen_pcm_ring_slot slots[NP2_OPNGEN_PCM_RING_CAPACITY];
    NP2_OPNGEN_PCM_RING_ATOMIC(uint32_t) head;
    NP2_OPNGEN_PCM_RING_ATOMIC(uint32_t) tail;
    uint64_t next_frame_offset;
    uint32_t next_sequence;
    uint16_t partial_valid_frames;
    bool finalized;
};

#if defined(__cplusplus)
static_assert((NP2_OPNGEN_PCM_RING_CAPACITY &
               (NP2_OPNGEN_PCM_RING_CAPACITY - 1U)) == 0U,
              "PCM ring capacity must be a power of two");
static_assert(NP2_OPNGEN_PCM_RING_CAPACITY == 8U,
              "PCM ring capacity is part of the A2 contract");
static_assert(NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES == 240U,
              "PCM ring quantum is part of the A2 contract");
static_assert(NP2_OPNGEN_PCM_RING_SLOT_BYTES == 960U,
              "PCM ring slot payload is part of the A2 contract");
static_assert(NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES <= UINT16_MAX,
              "valid_frames must represent a complete slot");
#else
_Static_assert((NP2_OPNGEN_PCM_RING_CAPACITY &
                (NP2_OPNGEN_PCM_RING_CAPACITY - 1U)) == 0U,
               "PCM ring capacity must be a power of two");
_Static_assert(NP2_OPNGEN_PCM_RING_CAPACITY == 8U,
               "PCM ring capacity is part of the A2 contract");
_Static_assert(NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES == 240U,
               "PCM ring quantum is part of the A2 contract");
_Static_assert(NP2_OPNGEN_PCM_RING_SLOT_BYTES == 960U,
               "PCM ring slot payload is part of the A2 contract");
_Static_assert(NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES <= UINT16_MAX,
               "valid_frames must represent a complete slot");
#endif

void np2opngen_pcm_ring_init(struct np2opngen_pcm_ring *ring);

int np2opngen_pcm_ring_append(struct np2opngen_pcm_ring *ring,
                              const uint8_t *canonical_pcm,
                              size_t frame_count, uint64_t frame_offset,
                              size_t *frames_consumed);

int np2opngen_pcm_ring_try_peek(
    const struct np2opngen_pcm_ring *ring,
    const struct np2opngen_pcm_ring_slot **slot);

int np2opngen_pcm_ring_consume(struct np2opngen_pcm_ring *ring);

uint32_t np2opngen_pcm_ring_occupancy(
    const struct np2opngen_pcm_ring *ring);

uint16_t np2opngen_pcm_ring_producer_partial_valid_frames(
    const struct np2opngen_pcm_ring *ring);

int np2opngen_pcm_ring_finish(struct np2opngen_pcm_ring *ring,
                              uint64_t final_frame);

#ifdef __cplusplus
}
#endif

#undef NP2_OPNGEN_PCM_RING_ATOMIC

#endif /* NP2_OPNGEN_PCM_RING_H */
