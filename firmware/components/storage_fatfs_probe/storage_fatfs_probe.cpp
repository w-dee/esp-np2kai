#include "storage_fatfs_probe/storage_fatfs_probe.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_vfs_fat.h"
#include "mbedtls/sha256.h"
#include "storage_fatfs/storage_fatfs.hpp"

namespace {

constexpr std::size_t kFixtureSize = 1261568;
constexpr char kFixtureSha256[] =
    "3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3";
constexpr std::size_t kHighFileSize = 2 * 1024 * 1024;
constexpr std::uint64_t kHighAddressMarkerOffset = 1024 * 1024;
constexpr char kHighAddressMarker[] = "STEP6A1-HIGH-ADDRESS-RAW-PROOF-v1";
constexpr char kHighFilePath[] = "/upload/step6a1-high.bin";
constexpr char kSmallFilePath[] = "/upload/step6a1-small.bin";
constexpr std::uint8_t kSmallFileData[] = {'s', 't', 'e', 'p', '6', 'a', '1', '\n'};

void fail(const char *reason)
{
    std::printf("STORAGEFATFS_RESULT=FAIL reason=%s\n", reason);
    std::fflush(stdout);
}

bool digest_hex(const std::uint8_t *digest, char *out, std::size_t capacity)
{
    if (digest == nullptr || out == nullptr || capacity < 65) return false;
    for (std::size_t index = 0; index < 32; ++index) {
        std::snprintf(out + index * 2, capacity - index * 2, "%02x", digest[index]);
    }
    out[64] = '\0';
    return true;
}

bool hash_fd(int fd, std::uint64_t expected_size, std::uint8_t digest[32])
{
    if (fd < 0 || digest == nullptr) return false;
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    bool success = mbedtls_sha256_starts(&context, 0) == 0;
    alignas(16) static std::array<std::uint8_t, 512> buffer{};
    std::uint64_t total = 0;
    while (success) {
        const ssize_t amount = ::pread(fd, buffer.data(), buffer.size(),
                                       static_cast<off_t>(total));
        if (amount < 0) {
            success = false;
            break;
        }
        if (amount == 0) break;
        total += static_cast<std::uint64_t>(amount);
        if ((total % (256 * 1024)) == 0) {
            std::printf("STORAGEFATFS_READ_PROGRESS bytes=%llu\n",
                        static_cast<unsigned long long>(total));
            std::fflush(stdout);
        }
        success = mbedtls_sha256_update(&context, buffer.data(),
                                        static_cast<std::size_t>(amount)) == 0;
    }
    if (success && total != expected_size) success = false;
    if (success) success = mbedtls_sha256_finish(&context, digest) == 0;
    mbedtls_sha256_free(&context);
    return success;
}

void fill_high_chunk(std::uint64_t offset, std::uint8_t *buffer, std::size_t length)
{
    const std::uint8_t marker[] = {0x53, 0x36, 0x41, 0x31};
    for (std::size_t index = 0; index < length; ++index) {
        buffer[index] = static_cast<std::uint8_t>((offset + index) ^
                                                   ((offset / 4096) * 29));
    }
    if (offset == 0 && length >= sizeof(marker)) {
        std::memcpy(buffer, marker, sizeof(marker));
    }
    if (offset == kHighAddressMarkerOffset &&
        length >= sizeof(kHighAddressMarker) - 1) {
        std::memcpy(buffer, kHighAddressMarker, sizeof(kHighAddressMarker) - 1);
    }
}

bool expected_high_digest(std::uint8_t digest[32])
{
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    bool success = mbedtls_sha256_starts(&context, 0) == 0;
    alignas(16) static std::array<std::uint8_t, 512> buffer{};
    for (std::uint64_t offset = 0; success && offset < kHighFileSize; offset += buffer.size()) {
        fill_high_chunk(offset, buffer.data(), buffer.size());
        success = mbedtls_sha256_update(&context, buffer.data(), buffer.size()) == 0;
    }
    if (success) success = mbedtls_sha256_finish(&context, digest) == 0;
    mbedtls_sha256_free(&context);
    return success;
}

bool read_storage_file(const storage::Storage &api, const char *path,
                       std::uint64_t expected_size, std::uint8_t digest[32])
{
    if (api.begin_read == nullptr || digest == nullptr) return false;
    storage::ReadSession session{};
    if (api.begin_read(api.context, path, &session) != storage::Error::Ok ||
        session.read == nullptr || session.close == nullptr) return false;

    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    bool success = mbedtls_sha256_starts(&context, 0) == 0;
    alignas(16) static std::array<std::uint8_t, 4096> buffer{};
    std::uint64_t offset = 0;
    while (success && offset < expected_size) {
        const std::size_t requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), expected_size - offset));
        std::size_t amount = 0;
        const storage::Error error = session.read(session.context, offset, buffer.data(),
                                                   requested, &amount);
        if (error != storage::Error::Ok || amount != requested) {
            success = false;
            break;
        }
        success = mbedtls_sha256_update(&context, buffer.data(), amount) == 0;
        offset += amount;
    }
    if (success) {
        std::size_t amount = 1;
        std::uint8_t extra = 0;
        success = session.read(session.context, expected_size, &extra, 1, &amount) ==
                      storage::Error::Ok && amount == 0;
    }
    if (session.close != nullptr) session.close(session.context);
    if (success) success = offset == expected_size &&
        mbedtls_sha256_finish(&context, digest) == 0;
    mbedtls_sha256_free(&context);
    return success;
}

