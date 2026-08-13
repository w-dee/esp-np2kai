#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *project;
    const char *firmware_version;
    const char *idf_version;
    const char *target;
} uart_control_metadata_t;

esp_err_t uart_control_transport_start(const uart_control_metadata_t *metadata);

#ifdef __cplusplus
}
#endif
