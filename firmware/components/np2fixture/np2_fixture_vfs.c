#include "np2_fixture.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mbedtls/sha256.h"

#include <compiler.h>
#include "common.h"
#include <diskimage/fddfile.h>

static const uint8_t np2_fixture_expected_sha256[NP2_FIXTURE_SHA256_SIZE] = {
    0x3b, 0x73, 0x66, 0x7d, 0x23, 0x56, 0x15, 0xe8,
    0x92, 0x05, 0xfb, 0xda, 0xb0, 0x4d, 0x3e, 0x6c,
    0xf9, 0xc2, 0xf9, 0xa1, 0xf3, 0xa1, 0xde, 0x82,
    0xcd, 0xb2, 0xb3, 0x86, 0x2a, 0xa3, 0x94, 0xb3,
};

static const np2_fixture_descriptor np2_fixture_default = {
    NP2_FIXTURE_PARTITION_LABEL,
    NP2_FIXTURE_PATH,
    NP2_FIXTURE_IMAGE_SIZE,
    np2_fixture_expected_sha256,
    NP2_FIXTURE_TRACKS,
    NP2_FIXTURE_SECTORS,
    NP2_FIXTURE_N,
    DISKTYPE_2HD,
};

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

const np2_fixture_descriptor *np2_fixture_default_descriptor(void)
{
    return &np2_fixture_default;
}

const char *np2_fixture_vfs_error_name(esp_err_t error)
{
    switch (error) {
        case ESP_ERR_INVALID_ARG:
            return "path_invalid";
        case ESP_ERR_NOT_FOUND:
            return "file_missing";
        case ESP_ERR_INVALID_SIZE:
            return "file_size_or_type_mismatch";
        case ESP_ERR_INVALID_CRC:
            return "sha256_mismatch";
        case ESP_OK:
            return "ok";
        default:
            return "file_io";
    }
}

esp_err_t np2_fixture_verify_vfs_file(
    const np2_fixture_descriptor *descriptor,
    const char *physical_path,
    uint8_t digest[NP2_FIXTURE_SHA256_SIZE])
{
    static uint8_t buffer[4096];
    struct stat status;
    mbedtls_sha256_context context;
    size_t remaining;
    int fd;
    int result;

    if (!np2_fixture_valid_descriptor(descriptor) ||
        (physical_path == NULL) || (physical_path[0] == '\0') ||
        (digest == NULL) || (strlen(physical_path) >= MAX_PATH)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (stat(physical_path, &status) != 0) {
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (!S_ISREG(status.st_mode) || (status.st_size < 0) ||
        ((uintmax_t)status.st_size != (uintmax_t)descriptor->image_size)) {
        return ESP_ERR_INVALID_SIZE;
    }

    fd = open(physical_path, O_RDONLY);
    if (fd < 0) {
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    mbedtls_sha256_init(&context);
    result = (mbedtls_sha256_starts(&context, 0) == 0) ? ESP_OK : ESP_FAIL;
    remaining = descriptor->image_size;
    while ((result == ESP_OK) && (remaining != 0U)) {
        const size_t request = (remaining < sizeof(buffer)) ?
            remaining : sizeof(buffer);
        ssize_t amount;

        do {
            amount = read(fd, buffer, request);
        } while ((amount < 0) && (errno == EINTR));
        if (amount <= 0 || (size_t)amount > remaining ||
            mbedtls_sha256_update(&context, buffer, (size_t)amount) != 0) {
            result = ESP_FAIL;
            break;
        }
        remaining -= (size_t)amount;
    }
    if ((result == ESP_OK) && (mbedtls_sha256_finish(&context, digest) != 0)) {
        result = ESP_FAIL;
    }
    if ((result == ESP_OK) &&
        (memcmp(digest, descriptor->expected_sha256,
                NP2_FIXTURE_SHA256_SIZE) != 0)) {
        result = ESP_ERR_INVALID_CRC;
    }
    mbedtls_sha256_free(&context);
    if ((close(fd) != 0) && (result == ESP_OK)) {
        result = ESP_FAIL;
    }
    return (esp_err_t)result;
}
