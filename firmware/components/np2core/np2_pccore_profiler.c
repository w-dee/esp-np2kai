#include "np2_pccore_profiler.h"

#include <stddef.h>
#include <string.h>

#if defined(NP2_PCCORE_PHASE_PROFILER)
#include "esp_timer.h"
#endif

typedef struct {
    np2_pccore_profile profile;
    uint64_t start_us[NP2_PCCORE_PHASE_COUNT];
    bool active[NP2_PCCORE_PHASE_COUNT];
} np2_pccore_profiler_state;

static np2_pccore_profiler_state np2_pccore_profile_state;
static bool np2_pccore_profile_enabled;

#if defined(NP2_PCCORE_PHASE_PROFILER)
static void np2_pccore_profiler_record_end_at(np2_pccore_phase phase,
                                               uint64_t end_us)
{
    /* Each completed interval is one vendor-defined invocation sample. */
    const uint64_t elapsed_us =
        end_us - np2_pccore_profile_state.start_us[phase];
    np2_pccore_phase_stats *stats =
        &np2_pccore_profile_state.profile.phases[phase];

    np2_pccore_profile_state.active[phase] = false;
    ++stats->count;
    stats->total_us += elapsed_us;
    if (elapsed_us > stats->max_single_us) {
        stats->max_single_us = elapsed_us;
    }
    if (elapsed_us < stats->min_single_us) {
        stats->min_single_us = elapsed_us;
    }
}
#endif

void np2_pccore_profiler_reset(void)
{
    memset(&np2_pccore_profile_state, 0, sizeof(np2_pccore_profile_state));
    for (unsigned int index = 0; index < NP2_PCCORE_PHASE_COUNT; ++index) {
        np2_pccore_profile_state.profile.phases[index].min_single_us =
            UINT64_MAX;
    }
}

void np2_pccore_profiler_set_enabled(bool enabled)
{
    np2_pccore_profile_enabled = enabled;
}

#if defined(NP2_PCCORE_PHASE_PROFILER)
void np2_pccore_profiler_count(np2_pccore_counter counter)
{
    if (!np2_pccore_profile_enabled || counter >= NP2_PCCORE_COUNTER_COUNT) {
        return;
    }
    ++np2_pccore_profile_state.profile.counters[counter];
}
#endif

void np2_pccore_profiler_snapshot(np2_pccore_profile *profile)
{
    if (profile == NULL) {
        return;
    }
    *profile = np2_pccore_profile_state.profile;
    for (unsigned int index = 0; index < NP2_PCCORE_PHASE_COUNT; ++index) {
        if (profile->phases[index].count == 0U) {
            profile->phases[index].min_single_us = 0U;
        }
    }
}

#if defined(NP2_PCCORE_PHASE_PROFILER)
void np2_pccore_profiler_phase_begin(np2_pccore_phase phase)
{
    if (!np2_pccore_profile_enabled || phase >= NP2_PCCORE_PHASE_COUNT) {
        return;
    }
    np2_pccore_profile_state.start_us[phase] =
        (uint64_t)esp_timer_get_time();
    np2_pccore_profile_state.active[phase] = true;
}

void np2_pccore_profiler_phase_end(np2_pccore_phase phase)
{
    uint64_t end_us;

    if (!np2_pccore_profile_enabled || phase >= NP2_PCCORE_PHASE_COUNT) {
        return;
    }
    if (!np2_pccore_profile_state.active[phase]) {
        return;
    }
    end_us = (uint64_t)esp_timer_get_time();
    np2_pccore_profiler_record_end_at(phase, end_us);
}

void np2_pccore_profiler_phase_transition(np2_pccore_phase from,
                                          np2_pccore_phase to)
{
    uint64_t now_us;

    if (!np2_pccore_profile_enabled ||
        from < 0 || to < 0 || from >= NP2_PCCORE_PHASE_COUNT ||
        to >= NP2_PCCORE_PHASE_COUNT ||
        !np2_pccore_profile_state.active[from] ||
        np2_pccore_profile_state.active[to]) {
        return;
    }
    /* One wall-clock read closes FROM and opens TO at the same boundary. */
    now_us = (uint64_t)esp_timer_get_time();
    np2_pccore_profiler_record_end_at(from, now_us);
    np2_pccore_profile_state.start_us[to] = now_us;
    np2_pccore_profile_state.active[to] = true;
}
#endif
