#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

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
} np2video_runner_result;

typedef void (*np2video_runner_complete_fn)(
    const np2video_runner_result *result, void *context);

typedef bool (*np2video_runner_ready_fn)(void *context);
typedef void (*np2video_runner_scene_ready_fn)(uint32_t generation,
                                               uint32_t update_sequence,
                                               void *context);
typedef void (*np2video_runner_stopping_fn)(void *context);
typedef bool (*np2video_runner_stop_requested_fn)(void *context);

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
} np2video_runner_config;

esp_err_t np2video_runner_start(np2video_runner_output_fn output,
                                void *output_context);
esp_err_t np2video_runner_start_ex(const np2video_runner_config *config);

#ifdef __cplusplus
}
#endif
