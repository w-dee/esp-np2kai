#include "np2dosio_probe/np2dosio_probe.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <compiler.h>
#include <common.h>
#ifndef BRESULT
#define BRESULT UINT
#endif
extern "C" {
#include <dosio.h>
}
#include <mbedtls/sha256.h>
#include "np2_fixture.h"
#include <np2host/dosio_esp.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "storage_fatfs/storage_fatfs.hpp"

namespace {

constexpr std::size_t kFixtureSize = 1261568;
constexpr char kFixtureSha256[] =
    "3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3";
constexpr char kLogicalPath[] = "./np2test-fd1232.hdm";
constexpr char kPhysicalPath[] = "/persist/fixtures/np2test-fd1232.hdm";
constexpr char kMissingPhysicalPath[] = "/persist/fixtures/missing.hdm";
constexpr char kWrongSizePhysicalPath[] =
    "/persist/fixtures/.np2-a1-wrong-size.hdm";
constexpr std::array<std::uint8_t, 16> kAtZero = {
    0xfa, 0xfc, 0x31, 0xc0, 0x8e, 0xd8, 0xb8, 0x00,
    0x28, 0x8e, 0xd0, 0xbc, 0x00, 0x10, 0xb8, 0x00,
};
constexpr std::array<std::uint8_t, 16> kAtOne = {
    0xfc, 0x31, 0xc0, 0x8e, 0xd8, 0xb8, 0x00, 0x28,
    0x8e, 0xd0, 0xbc, 0x00, 0x10, 0xb8, 0x00, 0x29,
};
constexpr std::array<std::uint8_t, 16> kAt512Boundary = {
    0x55, 0xaa, 0xbd, 0x01, 0x01, 0xb8, 0x34, 0x12,
    0x89, 0xc3, 0x81, 0xfb, 0x34, 0x12, 0x74, 0x03,
};
constexpr std::array<std::uint8_t, 16> kZeros{};

void fail(const char *reason)
{
    std::printf("NP2DOSIO_RESULT=FAIL reason=%s\n", reason);
    std::fflush(stdout);
}

bool digest_hex(const std::uint8_t digest[32], char out[65])
{
    if (digest == nullptr || out == nullptr) return false;
    for (std::size_t index = 0; index < 32; ++index) {
        std::snprintf(out + index * 2, 65 - index * 2, "%02x", digest[index]);
    }
    out[64] = '\0';
    return true;
}

void diag_stack(const char *point)
{
    const UBaseType_t high_water = uxTaskGetStackHighWaterMark(nullptr);
    std::printf("NP2DOSIO_DIAG stack point=%s configured_stack_bytes=%d "
                "high_water_bytes=%lu semantics=bytes\n",
                point, CONFIG_ESP_MAIN_TASK_STACK_SIZE,
                static_cast<unsigned long>(high_water));
    std::fflush(stdout);
}

bool attach_valid_mapping()
{
    return np2_dosio_attach_vfs_file(kLogicalPath, kPhysicalPath) != 0;
}

bool create_verification_file(const char *path, std::size_t size)
{
    const int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0666);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0) {
        if (fd >= 0) close(fd);
        return false;
    }
    return close(fd) == 0;
}

bool expect_vfs_verification_failure(const np2_fixture_descriptor *descriptor,
                                    const char *path, esp_err_t expected,
                                    const char *marker)
{
    std::array<std::uint8_t, NP2_FIXTURE_SHA256_SIZE> digest{};
    const esp_err_t error = np2_fixture_verify_vfs_file(
        descriptor, path, digest.data());
    const bool no_dosio_attachment = file_open_rb(kLogicalPath) == FILEH_INVALID;

    if (error != expected || !no_dosio_attachment) return false;
    std::printf("%s=PASS reason=%s\n", marker,
                np2_fixture_vfs_error_name(error));
    return true;
}

