#include "storage_fatfs/storage_fatfs.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"

namespace storage_fatfs {
namespace {

constexpr std::size_t kMaxDirectoryEntries = 64;
constexpr std::size_t kFatIoChunkBytes = 512;
constexpr char kLogTag[] = "storage_fatfs";

bool is_fat_invalid(unsigned char value)
{
    switch (value) {
    case ':':
    case '*':
    case '?':
    case '"':
    case '<':
    case '>':
    case '|':
        return true;
    default:
        return false;
    }
}

bool copy_string(char *out, std::size_t capacity, const char *value)
{
    if (out == nullptr || value == nullptr) return false;
    const std::size_t length = std::strlen(value);
    if (length >= capacity) return false;
    std::memcpy(out, value, length + 1);
    return true;
}

bool stat_path(const char *path, struct stat *result)
{
    return path != nullptr && result != nullptr && ::stat(path, result) == 0;
}

} // namespace

esp_err_t MountProvider::mount()
{
    if (mounted_) return ESP_ERR_INVALID_STATE;

    esp_vfs_fat_mount_config_t config = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    config.format_if_mount_failed = false;
    config.max_files = 8;
    config.allocation_unit_size = 0;
    config.disk_status_check_enable = false;
    config.use_one_fat = false;

    wl_handle_t handle = WL_INVALID_HANDLE;
    const esp_err_t result = esp_vfs_fat_spiflash_mount_rw_wl(
        kMountPath, kPartitionLabel, &config, &handle);
    if (result != ESP_OK) return result;

    wl_handle_ = handle;
    mounted_ = true;
    return ESP_OK;
}

esp_err_t MountProvider::unmount()
{
    if (!mounted_) return ESP_ERR_INVALID_STATE;
#if defined(STORAGE_FATFS_TEST_HOOKS)
    if (test_unmount_failure_) {
        errno = EIO;
        return ESP_FAIL;
    }
#endif
    const esp_err_t result = esp_vfs_fat_spiflash_unmount_rw_wl(kMountPath, wl_handle_);
    if (result != ESP_OK) return result;
    wl_handle_ = WL_INVALID_HANDLE;
    mounted_ = false;
    return ESP_OK;
}

esp_err_t StorageFatfs::mount()
{
    if (provider_.mounted()) return ESP_ERR_INVALID_STATE;
    esp_err_t result = provider_.mount();
    if (result != ESP_OK) return result;

    const auto cleanup_mount_failure = [this](esp_err_t failure) {
        const esp_err_t cleanup = provider_.unmount();
        return cleanup == ESP_OK ? failure : cleanup;
    };

#if defined(STORAGE_FATFS_TEST_HOOKS)
    if (test_hooks_ != nullptr && test_hooks_->fail_mount_initialization) {
        return cleanup_mount_failure(ESP_FAIL);
    }
#endif

    if (roots_.file_transfer_root == nullptr ||
        !ensure_directory(roots_.file_transfer_root) ||
        (roots_.fixture_root != nullptr && !ensure_directory(roots_.fixture_root)) ||
        roots_.staging_root == nullptr ||
        !ensure_directory(roots_.staging_root)) {
        return cleanup_mount_failure(ESP_FAIL);
    }

    DIR *directory = ::opendir(roots_.staging_root);
    if (directory == nullptr) {
        return cleanup_mount_failure(ESP_FAIL);
    }
    errno = 0;
    const dirent *entry = nullptr;
    while ((entry = ::readdir(directory)) != nullptr) {
        const std::string_view name(entry->d_name);
        if (name.size() < 5 || name.substr(name.size() - 4) != ".tmp") continue;
        static char orphan[kPhysicalPathBytes]{};
        if (std::snprintf(orphan, sizeof(orphan), "%s/%s", roots_.staging_root,
                          entry->d_name) >= static_cast<int>(sizeof(orphan)) ||
            unlink_path(orphan) != 0) {
            ::closedir(directory);
            return cleanup_mount_failure(ESP_FAIL);
        }
    }
    const int read_error = errno;
    const int close_result = ::closedir(directory);
    if (read_error != 0 || close_result != 0) {
        return cleanup_mount_failure(ESP_FAIL);
    }
    return ESP_OK;
}

esp_err_t StorageFatfs::unmount()
{
    if (has_active_sessions()) return ESP_ERR_INVALID_STATE;
    return provider_.unmount();
}

bool StorageFatfs::has_active_sessions() const
{
    return read_context_.active || write_context_.active;
}

storage::Storage StorageFatfs::api()
{
    return storage::Storage{
        this,
        stat_cb,
        list_page_cb,
        begin_read_cb,
        begin_write_cb,
    };
}

StorageFatfs *StorageFatfs::self(void *context)
{
    return static_cast<StorageFatfs *>(context);
}

storage::Error StorageFatfs::stat_cb(void *context, std::string_view path,
                                     storage::Metadata *metadata)
{
    return self(context)->stat_impl(path, metadata);
}

storage::Error StorageFatfs::list_page_cb(void *context, std::string_view path,
                                          std::string_view cursor, std::size_t limit,
                                          storage::DirectoryEntry *out,
                                          std::size_t capacity, std::size_t *count,
                                          bool *more)
{
    return self(context)->list_page_impl(path, cursor, limit, out, capacity, count, more);
}

storage::Error StorageFatfs::begin_read_cb(void *context, std::string_view path,
                                           storage::ReadSession *session)
{
    return self(context)->begin_read_impl(path, session);
}

storage::Error StorageFatfs::begin_write_cb(void *context, std::string_view path,
                                            std::uint64_t size, bool replace,
                                            storage::WriteSession *session)
{
    return self(context)->begin_write_impl(path, size, replace, session);
}

storage::Error StorageFatfs::read_cb(void *context, std::uint64_t offset,
                                     std::uint8_t *out, std::size_t requested,
                                     std::size_t *read)
{
    return static_cast<ReadContext *>(context)->owner->read_impl(offset, out, requested, read);
}

void StorageFatfs::close_read_cb(void *context)
{
    auto *session = static_cast<ReadContext *>(context);
    if (session != nullptr && session->owner != nullptr) session->owner->close_read_impl();
}

storage::Error StorageFatfs::write_cb(void *context, std::uint64_t offset,
                                      const std::uint8_t *data, std::size_t length)
{
    return static_cast<WriteContext *>(context)->owner->write_impl(offset, data, length);
}

storage::Error StorageFatfs::commit_cb(void *context)
{
    return static_cast<WriteContext *>(context)->owner->commit_impl();
}

void StorageFatfs::abort_cb(void *context)
{
    auto *session = static_cast<WriteContext *>(context);
    if (session != nullptr && session->owner != nullptr) session->owner->abort_impl();
}

storage::Error StorageFatfs::stat_impl(std::string_view path, storage::Metadata *metadata)
{
    if (metadata == nullptr || !validate_path(path, true)) return storage::Error::InvalidPath;
    if (!provider_.mounted()) return storage::Error::ReadFailed;
    static char physical[kPhysicalPathBytes]{};
    if (!make_physical_path(path, physical, sizeof(physical))) return storage::Error::InvalidPath;

    struct stat result{};
    if (!stat_path(physical, &result)) return map_errno(errno, false);
    if (S_ISREG(result.st_mode)) {
        metadata->type = storage::EntryType::File;
        metadata->size_bytes = static_cast<std::uint64_t>(result.st_size);
        return storage::Error::Ok;
    }
    if (S_ISDIR(result.st_mode)) {
        metadata->type = storage::EntryType::Directory;
        metadata->size_bytes = 0;
        return storage::Error::Ok;
    }
    return storage::Error::Unsupported;
}

storage::Error StorageFatfs::list_page_impl(std::string_view path, std::string_view cursor,
                                            std::size_t limit, storage::DirectoryEntry *out,
                                            std::size_t capacity, std::size_t *count,
                                            bool *more)
{
    if (count == nullptr || more == nullptr || out == nullptr || capacity == 0 ||
        limit == 0 || !validate_path(path, true) ||
        (!cursor.empty() && !valid_component(cursor))) {
        return storage::Error::InvalidPath;
    }
    if (!provider_.mounted()) return storage::Error::ReadFailed;
    *count = 0;
    *more = false;

    static char physical[kPhysicalPathBytes]{};
    if (!make_physical_path(path, physical, sizeof(physical))) return storage::Error::InvalidPath;
    DIR *directory = ::opendir(physical);
    if (directory == nullptr) return map_errno(errno, false);

    struct Candidate {
        char name[storage::kMaxComponentBytes + 1]{};
    };
    static std::array<Candidate, kMaxDirectoryEntries> candidates{};
    std::size_t candidate_count = 0;

    errno = 0;
    const dirent *entry = nullptr;
    while ((entry = ::readdir(directory)) != nullptr) {
        const std::string_view name(entry->d_name);
        if (name == "." || name == ".." || !valid_component(name) ||
            (!cursor.empty() && name <= cursor)) {
            continue;
        }
        if (candidate_count >= candidates.size()) {
            ::closedir(directory);
            return storage::Error::NoSpace;
        }

        if (!copy_component(candidates[candidate_count].name, name)) {
            ::closedir(directory);
            return storage::Error::InvalidPath;
        }
        ++candidate_count;
    }
    const int read_error = errno;
    const int close_result = ::closedir(directory);
    if (read_error != 0) return map_errno(read_error, false);
    if (close_result != 0) return map_errno(errno, false);

    std::sort(candidates.begin(), candidates.begin() + candidate_count,
              [](const Candidate &left, const Candidate &right) {
                  return std::strcmp(left.name, right.name) < 0;
              });

    const std::size_t output_count = std::min({candidate_count, capacity, limit});
    for (std::size_t index = 0; index < output_count; ++index) {
        static char child[kPhysicalPathBytes]{};
        if (std::snprintf(child, sizeof(child), "%s/%s", physical,
                          candidates[index].name) >= static_cast<int>(sizeof(child))) {
            return storage::Error::InvalidPath;
        }
        struct stat result{};
        if (!stat_path(child, &result)) return map_errno(errno, false);
        if (S_ISREG(result.st_mode)) {
            out[index].metadata.type = storage::EntryType::File;
            out[index].metadata.size_bytes = static_cast<std::uint64_t>(result.st_size);
        } else if (S_ISDIR(result.st_mode)) {
            out[index].metadata.type = storage::EntryType::Directory;
            out[index].metadata.size_bytes = 0;
        } else {
            return storage::Error::Unsupported;
        }
        std::memcpy(out[index].name, candidates[index].name,
                    std::strlen(candidates[index].name) + 1);
    }
    *count = output_count;
    *more = output_count < candidate_count;
    return storage::Error::Ok;
}

storage::Error StorageFatfs::begin_read_impl(std::string_view path,
                                             storage::ReadSession *session)
{
    if (session == nullptr || !validate_path(path, false)) return storage::Error::InvalidPath;
    if (has_active_sessions()) return storage::Error::Busy;
    if (!provider_.mounted()) return storage::Error::ReadFailed;

    static char physical[kPhysicalPathBytes]{};
    if (!make_physical_path(path, physical, sizeof(physical))) return storage::Error::InvalidPath;
    const int fd = ::open(physical, O_RDONLY, 0);
    if (fd < 0) return map_errno(errno, false);
    struct stat result{};
    if (::fstat(fd, &result) != 0) {
        const storage::Error error = map_errno(errno, false);
        ::close(fd);
        return error;
    }
    if (!S_ISREG(result.st_mode)) {
        const storage::Error error = S_ISDIR(result.st_mode) ?
            storage::Error::NotAFile : storage::Error::Unsupported;
        ::close(fd);
        return error;
    }
    read_context_ = ReadContext{this, fd, static_cast<std::uint64_t>(result.st_size), true};
    *session = storage::ReadSession{&read_context_, read_cb, close_read_cb};
    return storage::Error::Ok;
}

storage::Error StorageFatfs::begin_write_impl(std::string_view path, std::uint64_t size,
                                              bool replace, storage::WriteSession *session)
{
    if (session == nullptr || !validate_path(path, false)) return storage::Error::InvalidPath;
    if (has_active_sessions()) return storage::Error::Busy;
    if (!provider_.mounted()) return storage::Error::WriteFailed;
    if (!parent_is_directory(path)) return storage::Error::ParentNotFound;

    static char target[kPhysicalPathBytes]{};
    if (!make_physical_path(path, target, sizeof(target))) return storage::Error::InvalidPath;
    struct stat target_stat{};
    const bool target_exists = stat_path(target, &target_stat);
    if (!target_exists && errno != ENOENT) return map_errno(errno, true);
    if (target_exists && S_ISDIR(target_stat.st_mode)) return storage::Error::NotAFile;
    if (target_exists && !replace) return storage::Error::AlreadyExists;

    static char staging[kPhysicalPathBytes]{};
    int fd = -1;
    for (unsigned attempt = 0; attempt < 32 && fd < 0; ++attempt) {
        if (!make_staging_path(staging, sizeof(staging), "s", ".tmp")) {
            return storage::Error::WriteFailed;
        }
        fd = ::open(staging, O_CREAT | O_EXCL | O_RDWR, 0666);
        if (fd < 0 && errno != EEXIST) {
            const int open_errno = errno;
            return open_errno == EIO ? storage::Error::NoSpace :
                map_errno(open_errno, true);
        }
    }
    if (fd < 0) return storage::Error::NoSpace;
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        const int truncate_errno = errno;
        // FatFS reports an exhausted allocation during ftruncate() as EIO on
        // this target/emulator; preserve ordinary write-side EIO mapping.
        const storage::Error error = (truncate_errno == 0 || truncate_errno == EIO) ?
            storage::Error::NoSpace :
            map_errno(truncate_errno, true);
        ::close(fd);
        unlink_path(staging);
        return error;
    }

