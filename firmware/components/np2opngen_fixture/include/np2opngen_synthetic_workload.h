#ifndef NP2_OPNGEN_SYNTHETIC_WORKLOAD_H
#define NP2_OPNGEN_SYNTHETIC_WORKLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "np2opngen_synth_event.h"

#ifdef __cplusplus
extern "C" {
#endif

enum np2opngen_synthetic_profile {
    NP2_OPNGEN_SYNTHETIC_LIGHT = 0,
    NP2_OPNGEN_SYNTHETIC_HEAVY,
    NP2_OPNGEN_SYNTHETIC_STRESS,
};

#define NP2_OPNGEN_SYNTHETIC_WORKLOAD_VERSION 1U
#define NP2_OPNGEN_SYNTHETIC_RATE_HZ 48000U
#define NP2_OPNGEN_SYNTHETIC_QUANTUM 240U
#define NP2_OPNGEN_SYNTHETIC_WARMUP_FRAMES NP2_OPNGEN_SYNTHETIC_RATE_HZ

struct np2opngen_synthetic_workload {
    enum np2opngen_synthetic_profile profile;
    uint32_t duration_seconds;
    uint32_t second;
    uint32_t slot;
    uint32_t slot_event;
    uint64_t next_sequence;
    uint64_t emitted_count;
    bool bootstrap;
    bool complete;
};

int np2opngen_synthetic_workload_init(
    struct np2opngen_synthetic_workload *workload,
    enum np2opngen_synthetic_profile profile, uint32_t duration_seconds);

/* Peek is side-effect free.  Commit advances the bounded generator only after
 * the caller has successfully enqueued the pending event. */
int np2opngen_synthetic_workload_peek(
    const struct np2opngen_synthetic_workload *workload,
    struct np2opngen_synth_event *event);

int np2opngen_synthetic_workload_commit(
    struct np2opngen_synthetic_workload *workload);

uint64_t np2opngen_synthetic_workload_expected_events(
    enum np2opngen_synthetic_profile profile, uint32_t duration_seconds);

const char *np2opngen_synthetic_profile_name(
    enum np2opngen_synthetic_profile profile);

#ifdef __cplusplus
}
#endif

#endif /* NP2_OPNGEN_SYNTHETIC_WORKLOAD_H */