bool run_fixture_verification_tests()
{
    const np2_fixture_descriptor *descriptor;
    std::array<std::uint8_t, NP2_FIXTURE_SHA256_SIZE> digest_bytes{};
    char digest[65]{};
    esp_err_t error;

    descriptor = np2_fixture_default_descriptor();
    error = np2_fixture_verify_vfs_file(
        descriptor, kPhysicalPath, digest_bytes.data());
    if (error != ESP_OK || descriptor->image_size != kFixtureSize ||
        !digest_hex(digest_bytes.data(), digest) ||
        std::strcmp(digest, kFixtureSha256) != 0) {
        return false;
    }
    std::printf("NP2FIXTURE_VFS_VERIFY=PASS logical=%s physical=%s size=%u "
                "sha256=%s read_only=1\n",
                descriptor->logical_path, kPhysicalPath,
                (unsigned)descriptor->image_size, digest);

    unlink(kWrongSizePhysicalPath);
    if (!create_verification_file(kWrongSizePhysicalPath, 1U) ||
        !expect_vfs_verification_failure(
            descriptor, kWrongSizePhysicalPath, ESP_ERR_INVALID_SIZE,
            "NP2FIXTURE_VFS_WRONG_SIZE")) {
        unlink(kWrongSizePhysicalPath);
        return false;
    }
    std::array<std::uint8_t, NP2_FIXTURE_SHA256_SIZE> wrong_sha{};
    np2_fixture_descriptor wrong_sha_descriptor = *descriptor;
    wrong_sha_descriptor.expected_sha256 = wrong_sha.data();
    if (!expect_vfs_verification_failure(
            &wrong_sha_descriptor, kPhysicalPath, ESP_ERR_INVALID_CRC,
            "NP2FIXTURE_VFS_WRONG_SHA")) {
        unlink(kWrongSizePhysicalPath);
        return false;
    }
    unlink(kWrongSizePhysicalPath);
    if (!expect_vfs_verification_failure(
            descriptor, kMissingPhysicalPath, ESP_ERR_NOT_FOUND,
            "NP2FIXTURE_VFS_MISSING")) {
        return false;
    }
    return true;
}

bool run_attach_tests()
{
    static std::array<char, MAX_PATH + 1> overlong{};
    overlong.fill('x');
    overlong.back() = '\0';

    if (np2_dosio_attach_vfs_file("", kPhysicalPath) != 0 ||
        np2_dosio_attach_vfs_file(kLogicalPath, "") != 0 ||
        np2_dosio_attach_vfs_file(overlong.data(), kPhysicalPath) != 0 ||
        np2_dosio_attach_vfs_file(kLogicalPath, overlong.data()) != 0 ||
        !attach_valid_mapping() ||
        np2_dosio_attach_vfs_file("./other.hdm", kPhysicalPath) != 0) {
        return false;
    }
    std::printf("NP2DOSIO_ATTACH=PASS\n");
    return true;
}

bool run_attr_tests()
{
    if (file_attr(kLogicalPath) != FILEATTR_READONLY ||
        file_attr("./other.hdm") != -1) {
        return false;
    }
    np2_dosio_detach_vfs_file();
    if (np2_dosio_attach_vfs_file(kLogicalPath, kMissingPhysicalPath) == 0 ||
        file_attr(kLogicalPath) != -1) {
        return false;
    }
    np2_dosio_detach_vfs_file();
    if (np2_dosio_attach_vfs_file(kLogicalPath, storage_fatfs::kFixtureRoot) == 0 ||
        file_attr(kLogicalPath) != -1) {
        return false;
    }
    np2_dosio_detach_vfs_file();
    if (!attach_valid_mapping() || file_attr(kLogicalPath) != FILEATTR_READONLY) {
        return false;
    }
    std::printf("NP2DOSIO_ATTR=PASS\n");
    return true;
}

