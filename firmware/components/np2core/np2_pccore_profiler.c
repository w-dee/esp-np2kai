#include "np2_pccore_profiler.h"

#include <stddef.h>
#include <string.h>

#if defined(NP2_PCCORE_PHASE_PROFILER)
#include "esp_timer.h"
#endif

static np2_pccore_profile np2_pccore_profile_state;
static bool np2_pccore_profile_enabled;

void np2_pccore_profiler_reset(void)
{
    memset(&np2_pccore_profile_state, 0,
           sizeof(np2_pccore_profile_state));
    for (unsigned int index = 0; index < NP2_PCCORE_PHASE_COUNT; ++index) {
        np2_pccore_profile_state.phases[index].min_single_us = UINT64_MAX;
    }
}

void np2_pccore_profiler_set_enabled(bool enabled)
{
    np2_pccore_profile_enabled = enabled;
}

void np2_pccore_profiler_snapshot(np2_pccore_profile *profile)
{
    if (profile == NULL) {
        return;
    }
    *profile = np2_pccore_profile_state;
    for (unsigned int index = 0; index < NP2_PCCORE_PHASE_COUNT; ++index) {
        if (profile->phases[index].count == 0U) {
            profile->phases[index].min_single_us = 0U;
        }
    }
}

#if defined(NP2_PCCORE_PHASE_PROFILER)
uint64_t np2_pccore_profiler_phase_begin(np2_pccore_phase phase)
{
    if (!np2_pccore_profile_enabled || phase >= NP2_PCCORE_PHASE_COUNT) {
        return 0U;
    }
    return (uint64_t)esp_timer_get_time();
}

void np2_pccore_profiler_phase_end(np2_pccore_phase phase,
                                   uint64_t start_us)
{
    uint64_t elapsed_us;
    np2_pccore_phase_stats *stats;

    if (!np2_pccore_profile_enabled || phase >= NP2_PCCORE_PHASE_COUNT) {
        return;
    }
    elapsed_us = (uint64_t)esp_timer_get_time() - start_us;
    stats = &np2_pccore_profile_state.phases[phase];
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
