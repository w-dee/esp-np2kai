#ifndef NP2_OPNGEN_SPSC_H
#define NP2_OPNGEN_SPSC_H

#ifdef __cplusplus
#include <atomic>
#define NP2_OPNGEN_ATOMIC(type) std::atomic<type>
#else
#include <stdatomic.h>
#define NP2_OPNGEN_ATOMIC(type) _Atomic type
#endif
#include <stdbool.h>
#include <stdint.h>

#include "np2opngen_synth_event.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NP2_OPNGEN_SPSC_CAPACITY 8U

enum np2opngen_spsc_status {
    NP2_OPNGEN_SPSC_OK = 0,
    NP2_OPNGEN_SPSC_EMPTY = 1,
    NP2_OPNGEN_SPSC_FULL = 2,
    NP2_OPNGEN_SPSC_ARGUMENT = 3,
};

struct np2opngen_spsc_queue {
    struct np2opngen_synth_event slots[NP2_OPNGEN_SPSC_CAPACITY];
    NP2_OPNGEN_ATOMIC(uint32_t) head;
    NP2_OPNGEN_ATOMIC(uint32_t) tail;
};

void np2opngen_spsc_init(struct np2opngen_spsc_queue *queue);

int np2opngen_spsc_enqueue(struct np2opngen_spsc_queue *queue,
                           const struct np2opngen_synth_event *event);

int np2opngen_spsc_dequeue(struct np2opngen_spsc_queue *queue,
                           struct np2opngen_synth_event *event);

uint32_t np2opngen_spsc_occupancy(const struct np2opngen_spsc_queue *queue);

int np2opngen_spsc_atomic_lock_free(const struct np2opngen_spsc_queue *queue,
                                    bool *head_lock_free,
                                    bool *tail_lock_free);

#ifdef __cplusplus
}
#endif

#undef NP2_OPNGEN_ATOMIC

#endif /* NP2_OPNGEN_SPSC_H */
