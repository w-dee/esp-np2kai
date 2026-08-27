#ifndef NP2_OPNGEN_SPSC_H
#define NP2_OPNGEN_SPSC_H

#include <stdatomic.h>
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
    _Atomic uint32_t head;
    _Atomic uint32_t tail;
};

void np2opngen_spsc_init(struct np2opngen_spsc_queue *queue);

int np2opngen_spsc_enqueue(struct np2opngen_spsc_queue *queue,
                           const struct np2opngen_synth_event *event);

int np2opngen_spsc_dequeue(struct np2opngen_spsc_queue *queue,
                           struct np2opngen_synth_event *event);

uint32_t np2opngen_spsc_occupancy(const struct np2opngen_spsc_queue *queue);

#ifdef __cplusplus
}
#endif

#endif /* NP2_OPNGEN_SPSC_H */