bool read_small_file(const storage::Storage &api, const char *path,
                     const std::uint8_t *expected, std::size_t expected_size)
{
    std::uint8_t digest[32]{};
    if (!read_storage_file(api, path, expected_size, digest)) return false;
    alignas(16) static std::array<std::uint8_t, 64> data{};
    storage::ReadSession session{};
    if (api.begin_read(api.context, path, &session) != storage::Error::Ok) return false;
    std::size_t amount = 0;
    const storage::Error error = session.read(session.context, 0, data.data(),
                                              expected_size, &amount);
    session.close(session.context);
    return error == storage::Error::Ok && amount == expected_size &&
           std::memcmp(data.data(), expected, expected_size) == 0;
}

bool write_file(const storage::Storage &api, const char *path, const std::uint8_t *data,
                std::size_t size, bool replace)
{
    storage::WriteSession session{};
    if (api.begin_write(api.context, path, size, replace, &session) != storage::Error::Ok ||
        session.write == nullptr || session.commit == nullptr || session.abort == nullptr) {
        return false;
    }
    if (size != 0 && session.write(session.context, 0, data, size) != storage::Error::Ok) {
        session.abort(session.context);
        return false;
    }
    if (session.commit(session.context) != storage::Error::Ok) {
        session.abort(session.context);
        return false;
    }
    return true;
}

bool write_file_fresh_or_replace(const storage::Storage &api, const char *path,
                                 const std::uint8_t *data, std::size_t size)
{
    if (api.stat == nullptr) return false;
    storage::Metadata metadata{};
    const storage::Error status = api.stat(api.context, path, &metadata);
    if (status != storage::Error::Ok && status != storage::Error::NotFound) return false;
    if (status == storage::Error::Ok && metadata.type != storage::EntryType::File) return false;
    return write_file(api, path, data, size, status == storage::Error::Ok);
}

#if defined(STORAGE_FATFS_TEST_HOOKS)
void abort_session(storage::WriteSession *session)
{
    if (session != nullptr && session->abort != nullptr) session->abort(session->context);
}

bool begin_write_with_data(const storage::Storage &api, const char *path,
                           const std::uint8_t *data, std::size_t size, bool replace,
                           storage::WriteSession *session)
{
    if (session == nullptr || api.begin_write(api.context, path, size, replace, session) !=
            storage::Error::Ok ||
        session->write == nullptr || session->commit == nullptr || session->abort == nullptr) {
        return false;
    }
    if (size != 0 && session->write(session->context, 0, data, size) != storage::Error::Ok) {
        abort_session(session);
        return false;
    }
    return true;
}

bool has_staging_suffix(const char *suffix)
{
    DIR *directory = ::opendir(storage_fatfs::kStagingRoot);
    if (directory == nullptr) return false;
    bool found = false;
    errno = 0;
    const dirent *entry = nullptr;
    while ((entry = ::readdir(directory)) != nullptr) {
        const std::string_view name(entry->d_name);
        if (name.size() >= std::strlen(suffix) &&
            name.substr(name.size() - std::strlen(suffix)) == suffix) {
            found = true;
            break;
        }
    }
    const int read_error = errno;
    const int close_result = ::closedir(directory);
    return found && read_error == 0 && close_result == 0;
}