    write_context_ = WriteContext{this, fd, size, target_exists, false, true};
    if (!copy_string(write_context_.staging_path, sizeof(write_context_.staging_path), staging) ||
        !copy_string(write_context_.target_path, sizeof(write_context_.target_path), target)) {
        ::close(fd);
        unlink_path(staging);
        reset_write_context();
        return storage::Error::WriteFailed;
    }
    *session = storage::WriteSession{&write_context_, write_cb, commit_cb, abort_cb};
    return storage::Error::Ok;
}

storage::Error StorageFatfs::read_impl(std::uint64_t offset, std::uint8_t *out,
                                       std::size_t requested, std::size_t *read)
{
    if (read == nullptr || (out == nullptr && requested != 0) ||
        !read_context_.active || read_context_.fd < 0) {
        return storage::Error::ReadFailed;
    }
    if (offset > read_context_.size) return storage::Error::OutOfRange;
    const std::size_t amount = std::min<std::uint64_t>(
        requested, read_context_.size - offset);
    if (amount == 0) {
        *read = 0;
        return storage::Error::Ok;
    }
    std::size_t completed = 0;
    while (completed < amount) {
        const std::size_t chunk = std::min(kFatIoChunkBytes, amount - completed);
        alignas(16) static std::array<std::uint8_t, kFatIoChunkBytes> scratch{};
        std::uint8_t *destination = chunk < kFatIoChunkBytes ? scratch.data() :
            out + completed;
        const std::size_t request = chunk < kFatIoChunkBytes ? kFatIoChunkBytes : chunk;
        const ssize_t result = ::pread(read_context_.fd, destination, request,
                                       static_cast<off_t>(offset + completed));
        if (result < 0) return map_errno(errno, false);
        if (static_cast<std::size_t>(result) < chunk) return storage::Error::ReadFailed;
        if (destination == scratch.data()) {
            std::memcpy(out + completed, scratch.data(), chunk);
        }
        completed += chunk;
    }
    *read = completed;
    return storage::Error::Ok;
}