bool run_open_size_tests()
{
    FILEH handle = file_open_rb(kLogicalPath);
    if (handle == FILEH_INVALID ||
        file_open_rb(kLogicalPath) != FILEH_INVALID ||
        file_close(handle) != 0 ||
        file_open_rb("./other.hdm") != FILEH_INVALID) {
        if (handle != FILEH_INVALID) file_close(handle);
        return false;
    }

    np2_dosio_detach_vfs_file();
    if (np2_dosio_attach_vfs_file(kLogicalPath, kMissingPhysicalPath) == 0 ||
        file_open_rb(kLogicalPath) != FILEH_INVALID) {
        return false;
    }
    np2_dosio_detach_vfs_file();
    if (np2_dosio_attach_vfs_file(kLogicalPath, storage_fatfs::kFixtureRoot) == 0 ||
        file_open_rb(kLogicalPath) != FILEH_INVALID) {
        return false;
    }
    np2_dosio_detach_vfs_file();
    if (!attach_valid_mapping()) return false;

    handle = file_open_rb(kLogicalPath);
    if (handle == FILEH_INVALID || file_getsize(handle) != (FILELEN)kFixtureSize ||
        file_close(handle) != 0) {
        if (handle != FILEH_INVALID) file_close(handle);
        return false;
    }
    std::printf("NP2DOSIO_OPEN=PASS\n");
    std::printf("NP2DOSIO_SIZE=PASS bytes=%lu\n",
                (unsigned long)kFixtureSize);
    return true;
}

template <std::size_t N>
bool read_at(FILEH handle, FILEPOS offset, const std::array<std::uint8_t, N> &expected)
{
    std::array<std::uint8_t, N> actual{};
    return file_seek(handle, offset, FSEEK_SET) == offset &&
           file_read(handle, actual.data(), static_cast<UINT>(actual.size())) ==
               actual.size() &&
           std::memcmp(actual.data(), expected.data(), expected.size()) == 0;
}

bool run_read_tests()
{
    FILEH handle = file_open_rb(kLogicalPath);
    if (handle == FILEH_INVALID ||
        !read_at(handle, 0, kAtZero) ||
        !read_at(handle, 1, kAtOne) ||
        !read_at(handle, 510, kAt512Boundary) ||
        !read_at(handle, 4096, kZeros) ||
        !read_at(handle, static_cast<FILEPOS>(kFixtureSize / 2), kZeros) ||
        !read_at(handle, static_cast<FILEPOS>(kFixtureSize - kZeros.size()), kZeros)) {
        if (handle != FILEH_INVALID) file_close(handle);
        return false;
    }
    if (file_close(handle) != 0) return false;
    std::printf("NP2DOSIO_READ=PASS offsets=0,1,510,4096,middle,near_eof\n");
    return true;
}

bool run_seek_eof_tests()
{
    FILEH handle = file_open_rb(kLogicalPath);
    std::array<std::uint8_t, 16> tail{};
    std::array<std::uint8_t, 16> crossing{};
    const FILEPOS size = static_cast<FILEPOS>(kFixtureSize);

    if (handle == FILEH_INVALID ||
        file_seek(handle, 0, FSEEK_SET) != 0 ||
        file_seek(handle, 513, FSEEK_SET) != 513 ||
        file_seek(handle, -16, FSEEK_END) != size - 16 ||
        file_read(handle, tail.data(), tail.size()) != tail.size() ||
        tail != kZeros ||
        file_seek(handle, 0, FSEEK_END) != size ||
        file_read(handle, tail.data(), 1) != 0 ||
        file_seek(handle, -(size + 1), FSEEK_END) != (FILEPOS)-1 ||
        file_read(handle, tail.data(), 1) != 0 ||
        file_seek(handle, size + 1, FSEEK_SET) != (FILEPOS)-1 ||
        file_read(handle, tail.data(), 1) != 0 ||
        file_seek(handle, size - 8, FSEEK_SET) != size - 8 ||
        file_read(handle, crossing.data(), crossing.size()) != 8 ||
        std::memcmp(crossing.data(), kZeros.data(), 8) != 0 ||
        file_read(handle, crossing.data(), 1) != 0 ||
        file_close(handle) != 0) {
        if (handle != FILEH_INVALID) file_close(handle);
        return false;
    }
    std::printf("NP2DOSIO_SEEK=PASS set=0,set_nonzero,end_negative,end_zero\n");
    std::printf("NP2DOSIO_EOF=PASS exact=0 crossing=8 at_eof=0\n");
    return true;
}

