#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NP2TEST_PROFILE_FORMAL = 0,
    NP2TEST_PROFILE_REDUCED_EXTMEM8,
} np2test_profile;

typedef bool (*np2test_runner_output_fn)(void *context,
                                         const char *data,
                                         size_t length);

typedef struct {
    np2test_profile profile;
    np2test_runner_output_fn output;
    void *output_context;
} np2test_runner_config;

esp_err_t np2test_runner_start(const np2test_runner_config *config);

#ifdef __cplusplus
}
#endif
