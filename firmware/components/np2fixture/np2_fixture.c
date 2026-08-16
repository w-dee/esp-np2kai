#include "np2_fixture.h"

#include <string.h>

#include "esp_partition.h"

#include <compiler.h>
#include <diskimage/fddfile.h>
#include <np2host/dosio_esp.h>

static const uint8_t np2_fixture_expected_sha256[32] = {
    0x3b, 0x73, 0x66, 0x7d, 0x23, 0x56, 0x15, 0xe8,
    0x92, 0x05, 0xfb, 0xda, 0xb0, 0x4d, 0x3e, 0x6c,
    0xf9, 0xc2, 0xf9, 0xa1, 0xf3, 0xa1, 0xde, 0x82,
    0xcd, 0xb2, 0xb3, 0x86, 0x2a, 0xa3, 0x94, 0xb3,
};

const char *np2_fixture_error_name(esp_err_t error)
{
    switch (error) {
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

esp_err_t np2_fixture_acquire(np2_fixture *fixture)
{
    const esp_partition_t *partition;
    const void *mapped = NULL;

    if (fixture == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(fixture, 0, sizeof(*fixture));

    partition = esp_partition_find_first(NP2_FIXTURE_PARTITION_TYPE,
                                          NP2_FIXTURE_PARTITION_SUBTYPE,
                                          NP2_FIXTURE_PARTITION_LABEL);
    if (partition == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if ((partition->size < NP2_FIXTURE_IMAGE_SIZE) || !partition->readonly) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (esp_partition_get_sha256(partition, fixture->digest) != ESP_OK) {
        return ESP_FAIL;
    }
    if (memcmp(fixture->digest, np2_fixture_expected_sha256,
               sizeof(fixture->digest)) != 0) {
        return ESP_ERR_INVALID_CRC;
    }
    if (esp_partition_mmap(partition, 0, NP2_FIXTURE_IMAGE_SIZE,
                           ESP_PARTITION_MMAP_DATA,
                           &mapped,
                           &fixture->map_handle) != ESP_OK) {
        return ESP_FAIL;
    }
    fixture->partition = partition;
    fixture->mapped = mapped;
    return ESP_OK;
}

esp_err_t np2_fixture_attach_dosio(np2_fixture *fixture)
{
    if ((fixture == NULL) || (fixture->mapped == NULL) ||
        fixture->dosio_attached) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!np2_dosio_attach_fixture(NP2_FIXTURE_PATH,
                                  fixture->mapped,
                                  NP2_FIXTURE_IMAGE_SIZE)) {
        return ESP_FAIL;
    }
    fixture->dosio_attached = 1;
    return ESP_OK;
}

esp_err_t np2_fixture_attach_fdd(np2_fixture *fixture)
{
    FDDFILE fdd;

    if ((fixture == NULL) || !fixture->dosio_attached ||
        fixture->fdd_attached) {
        return ESP_ERR_INVALID_STATE;
    }
    if (fdd_set(0, NP2_FIXTURE_PATH, FTYPE_NONE, 1) != SUCCESS ||
        !fdd_diskready(0)) {
        return ESP_FAIL;
    }
    fdd = fddfile + 0;
    if ((fdd->type != DISKTYPE_BETA) ||
        (fdd->inf.xdf.tracks != NP2_FIXTURE_TRACKS) ||
        (fdd->inf.xdf.sectors != NP2_FIXTURE_SECTORS) ||
        (fdd->inf.xdf.n != NP2_FIXTURE_N) ||
        (fdd->inf.xdf.disktype != DISKTYPE_2HD) ||
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
        np2_dosio_detach_fixture();
        fixture->dosio_attached = 0;
    }
    if (fixture->mapped != NULL) {
        esp_partition_munmap(fixture->map_handle);
    }
    memset(fixture, 0, sizeof(*fixture));
}