bool staging_suffix_absent(const char *suffix)
{
    DIR *directory = ::opendir(storage_fatfs::kStagingRoot);
    if (directory == nullptr) return false;
    bool found = false;
    errno = 0;
    const dirent *entry = nullptr;
    while ((entry = ::readdir(directory)) != nullptr) {
        const std::string_view name(entry->d_name);
        if (name.size() >= std::strlen(suffix) &&
            name.substr(name.size() - std::strlen(suffix)) == suffix) {
            found = true;
            break;
        }
    }
    const int read_error = errno;
    const int close_result = ::closedir(directory);
    return !found && read_error == 0 && close_result == 0;
}

bool remove_staging_suffix(const char *suffix)
{
    DIR *directory = ::opendir(storage_fatfs::kStagingRoot);
    if (directory == nullptr) return false;
    bool success = true;
    errno = 0;
    const dirent *entry = nullptr;
    while ((entry = ::readdir(directory)) != nullptr) {
        const std::string_view name(entry->d_name);
        if (name.size() < std::strlen(suffix) ||
            name.substr(name.size() - std::strlen(suffix)) != suffix) {
            continue;
        }
        static char path[storage_fatfs::kPhysicalPathBytes]{};
        if (std::snprintf(path, sizeof(path), "%s/%s", storage_fatfs::kStagingRoot,
                          entry->d_name) >= static_cast<int>(sizeof(path)) ||
            ::unlink(path) != 0) {
            success = false;
            break;
        }
    }
    const int read_error = errno;
    const int close_result = ::closedir(directory);
    return success && read_error == 0 && close_result == 0;
}

bool run_replacement_error_checks(storage_fatfs::StorageFatfs &storage)
{
    const storage::Storage api = storage.api();
    constexpr char kPath[] = "/upload/step6a1-replacement.bin";
    const std::uint8_t old_data[] = {'o', 'l', 'd', '\n'};
    const std::uint8_t new_data[] = {'n', 'e', 'w', '\n'};

    if (!remove_staging_suffix(".bak") || !remove_staging_suffix(".tmp") ||
        !staging_suffix_absent(".tmp") ||
        !write_file_fresh_or_replace(api, kPath, old_data, sizeof(old_data))) {
        return false;
    }

    storage_fatfs::TestHooks hooks{};
    storage.set_test_hooks(&hooks);

    hooks.fail_rename_call_1 = 1;
    storage::WriteSession session{};
    if (!begin_write_with_data(api, kPath, new_data, sizeof(new_data), true, &session) ||
        session.commit(session.context) != storage::Error::CommitFailed ||
        !read_small_file(api, kPath, old_data, sizeof(old_data)) ||
        !staging_suffix_absent(".tmp")) {
        abort_session(&session);
        storage.set_test_hooks(nullptr);
        return false;
    }
    std::printf("STORAGEFATFS_REPLACEMENT backup_rename_failure=preserved\n");

    hooks = storage_fatfs::TestHooks{};
    hooks.fail_rename_call_1 = 2;
    session = storage::WriteSession{};
    if (!begin_write_with_data(api, kPath, new_data, sizeof(new_data), true, &session) ||
        session.commit(session.context) != storage::Error::CommitFailed ||
        !read_small_file(api, kPath, old_data, sizeof(old_data)) ||
        has_staging_suffix(".bak")) {
        abort_session(&session);
        storage.set_test_hooks(nullptr);
        return false;
    }
    std::printf("STORAGEFATFS_REPLACEMENT install_failure_rollback=restored\n");

    hooks = storage_fatfs::TestHooks{};
    hooks.fail_rename_call_1 = 2;
    hooks.fail_rename_call_2 = 3;
    session = storage::WriteSession{};
    if (!begin_write_with_data(api, kPath, new_data, sizeof(new_data), true, &session) ||
        session.commit(session.context) != storage::Error::CommitFailed) {
        abort_session(&session);
        storage.set_test_hooks(nullptr);
        return false;
    }
    storage::Metadata metadata{};
    if (api.stat(api.context, kPath, &metadata) != storage::Error::NotFound ||
        !has_staging_suffix(".bak")) {
        abort_session(&session);
        storage.set_test_hooks(nullptr);
        return false;
    }
    std::printf("STORAGEFATFS_REPLACEMENT rollback_failure=backup_preserved\n");
    abort_session(&session);
    if (!read_small_file(api, kPath, old_data, sizeof(old_data)) ||
        has_staging_suffix(".bak")) {
        storage.set_test_hooks(nullptr);
        return false;
    }

    hooks = storage_fatfs::TestHooks{};
    hooks.fail_unlink_call = 1;
    session = storage::WriteSession{};
    if (!begin_write_with_data(api, kPath, new_data, sizeof(new_data), true, &session) ||
        session.commit(session.context) != storage::Error::Ok ||
        !read_small_file(api, kPath, new_data, sizeof(new_data)) ||
        !has_staging_suffix(".bak")) {
        abort_session(&session);
        storage.set_test_hooks(nullptr);
        return false;
    }
    std::printf("STORAGEFATFS_REPLACEMENT backup_cleanup_failure=content_committed\n");
    storage.set_test_hooks(nullptr);
    if (!remove_staging_suffix(".bak")) return false;
    if (!staging_suffix_absent(".tmp")) return false;
    std::printf("STORAGEFATFS_STAGING_CLEANUP failed_write_tmp_absent=1\n");
    return true;
}

