#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "storage/storage.hpp"
#include "wear_levelling.h"

namespace storage_fatfs {

inline constexpr char kMountPath[] = "/persist";
inline constexpr char kPartitionLabel[] = "storage";
inline constexpr char kFileTransferRoot[] = "/persist/files";
inline constexpr char kFixtureRoot[] = "/persist/fixtures";
inline constexpr char kStagingRoot[] = "/persist/.np2-staging";
inline constexpr char kFixturePath[] = "/persist/fixtures/np2test-fd1232.hdm";
inline constexpr std::size_t kPhysicalPathBytes = storage::kMaxPathBytes + 32;

class MountProvider {
public:
    esp_err_t mount();
    esp_err_t unmount();

    bool mounted() const { return mounted_; }
    wl_handle_t wl_handle() const { return wl_handle_; }

private:
    bool mounted_ = false;
    wl_handle_t wl_handle_ = WL_INVALID_HANDLE;
};

class StorageFatfs {
public:
    explicit StorageFatfs(MountProvider &provider) : provider_(provider) {}

    esp_err_t mount();
    esp_err_t unmount();
    bool mounted() const { return provider_.mounted(); }
    bool has_active_sessions() const;

    storage::Storage api();

private:
    struct ReadContext {
        StorageFatfs *owner = nullptr;
        int fd = -1;
        std::uint64_t size = 0;
        bool active = false;
    };

    struct WriteContext {
        StorageFatfs *owner = nullptr;
        int fd = -1;
        std::uint64_t size = 0;
        bool target_exists = false;
        bool target_moved = false;
        bool active = false;
        char staging_path[kPhysicalPathBytes]{};
        char target_path[kPhysicalPathBytes]{};
        char backup_path[kPhysicalPathBytes]{};
    };

    MountProvider &provider_;
    std::uint32_t sequence_ = 1;
    ReadContext read_context_{};
    WriteContext write_context_{};

    static StorageFatfs *self(void *context);
    static storage::Error stat_cb(void *, std::string_view, storage::Metadata *);
    static storage::Error list_page_cb(void *, std::string_view, std::string_view,
                                       std::size_t, storage::DirectoryEntry *,
                                       std::size_t, std::size_t *, bool *);
    static storage::Error begin_read_cb(void *, std::string_view, storage::ReadSession *);
    static storage::Error begin_write_cb(void *, std::string_view, std::uint64_t,
                                         bool, storage::WriteSession *);
    static storage::Error read_cb(void *, std::uint64_t, std::uint8_t *,
                                  std::size_t, std::size_t *);
    static void close_read_cb(void *);
    static storage::Error write_cb(void *, std::uint64_t, const std::uint8_t *, std::size_t);
    static storage::Error commit_cb(void *);
    static void abort_cb(void *);

    storage::Error stat_impl(std::string_view, storage::Metadata *);
    storage::Error list_page_impl(std::string_view, std::string_view, std::size_t,
                                  storage::DirectoryEntry *, std::size_t,
                                  std::size_t *, bool *);
    storage::Error begin_read_impl(std::string_view, storage::ReadSession *);
    storage::Error begin_write_impl(std::string_view, std::uint64_t, bool,
                                    storage::WriteSession *);
    storage::Error read_impl(std::uint64_t, std::uint8_t *, std::size_t, std::size_t *);
    void close_read_impl();
    storage::Error write_impl(std::uint64_t, const std::uint8_t *, std::size_t);
    storage::Error commit_impl();
    void abort_impl();

    static bool valid_utf8(std::string_view);
    static bool valid_component(std::string_view);
    static bool validate_path(std::string_view, bool allow_root);
    static bool make_physical_path(std::string_view, char *, std::size_t);
    static storage::Error map_errno(int, bool write_operation);
    static bool copy_component(char *, std::string_view);
    bool ensure_directory(const char *path);
    bool make_staging_path(char *, std::size_t, const char *prefix, const char *suffix);
    bool parent_is_directory(std::string_view);
    void reset_read_context();
    void reset_write_context();
};

} // namespace storage_fatfs
