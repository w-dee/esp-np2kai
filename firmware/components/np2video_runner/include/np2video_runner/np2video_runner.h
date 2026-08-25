#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "np2_pccore_profiler.h"
#include "np2video_runner/pccore_trace.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*np2video_runner_output_fn)(void *context,
                                          const char *data,
                                          size_t length);

typedef struct {
    esp_err_t status;
    uint32_t source_generation;
    uint32_t source_update_sequence;
    uint32_t source_crc32;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;
    uint32_t visible_bytes;
    uint32_t cooperate_calls;
    uint64_t pccore_exec_count;
    uint64_t pccore_exec_first_us;
    uint64_t pccore_exec_min_us;
    uint64_t pccore_exec_max_us;
    uint64_t pccore_exec_total_us;
    np2_pccore_profile pccore_profile;
} np2video_runner_result;

typedef void (*np2video_runner_complete_fn)(
    const np2video_runner_result *result, void *context);

typedef bool (*np2video_runner_ready_fn)(void *context);
typedef void (*np2video_runner_scene_ready_fn)(uint32_t generation,
                                               uint32_t update_sequence,
                                               void *context);
typedef void (*np2video_runner_stopping_fn)(void *context);
typedef bool (*np2video_runner_stop_requested_fn)(void *context);
typedef void (*np2video_runner_cooperate_fn)(uint32_t cooperate_calls,
                                             void *context);
typedef bool (*np2video_runner_pause_at_cooperate_fn)(
    uint32_t cooperate_calls, void *context);

typedef struct {
    np2video_runner_output_fn output;
    void *output_context;
    np2video_runner_ready_fn ready;
    np2video_runner_scene_ready_fn scene_ready;
    np2video_runner_stopping_fn stopping;
    np2video_runner_complete_fn complete;
    void *complete_context;
    void *lifecycle_context;
    np2video_runner_stop_requested_fn stop_requested;
    np2video_runner_cooperate_fn cooperate;
    np2video_runner_pause_at_cooperate_fn pause_at_cooperate;
    /* Optional benchmark-owned storage.  The runner is the sole writer and
     * the owner reads it only after the completion callback publication. */
    np2video_pccore_trace *pccore_trace;
    /* Retains existing DRAW_NESTED profiler timestamps; NULL in production. */
    np2_pccore_draw_trace *draw_trace;
    bool task_scheduling_override;
    int task_core_id;
    uint32_t task_priority;
} np2video_runner_config;

esp_err_t np2video_runner_start(np2video_runner_output_fn output,
                                void *output_context);
esp_err_t np2video_runner_start_ex(const np2video_runner_config *config);

#ifdef __cplusplus
}
#endif