void StorageFatfs::close_read_impl()
{
    if (!read_context_.active) return;
    ::close(read_context_.fd);
    reset_read_context();
}

storage::Error StorageFatfs::write_impl(std::uint64_t offset, const std::uint8_t *data,
                                        std::size_t length)
{
    if (!write_context_.active || write_context_.fd < 0 ||
        (data == nullptr && length != 0)) return storage::Error::WriteFailed;
    if (offset > write_context_.size || length > write_context_.size - offset) {
        return storage::Error::OutOfRange;
    }
    if (length == 0) return storage::Error::Ok;
    std::size_t completed = 0;
    while (completed < length) {
        const std::size_t chunk = std::min(kFatIoChunkBytes, length - completed);
        const ssize_t result = ::pwrite(write_context_.fd, data + completed, chunk,
                                         static_cast<off_t>(offset + completed));
        if (result < 0) return map_errno(errno, true);
        if (static_cast<std::size_t>(result) != chunk) return storage::Error::WriteFailed;
        completed += chunk;
    }
    return storage::Error::Ok;
}

storage::Error StorageFatfs::commit_impl()
{
    if (!write_context_.active || write_context_.fd < 0) return storage::Error::CommitFailed;
    if (::fsync(write_context_.fd) != 0) {
        const storage::Error error = map_errno(errno, true);
        abort_impl();
        return error == storage::Error::WriteFailed ? error : storage::Error::CommitFailed;
    }
    ::close(write_context_.fd);
    write_context_.fd = -1;

    if (write_context_.target_exists) {
        if (!make_staging_path(write_context_.backup_path,
                               sizeof(write_context_.backup_path), "b", ".bak") ||
            rename_path(write_context_.target_path, write_context_.backup_path) != 0) {
            abort_impl();
            return storage::Error::CommitFailed;
        }
        write_context_.target_moved = true;
    }

    if (rename_path(write_context_.staging_path, write_context_.target_path) != 0) {
        if (write_context_.target_moved) {
            if (rename_path(write_context_.backup_path, write_context_.target_path) != 0) {
                ESP_LOGE(kLogTag,
                         "replacement install and rollback failed; preserving staging and backup");
                return storage::Error::CommitFailed;
            }
        }
        if (unlink_path(write_context_.staging_path) != 0) {
            ESP_LOGW(kLogTag, "staging cleanup pending after failed replacement: %s",
                     write_context_.staging_path);
        }
        reset_write_context();
        return storage::Error::CommitFailed;
    }

    if (write_context_.target_moved && unlink_path(write_context_.backup_path) != 0) {
        ESP_LOGW(kLogTag, "replacement committed; backup cleanup pending: %s",
                 write_context_.backup_path);
        reset_write_context();
        return storage::Error::Ok;
    }
    reset_write_context();
    return storage::Error::Ok;
}

