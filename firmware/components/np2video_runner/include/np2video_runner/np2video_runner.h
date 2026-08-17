#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*np2video_runner_output_fn)(void *context,
                                          const char *data,
                                          size_t length);

esp_err_t np2video_runner_start(np2video_runner_output_fn output,
                                void *output_context);

#ifdef __cplusplus
}
#endif
