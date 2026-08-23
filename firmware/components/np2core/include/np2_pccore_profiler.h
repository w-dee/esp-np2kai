#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The coarse LOOP/CALLBACKS/SOUND/DRAW model follows np2_espresso's
 * g_prof_loop/g_prof_cb/g_prof_snd/g_prof_draw prior art.  DRAW is explicitly
 * nested inside LOOP and is excluded from top-level reconciliation.  Guest
 * realtime-factor and CPU-cycle comparisons are intentionally future passes. */
typedef enum {
    NP2_PCCORE_PHASE_LOOP_INCLUSIVE = 0,
    NP2_PCCORE_PHASE_CALLBACKS = 1,
    NP2_PCCORE_PHASE_SOUND = 2,
    NP2_PCCORE_PHASE_DRAW_NESTED = 3,
    NP2_PCCORE_PHASE_COUNT = 4,
} np2_pccore_phase;

typedef enum {
    NP2_PCCORE_COUNTER_LOOP_ITERATION = 0,
    NP2_PCCORE_COUNTER_CPU_EXEC_I286 = 1,
    NP2_PCCORE_COUNTER_CPU_EXEC_V30 = 2,
    NP2_PCCORE_COUNTER_CPU_SKIPPED_REMCLOCK = 3,
    NP2_PCCORE_COUNTER_NEVENT_PROGRESS = 4,
    NP2_PCCORE_COUNTER_COUNT = 5,
} np2_pccore_counter;

typedef struct {
    uint64_t count;
    uint64_t total_us;
    uint64_t max_single_us;
    uint64_t min_single_us;
} np2_pccore_phase_stats;

typedef struct {
    np2_pccore_phase_stats phases[NP2_PCCORE_PHASE_COUNT];
    uint64_t counters[NP2_PCCORE_COUNTER_COUNT];
} np2_pccore_profile;

void np2_pccore_profiler_reset(void);
void np2_pccore_profiler_set_enabled(bool enabled);
void np2_pccore_profiler_snapshot(np2_pccore_profile *profile);

#if defined(NP2_PCCORE_PHASE_PROFILER)
void np2_pccore_profiler_phase_begin(np2_pccore_phase phase);
void np2_pccore_profiler_phase_end(np2_pccore_phase phase);
void np2_pccore_profiler_count(np2_pccore_counter counter);
#else
static inline void np2_pccore_profiler_phase_begin(np2_pccore_phase phase)
{
    (void)phase;
}

static inline void np2_pccore_profiler_phase_end(np2_pccore_phase phase)
{
    (void)phase;
}

static inline void np2_pccore_profiler_count(np2_pccore_counter counter)
{
    (void)counter;
}
#endif

#ifdef __cplusplus
}
#endif
