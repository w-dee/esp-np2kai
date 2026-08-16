#pragma once

#include <stdbool.h>
#include <stddef.h>

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

/* Serialized machine-output sink shared by firmware runtime tasks. */
bool uart_control_transport_write(const char *data, size_t length);

#ifdef __cplusplus
}
#endif