bool run_close_detach_tests()
{
    FILEH handle = file_open_rb(kLogicalPath);
    if (handle == FILEH_INVALID) return false;
    np2_dosio_detach_vfs_file();
    if (file_open_rb(kLogicalPath) != FILEH_INVALID ||
        !attach_valid_mapping()) {
        return false;
    }
    handle = file_open_rb(kLogicalPath);
    if (handle == FILEH_INVALID || file_close(handle) != 0) return false;
    np2_dosio_detach_vfs_file();
    if (file_attr(kLogicalPath) != -1 ||
        file_open_rb(kLogicalPath) != FILEH_INVALID ||
        !attach_valid_mapping()) {
        return false;
    }
    handle = file_open_rb(kLogicalPath);
    if (handle == FILEH_INVALID || file_close(handle) != 0) return false;
    std::printf("NP2DOSIO_CLOSE=PASS reopen=1\n");
    std::printf("NP2DOSIO_DETACH=PASS active_safe=1 reattach=1\n");
    return true;
}

bool hash_through_dosio(char actual_hex[65])
{
    static std::array<std::uint8_t, 4096> buffer{};
    std::uint8_t digest[32]{};
    FILEH handle = file_open_rb(kLogicalPath);
    mbedtls_sha256_context context;
    std::size_t total = 0;
    bool success;

    if (handle == FILEH_INVALID || file_getsize(handle) != (FILELEN)kFixtureSize) {
        if (handle != FILEH_INVALID) file_close(handle);
        return false;
    }
    mbedtls_sha256_init(&context);
    success = mbedtls_sha256_starts(&context, 0) == 0;
    while (success && total < kFixtureSize) {
        const UINT requested = static_cast<UINT>(
            (kFixtureSize - total < buffer.size()) ? kFixtureSize - total : buffer.size());
        const UINT amount = file_read(handle, buffer.data(), requested);
        if (amount != requested) {
            success = false;
            break;
        }
        success = mbedtls_sha256_update(&context, buffer.data(), amount) == 0;
        total += amount;
    }
    if (success) success = file_read(handle, buffer.data(), 1) == 0;
    if (success) success = file_close(handle) == 0;
    else file_close(handle);
    if (success) success = total == kFixtureSize &&
        mbedtls_sha256_finish(&context, digest) == 0;
    mbedtls_sha256_free(&context);
    if (!success || !digest_hex(digest, actual_hex)) return false;
    return std::strcmp(actual_hex, kFixtureSha256) == 0;
}

bool run_all_tests()
{
    char actual_sha256[65]{};

    if (!run_fixture_verification_tests() || !run_attach_tests()) return false;
    diag_stack("post_attach_matrix");
    if (!run_attr_tests() || !run_open_size_tests() || !run_read_tests() ||
        !run_seek_eof_tests() || !run_close_detach_tests()) {
        return false;
    }
    diag_stack("before_full_sha");
    const bool hash_passed = hash_through_dosio(actual_sha256);
    diag_stack("after_full_sha");
    if (!hash_passed) return false;
    std::printf("NP2DOSIO_SHA256 expected=%s actual=%s path=dosio\n",
                kFixtureSha256, actual_sha256);
    return true;
}

} // namespace

extern "C" esp_err_t np2dosio_probe_run(void)
{
    diag_stack("entry");
    storage_fatfs::MountProvider provider;
    storage_fatfs::StorageFatfs storage(provider);
    if (storage.mount() != ESP_OK) {
        fail("storage_mount");
        return ESP_FAIL;
    }
    diag_stack("post_mount");
    const bool tests_passed = run_all_tests();
    np2_dosio_detach_vfs_file();
    const esp_err_t unmount_result = storage.unmount();
    if (!tests_passed) {
        fail("vfs_matrix");
        return ESP_FAIL;
    }
    if (unmount_result != ESP_OK) {
        fail("storage_unmount");
        return ESP_FAIL;
    }
    std::printf("NP2DOSIO_VFS=PASS provider=POSIX_VFS mounted_by=np2dosio_probe storage_provider=storage_fatfs\n");
    std::printf("NP2DOSIO_RESULT=PASS\n");
    std::fflush(stdout);
    return ESP_OK;
}