void StorageFatfs::abort_impl()
{
    if (!write_context_.active) return;
    if (write_context_.fd >= 0) ::close(write_context_.fd);
    const int staging_unlink_result = unlink_path(write_context_.staging_path);
    const int staging_unlink_errno = errno;
    if (write_context_.target_moved) {
        struct stat target{};
        if (::stat(write_context_.target_path, &target) != 0 && errno == ENOENT) {
            if (rename_path(write_context_.backup_path, write_context_.target_path) != 0) {
                ESP_LOGE(kLogTag, "replacement abort could not restore backup: %s",
                         write_context_.backup_path);
                reset_write_context();
                return;
            }
        }
    }
    if (staging_unlink_result != 0 && staging_unlink_errno != ENOENT) {
        ESP_LOGW(kLogTag, "staging cleanup pending after abort: %s",
                 write_context_.staging_path);
    }
    reset_write_context();
}

bool StorageFatfs::valid_utf8(std::string_view value)
{
    for (std::size_t index = 0; index < value.size();) {
        unsigned char first = 0;
        std::memcpy(&first, value.data() + index, sizeof(first));
        std::size_t count = 0;
        std::uint32_t code = 0;
        if (first <= 0x7f) { count = 1; code = first; }
        else if (first >= 0xc2 && first <= 0xdf) { count = 2; code = first & 0x1f; }
        else if (first >= 0xe0 && first <= 0xef) { count = 3; code = first & 0x0f; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 4; code = first & 0x07; }
        else return false;
        if (index + count > value.size()) return false;
        for (std::size_t offset = 1; offset < count; ++offset) {
            unsigned char next = 0;
            std::memcpy(&next, value.data() + index + offset, sizeof(next));
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if ((count == 2 && code < 0x80) || (count == 3 && code < 0x800) ||
            (count == 4 && code < 0x10000) || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff)) return false;
        index += count;
    }
    return true;
}

