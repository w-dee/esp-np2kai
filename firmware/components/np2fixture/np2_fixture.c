#include "np2_fixture.h"

#include <string.h>

#include "esp_partition.h"

#include <compiler.h>
#include "common.h"
#include <diskimage/fddfile.h>
#include <np2host/dosio_esp.h>

typedef enum {
    NP2_FIXTURE_SOURCE_NONE = 0,
    NP2_FIXTURE_SOURCE_RAW,
    NP2_FIXTURE_SOURCE_VFS,
} np2_fixture_source;

static const np2_fixture_descriptor *np2_fixture_get_descriptor(
    const np2_fixture *fixture)
{
    return ((fixture != NULL) && (fixture->descriptor != NULL)) ?
               fixture->descriptor : np2_fixture_default_descriptor();
}

const char *np2_fixture_error_name(esp_err_t error)
{
    switch (error) {
        case ESP_ERR_INVALID_ARG:
            return "invalid_argument";
        case ESP_ERR_NOT_FOUND:
            return "partition_missing";
        case ESP_ERR_INVALID_SIZE:
            return "size_or_geometry_mismatch";
        case ESP_ERR_INVALID_CRC:
            return "sha256_mismatch";
        case ESP_ERR_INVALID_STATE:
            return "invalid_state";
        case ESP_OK:
            return "ok";
        default:
            return "fixture_error";
    }
}

static int np2_fixture_valid_descriptor(
    const np2_fixture_descriptor *descriptor)
{
    return (descriptor != NULL) &&
           (descriptor->partition_label != NULL) &&
           (descriptor->logical_path != NULL) &&
           (descriptor->expected_sha256 != NULL) &&
           (descriptor->image_size != 0U) &&
           (descriptor->tracks != 0U) &&
           (descriptor->sectors != 0U) &&
           (descriptor->n != 0U);
}

void np2_fixture_init(np2_fixture *fixture)
{
    if (fixture != NULL) {
        memset(fixture, 0, sizeof(*fixture));
    }
}

esp_err_t np2_fixture_acquire(np2_fixture *fixture)
{
    return np2_fixture_acquire_for(fixture, np2_fixture_default_descriptor());
}

esp_err_t np2_fixture_acquire_for(
    np2_fixture *fixture, const np2_fixture_descriptor *descriptor)
{
    const esp_partition_t *partition;
    const void *mapped = NULL;

    if ((fixture == NULL) || !np2_fixture_valid_descriptor(descriptor)) {
        return ESP_ERR_INVALID_ARG;
    }
    np2_fixture_init(fixture);

    partition = esp_partition_find_first(NP2_FIXTURE_PARTITION_TYPE,
                                          NP2_FIXTURE_PARTITION_SUBTYPE,
                                          descriptor->partition_label);
    if (partition == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if ((partition->size < descriptor->image_size) || !partition->readonly) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (esp_partition_get_sha256(partition, fixture->digest) != ESP_OK) {
        return ESP_FAIL;
    }
    if (memcmp(fixture->digest, descriptor->expected_sha256,
               sizeof(fixture->digest)) != 0) {
        return ESP_ERR_INVALID_CRC;
    }
    if (esp_partition_mmap(partition, 0, descriptor->image_size,
                           ESP_PARTITION_MMAP_DATA,
                           &mapped,
                           &fixture->map_handle) != ESP_OK) {
        return ESP_FAIL;
    }
    fixture->partition = partition;
    fixture->mapped = mapped;
    fixture->descriptor = descriptor;
    fixture->source = NP2_FIXTURE_SOURCE_RAW;
    return ESP_OK;
}

esp_err_t np2_fixture_attach_dosio(np2_fixture *fixture)
{
    const np2_fixture_descriptor *descriptor;

    if ((fixture == NULL) || (fixture->mapped == NULL) ||
        fixture->dosio_attached) {
        return ESP_ERR_INVALID_STATE;
    }
    descriptor = np2_fixture_get_descriptor(fixture);
    if (!np2_dosio_attach_fixture(descriptor->logical_path,
                                  fixture->mapped,
                                  descriptor->image_size)) {
        return ESP_FAIL;
    }
    fixture->dosio_attached = 1;
    return ESP_OK;
}

esp_err_t np2_fixture_attach_vfs_dosio(np2_fixture *fixture,
                                       const char *physical_path)
{
    const np2_fixture_descriptor *descriptor;
    esp_err_t verification;

    if ((fixture == NULL) || (fixture->source != NP2_FIXTURE_SOURCE_NONE) ||
        fixture->mapped != NULL || fixture->dosio_attached) {
        return ESP_ERR_INVALID_ARG;
    }
    descriptor = np2_fixture_get_descriptor(fixture);
    verification = np2_fixture_verify_vfs_file(descriptor, physical_path,
                                               fixture->digest);
    if (verification != ESP_OK) {
        return verification;
    }
    if (!np2_dosio_attach_vfs_file(descriptor->logical_path, physical_path)) {
        return ESP_FAIL;
    }
    fixture->descriptor = descriptor;
    fixture->source = NP2_FIXTURE_SOURCE_VFS;
    fixture->dosio_attached = 1;
    return ESP_OK;
}

esp_err_t np2_fixture_attach_fdd(np2_fixture *fixture)
{
    FDDFILE fdd;
    const np2_fixture_descriptor *descriptor;

    if ((fixture == NULL) || !fixture->dosio_attached ||
        fixture->fdd_attached) {
        return ESP_ERR_INVALID_STATE;
    }
    descriptor = np2_fixture_get_descriptor(fixture);
    if (fdd_set(0, descriptor->logical_path, FTYPE_NONE, 1) != SUCCESS ||
        !fdd_diskready(0)) {
        return ESP_FAIL;
    }
    fdd = fddfile + 0;
    if ((fdd->type != DISKTYPE_BETA) ||
        (fdd->inf.xdf.tracks != descriptor->tracks) ||
        (fdd->inf.xdf.sectors != descriptor->sectors) ||
        (fdd->inf.xdf.n != descriptor->n) ||
        (fdd->inf.xdf.disktype != descriptor->disktype) ||
        !fdd->ro) {
        fdd_eject(0);
        return ESP_ERR_INVALID_SIZE;
    }
    fixture->fdd_attached = 1;
    return ESP_OK;
}

void np2_fixture_detach_fdd(np2_fixture *fixture)
{
    if ((fixture != NULL) && fixture->fdd_attached) {
        fdd_eject(0);
        fixture->fdd_attached = 0;
    }
}

void np2_fixture_release(np2_fixture *fixture)
{
    if (fixture == NULL) {
        return;
    }
    np2_fixture_detach_fdd(fixture);
    if (fixture->dosio_attached) {
        if (fixture->source == NP2_FIXTURE_SOURCE_VFS) {
            np2_dosio_detach_vfs_file();
        } else {
            np2_dosio_detach_fixture();
        }
        fixture->dosio_attached = 0;
    }
    if ((fixture->source == NP2_FIXTURE_SOURCE_RAW) &&
        (fixture->mapped != NULL)) {
        esp_partition_munmap(fixture->map_handle);
    }
    np2_fixture_init(fixture);
}
