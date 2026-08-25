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
    bool pending_cpu_executed;
} np2_pccore_profiler_state;

static np2_pccore_profiler_state np2_pccore_profile_state;
static bool np2_pccore_profile_enabled;
static np2_pccore_draw_trace *np2_pccore_active_draw_trace;
static np2_pccore_cpu_nevent_trace *np2_pccore_active_cpu_nevent_trace;

bool np2_pccore_draw_trace_append(np2_pccore_draw_trace *trace,
                                  uint64_t start_us, uint64_t end_us,
                                  uint64_t call_index)
{
    if (trace == NULL) {
        return false;
    }
    if (trace->stored >= NP2_PCCORE_DRAW_TRACE_CAPACITY ||
        call_index > UINT32_MAX) {
        trace->overflow = true;
        return false;
    }
    trace->intervals[trace->stored].start_us = start_us;
    trace->intervals[trace->stored].end_us = end_us;
    trace->intervals[trace->stored].call_index = (uint32_t)call_index;
    ++trace->stored;
    return true;
}

bool np2_pccore_cpu_nevent_trace_append(
    np2_pccore_cpu_nevent_trace *trace, uint64_t cpu_start_us,
    uint64_t nevent_start_us, uint64_t nevent_end_us, uint64_t call_index,
    bool has_cpu)
{
    if (trace == NULL) {
        return false;
    }
    if (trace->stored >= NP2_PCCORE_CPU_NEVENT_TRACE_CAPACITY ||
        call_index > UINT32_MAX) {
        trace->overflow = true;
        return false;
    }
    trace->intervals[trace->stored].cpu_start_us = cpu_start_us;
    trace->intervals[trace->stored].nevent_start_us = nevent_start_us;
    trace->intervals[trace->stored].nevent_end_us = nevent_end_us;
    trace->intervals[trace->stored].call_index = (uint32_t)call_index;
    trace->intervals[trace->stored].has_cpu = has_cpu;
    ++trace->stored;
    if (has_cpu) {
        ++trace->has_cpu_stored;
    }
    return true;
}

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
    if (phase == NP2_PCCORE_PHASE_DRAW_NESTED) {
        /* Reuse the exact start and already-read end of the profiler sample;
         * appending this record performs no additional wall-clock read. */
        (void)np2_pccore_draw_trace_append(
            np2_pccore_active_draw_trace,
            np2_pccore_profile_state.start_us[phase], end_us, stats->count);
    }
    if (phase == NP2_PCCORE_PHASE_NEVENT_PROGRESS_NESTED) {
        /* CPU_EXEC -> NEVENT uses the transition timestamp already stored in
         * start_us[NEVENT].  Appending here adds no timer read and emits one
         * paired record for every completed NEVENT invocation. */
        (void)np2_pccore_cpu_nevent_trace_append(
            np2_pccore_active_cpu_nevent_trace,
            np2_pccore_profile_state.pending_cpu_executed
                ? np2_pccore_profile_state.start_us[
                      NP2_PCCORE_PHASE_CPU_EXEC_NESTED]
                : np2_pccore_profile_state.start_us[phase],
            np2_pccore_profile_state.start_us[phase], end_us, stats->count,
            np2_pccore_profile_state.pending_cpu_executed);
    }
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

void np2_pccore_draw_trace_reset(np2_pccore_draw_trace *trace)
{
    if (trace != NULL) {
        trace->stored = 0U;
        trace->overflow = false;
        trace->reentrant = false;
    }
}

void np2_pccore_cpu_nevent_trace_reset(np2_pccore_cpu_nevent_trace *trace)
{
    if (trace != NULL) {
        trace->stored = 0U;
        trace->has_cpu_stored = 0U;
        trace->overflow = false;
    }
}

void np2_pccore_profiler_set_draw_trace(np2_pccore_draw_trace *trace)
{
    np2_pccore_active_draw_trace = trace;
}

void np2_pccore_profiler_set_cpu_nevent_trace(
    np2_pccore_cpu_nevent_trace *trace)
{
    np2_pccore_active_cpu_nevent_trace = trace;
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
    if (phase == NP2_PCCORE_PHASE_DRAW_NESTED &&
        np2_pccore_profile_state.active[phase] &&
        np2_pccore_active_draw_trace != NULL) {
        /* Preserve existing profiler semantics while making unexpected
         * reentrancy a hard-invalid trace condition. */
        np2_pccore_active_draw_trace->reentrant = true;
    }
    np2_pccore_profile_state.start_us[phase] =
        (uint64_t)esp_timer_get_time();
    np2_pccore_profile_state.active[phase] = true;
    if (phase == NP2_PCCORE_PHASE_CPU_EXEC_NESTED) {
        np2_pccore_profile_state.pending_cpu_executed = true;
    } else if (phase == NP2_PCCORE_PHASE_NEVENT_PROGRESS_NESTED &&
               !np2_pccore_profile_state.active[
                   NP2_PCCORE_PHASE_CPU_EXEC_NESTED]) {
        /* This is the existing CPU_REMCLOCK-skipped path. */
        np2_pccore_profile_state.pending_cpu_executed = false;
    }
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