bool run_mount_cleanup_checks(storage_fatfs::MountProvider &provider,
                              storage_fatfs::StorageFatfs &storage)
{
    if (storage.unmount() != ESP_OK) return false;
    storage_fatfs::TestHooks hooks{};
    storage.set_test_hooks(&hooks);
    hooks.fail_mount_initialization = true;
    provider.set_test_unmount_failure(false);
    if (storage.mount() != ESP_FAIL || storage.mounted() ||
        provider.wl_handle() != WL_INVALID_HANDLE) {
        storage.set_test_hooks(nullptr);
        return false;
    }
    std::printf("STORAGEFATFS_MOUNT_CLEANUP unmount_success=clean\n");

    provider.set_test_unmount_failure(true);
    if (storage.mount() != ESP_FAIL || !storage.mounted() ||
        provider.wl_handle() == WL_INVALID_HANDLE) {
        provider.set_test_unmount_failure(false);
        storage.set_test_hooks(nullptr);
        return false;
    }
    std::printf("STORAGEFATFS_MOUNT_CLEANUP unmount_failure=state_retained\n");
    provider.set_test_unmount_failure(false);
    if (storage.unmount() != ESP_OK || storage.mounted() ||
        provider.wl_handle() != WL_INVALID_HANDLE) {
        storage.set_test_hooks(nullptr);
        return false;
    }
    hooks.fail_mount_initialization = false;
    if (storage.mount() != ESP_OK) {
        storage.set_test_hooks(nullptr);
        return false;
    }
    storage.set_test_hooks(nullptr);
    return true;
}
#endif

bool write_high_file(const storage::Storage &api, bool *created)
{
    if (created == nullptr || api.stat == nullptr) return false;
    storage::Metadata metadata{};
    const storage::Error status = api.stat(api.context, kHighFilePath, &metadata);
    if (status == storage::Error::Ok) {
        *created = false;
        return metadata.type == storage::EntryType::File && metadata.size_bytes == kHighFileSize;
    }
    if (status != storage::Error::NotFound) return false;

    storage::WriteSession session{};
    if (api.begin_write(api.context, kHighFilePath, kHighFileSize, false, &session) !=
            storage::Error::Ok ||
        session.write == nullptr || session.commit == nullptr || session.abort == nullptr) {
        return false;
    }
    alignas(16) static std::array<std::uint8_t, 4096> buffer{};
    for (std::uint64_t offset = 0; offset < kHighFileSize; offset += buffer.size()) {
        fill_high_chunk(offset, buffer.data(), buffer.size());
        if (session.write(session.context, offset, buffer.data(), buffer.size()) !=
            storage::Error::Ok) {
            session.abort(session.context);
            return false;
        }
    }
    if (session.commit(session.context) != storage::Error::Ok) {
        session.abort(session.context);
        return false;
    }
    *created = true;
    return true;
}

