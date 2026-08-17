#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NP2_FIXTURE_SHA256_SIZE 32U
#define NP2_FIXTURE_PARTITION_TYPE ((esp_partition_type_t)0x40)
#define NP2_FIXTURE_PARTITION_SUBTYPE ((esp_partition_subtype_t)0x01)
#define NP2_FIXTURE_PARTITION_LABEL "np2test"
#define NP2_FIXTURE_PATH "./np2test-fd1232.hdm"
#define NP2_FIXTURE_IMAGE_SIZE 0x134000U
#define NP2_FIXTURE_TRACKS 154U
#define NP2_FIXTURE_SECTORS 8U
#define NP2_FIXTURE_N 3U

typedef struct {
    const char *partition_label;
    const char *logical_path;
    size_t image_size;
    const uint8_t *expected_sha256;
    unsigned tracks;
    unsigned sectors;
    unsigned n;
    unsigned disktype;
} np2_fixture_descriptor;

typedef struct {
    const esp_partition_t *partition;
    const uint8_t *mapped;
    esp_partition_mmap_handle_t map_handle;
    uint8_t digest[NP2_FIXTURE_SHA256_SIZE];
    const np2_fixture_descriptor *descriptor;
    int source;
    int dosio_attached;
    int fdd_attached;
} np2_fixture;

void np2_fixture_init(np2_fixture *fixture);
esp_err_t np2_fixture_acquire(np2_fixture *fixture);
esp_err_t np2_fixture_acquire_for(
    np2_fixture *fixture, const np2_fixture_descriptor *descriptor);
esp_err_t np2_fixture_attach_dosio(np2_fixture *fixture);
esp_err_t np2_fixture_attach_vfs_dosio(np2_fixture *fixture,
                                       const char *physical_path);
esp_err_t np2_fixture_attach_fdd(np2_fixture *fixture);
void np2_fixture_detach_fdd(np2_fixture *fixture);
void np2_fixture_release(np2_fixture *fixture);

const char *np2_fixture_error_name(esp_err_t error);

#ifdef __cplusplus
}
#endif