bool StorageFatfs::valid_component(std::string_view value)
{
    if (value.empty() || value.size() > storage::kMaxComponentBytes ||
        !valid_utf8(value) || value == "." || value == "..") return false;
    for (unsigned char value_byte : value) {
        if (value_byte == '/' || value_byte == '\\' || value_byte < 0x20 ||
            value_byte == 0x7f || is_fat_invalid(value_byte)) return false;
    }
    return true;
}

bool StorageFatfs::validate_path(std::string_view path, bool allow_root)
{
    if (path.empty() || path.size() > storage::kMaxPathBytes || path.front() != '/' ||
        !valid_utf8(path)) return false;
    if (path == "/") return allow_root;
    if (path.back() == '/') return false;
    std::size_t start = 1;
    while (start < path.size()) {
        const std::size_t end = path.find('/', start);
        const std::size_t length = end == std::string_view::npos ?
            path.size() - start : end - start;
        if (length > storage::kMaxComponentBytes) return false;
        static std::array<char, storage::kMaxComponentBytes + 1> component{};
        std::memcpy(component.data(), path.data() + start, length);
        if (!valid_component(std::string_view(component.data(), length))) return false;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

bool StorageFatfs::make_physical_path(std::string_view path, char *out, std::size_t capacity)
{
    if (out == nullptr || capacity == 0 || roots_.file_transfer_root == nullptr) return false;
    if (path == "/") {
        return copy_string(out, capacity, roots_.file_transfer_root);
    }
    const std::size_t prefix_size = std::strlen(roots_.file_transfer_root);
    if (prefix_size + path.size() >= capacity) return false;
    std::memcpy(out, roots_.file_transfer_root, prefix_size);
    std::memcpy(out + prefix_size, path.data(), path.size());
    out[prefix_size + path.size()] = '\0';
    return true;
}

storage::Error StorageFatfs::map_errno(int error, bool write_operation)
{
    switch (error) {
    case ENOENT: return storage::Error::NotFound;
    case EEXIST: return storage::Error::AlreadyExists;
    case ENOTDIR: return storage::Error::NotADirectory;
    case EISDIR: return storage::Error::NotAFile;
    case EFBIG:
    case ENOSPC: return storage::Error::NoSpace;
    case EBUSY: return storage::Error::Busy;
    case EINVAL:
    case ENAMETOOLONG: return storage::Error::InvalidPath;
    case EACCES:
    case EROFS: return write_operation ? storage::Error::WriteFailed : storage::Error::ReadFailed;
    case EIO: return write_operation ? storage::Error::WriteFailed : storage::Error::ReadFailed;
    default: return write_operation ? storage::Error::WriteFailed : storage::Error::ReadFailed;
    }
}

bool StorageFatfs::copy_component(char *out, std::string_view value)
{
    if (out == nullptr || value.size() > storage::kMaxComponentBytes) return false;
    std::memcpy(out, value.data(), value.size());
    out[value.size()] = '\0';
    return true;
}

bool StorageFatfs::ensure_directory(const char *path)
{
    struct stat result{};
    if (stat_path(path, &result)) return S_ISDIR(result.st_mode);
    if (errno != ENOENT || ::mkdir(path, 0777) != 0) return false;
    return true;
}

bool StorageFatfs::make_staging_path(char *out, std::size_t capacity,
                                     const char *prefix, const char *suffix)
{
    if (out == nullptr || prefix == nullptr || suffix == nullptr ||
        roots_.staging_root == nullptr) return false;
    const int written = std::snprintf(out, capacity, "%s/%s%08lx%s", roots_.staging_root,
                                      prefix, static_cast<unsigned long>(sequence_++), suffix);
    return written >= 0 && static_cast<std::size_t>(written) < capacity;
}

bool StorageFatfs::parent_is_directory(std::string_view path)
{
    const std::size_t slash = path.rfind('/');
    const std::string_view parent = slash == 0 ? std::string_view("/") : path.substr(0, slash);
    static char physical[kPhysicalPathBytes]{};
    if (!make_physical_path(parent, physical, sizeof(physical))) return false;
    struct stat result{};
    return stat_path(physical, &result) && S_ISDIR(result.st_mode);
}

int StorageFatfs::rename_path(const char *from, const char *to)
{
#if defined(STORAGE_FATFS_TEST_HOOKS)
    if (test_hooks_ != nullptr) {
        ++test_hooks_->rename_call_count;
        const int call = test_hooks_->rename_call_count;
        if (call == test_hooks_->fail_rename_call_1 ||
            call == test_hooks_->fail_rename_call_2) {
            errno = EIO;
            return -1;
        }
    }
#endif
    return ::rename(from, to);
}

int StorageFatfs::unlink_path(const char *path)
{
#if defined(STORAGE_FATFS_TEST_HOOKS)
    if (test_hooks_ != nullptr) {
        ++test_hooks_->unlink_call_count;
        if (test_hooks_->unlink_call_count == test_hooks_->fail_unlink_call) {
            errno = EIO;
            return -1;
        }
    }
#endif
    return ::unlink(path);
}

void StorageFatfs::reset_read_context()
{
    read_context_ = ReadContext{};
}

void StorageFatfs::reset_write_context()
{
    write_context_ = WriteContext{};
}

} // namespace storage_fatfs