bool check_listing(const storage::Storage &api)
{
    storage::DirectoryEntry entries[2]{};
    std::size_t count = 0;
    bool more = false;
    if (api.list_page(api.context, "/", "", 2, entries, 2, &count, &more) !=
            storage::Error::Ok || count != 2 || !more ||
        std::strcmp(entries[0].name, "long") != 0 ||
        std::strcmp(entries[1].name, "seed") != 0) {
        return false;
    }
    if (api.list_page(api.context, "/", entries[1].name, 2, entries, 2, &count, &more) !=
            storage::Error::Ok || count != 1 || more ||
        std::strcmp(entries[0].name, "upload") != 0) {
        return false;
    }
    return true;
}

bool run_provider_checks(storage_fatfs::StorageFatfs &storage)
{
    const storage::Storage api = storage.api();
    storage::Metadata metadata{};

    if (api.stat(api.context, "/", &metadata) != storage::Error::Ok ||
        metadata.type != storage::EntryType::Directory || !check_listing(api)) {
        return false;
    }
    if (api.stat(api.context, "/fixtures", &metadata) != storage::Error::NotFound ||
        api.stat(api.context, "/.np2-staging", &metadata) != storage::Error::NotFound ||
        api.stat(api.context, "/bad:name", &metadata) != storage::Error::InvalidPath ||
        api.stat(api.context, "/../secret", &metadata) != storage::Error::InvalidPath) {
        return false;
    }

    const std::uint8_t seed[37] = {
        0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
        0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3,
        0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd,
        0xbe, 0xbf, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4,
    };
    if (!read_small_file(api, "/seed/existing.bin", seed, sizeof(seed))) return false;

    const std::uint8_t utf8_file[] = {'u', 't', 'f', '8', '\n'};
    if (!read_small_file(api, "/long/utf8-long.txt", utf8_file, sizeof(utf8_file))) return false;

    storage::WriteSession collision{};
    if (api.begin_write(api.context, "/seed/EXISTING.BIN", 1, false, &collision) !=
        storage::Error::AlreadyExists) return false;

    if (!write_file_fresh_or_replace(api, kSmallFilePath, kSmallFileData,
                                     sizeof(kSmallFileData))) return false;
    if (!write_file(api, kSmallFilePath, kSmallFileData, sizeof(kSmallFileData), true)) return false;
    if (!write_file_fresh_or_replace(api, "/upload/é.txt", utf8_file,
                                     sizeof(utf8_file)) ||
        !read_small_file(api, "/upload/é.txt", utf8_file, sizeof(utf8_file))) return false;
    if (!write_file_fresh_or_replace(api, "/upload/zero.bin", nullptr, 0)) return false;

    storage::WriteSession aborted{};
    if (api.begin_write(api.context, "/seed/existing.bin", sizeof(seed), true, &aborted) !=
            storage::Error::Ok ||
        aborted.write(aborted.context, 0, kSmallFileData, sizeof(kSmallFileData)) !=
            storage::Error::Ok) {
        if (aborted.abort != nullptr) aborted.abort(aborted.context);
        return false;
    }
    aborted.abort(aborted.context);
    if (!read_small_file(api, "/seed/existing.bin", seed, sizeof(seed))) return false;
#if defined(STORAGE_FATFS_TEST_HOOKS)
    if (!staging_suffix_absent(".tmp")) return false;
    std::printf("STORAGEFATFS_STAGING_CLEANUP abort_tmp_absent=1\n");
#endif

#if defined(STORAGE_FATFS_CI_BOUNDED)
    std::printf("STORAGEFATFS_PROFILE bounded-ci\n");
#else
    bool created = false;
    if (!write_high_file(api, &created)) return false;
    std::uint8_t high_digest[32]{};
    std::uint8_t expected_digest[32]{};
    if (!read_storage_file(api, kHighFilePath, kHighFileSize, high_digest) ||
        !expected_high_digest(expected_digest) ||
        std::memcmp(high_digest, expected_digest, sizeof(high_digest)) != 0) {
        return false;
    }
    char high_hex[65]{};
    digest_hex(high_digest, high_hex, sizeof(high_hex));
    std::printf("STORAGEFATFS_HIGH_FILE created=%d size=%u sha256=%s\n",
                created ? 1 : 0, static_cast<unsigned>(kHighFileSize), high_hex);
#endif
    return true;
}

} // namespace

extern "C" esp_err_t storage_fatfs_probe_run(void)
{
    static storage_fatfs::MountProvider provider;
    static storage_fatfs::StorageFatfs storage(provider);
    esp_err_t result = storage.mount();
    if (result != ESP_OK) {
        fail("mount");
        return result;
    }
    std::printf("STORAGEFATFS_MOUNT base=%s partition=%s format_if_mount_failed=0\n",
                storage_fatfs::kMountPath, storage_fatfs::kPartitionLabel);

    std::uint64_t total_bytes = 0;
    std::uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(storage_fatfs::kMountPath, &total_bytes, &free_bytes) != ESP_OK) {
        fail("capacity");
        storage.unmount();
        return ESP_FAIL;
    }
    std::printf("STORAGEFATFS_CAPACITY total_bytes=%llu free_bytes=%llu\n",
                static_cast<unsigned long long>(total_bytes),
                static_cast<unsigned long long>(free_bytes));

    const int fixture_fd = ::open(storage_fatfs::kFixturePath, O_RDONLY, 0);
    struct stat fixture_stat{};
    std::uint8_t fixture_digest[32]{};
    const bool fixture_ok = fixture_fd >= 0 && ::fstat(fixture_fd, &fixture_stat) == 0 &&
        S_ISREG(fixture_stat.st_mode) &&
        static_cast<std::uint64_t>(fixture_stat.st_size) == kFixtureSize &&
        hash_fd(fixture_fd, kFixtureSize, fixture_digest);
    if (fixture_fd >= 0) ::close(fixture_fd);
    char fixture_hex[65]{};
    digest_hex(fixture_digest, fixture_hex, sizeof(fixture_hex));
    std::printf("STORAGEFATFS_FIXTURE size=%llu sha256=%s\n",
                static_cast<unsigned long long>(fixture_stat.st_size), fixture_hex);
    if (!fixture_ok || std::strcmp(fixture_hex, kFixtureSha256) != 0) {
        fail("fixture");
        storage.unmount();
        return ESP_FAIL;
    }

    if (!run_provider_checks(storage)) {
        fail("provider_checks");
        storage.unmount();
        return ESP_FAIL;
    }
    std::printf("STORAGEFATFS_PROVIDER_CHECKS pass=1\n");

#if defined(STORAGE_FATFS_TEST_HOOKS)
    if (!run_replacement_error_checks(storage)) {
        fail("replacement_error_checks");
        storage.unmount();
        return ESP_FAIL;
    }
    std::printf("STORAGEFATFS_STAGING_CLEANUP=PASS "
                "failed_write_tmp_absent=1 abort_tmp_absent=1\n");
    if (!run_mount_cleanup_checks(provider, storage)) {
        fail("mount_cleanup_checks");
        storage.unmount();
        return ESP_FAIL;
    }
#endif

    if (storage.unmount() != ESP_OK || storage.mount() != ESP_OK) {
        fail("remount");
        return ESP_FAIL;
    }
    const storage::Storage remounted_api = storage.api();
#if defined(STORAGE_FATFS_CI_BOUNDED)
    if (!read_small_file(remounted_api, kSmallFilePath, kSmallFileData,
                         sizeof(kSmallFileData))) {
        fail("bounded_persistence");
        storage.unmount();
        return ESP_FAIL;
    }
    std::printf("STORAGEFATFS_REMOUNT persisted=1 sha256_match=1 path=%s\n",
                kSmallFilePath);
#else
    std::uint8_t persisted_digest[32]{};
    std::uint8_t expected_digest[32]{};
    if (!read_storage_file(remounted_api, kHighFilePath, kHighFileSize, persisted_digest) ||
        !expected_high_digest(expected_digest) ||
        std::memcmp(persisted_digest, expected_digest, sizeof(persisted_digest)) != 0) {
        fail("same_process_remount");
        storage.unmount();
        return ESP_FAIL;
    }
    std::printf("STORAGEFATFS_REMOUNT persisted=1 sha256_match=1\n");
#endif
    if (storage.unmount() != ESP_OK) {
        fail("unmount");
        return ESP_FAIL;
    }
    std::printf("STORAGEFATFS_RESULT=PASS\n");
    std::fflush(stdout);
    return ESP_OK;
}
