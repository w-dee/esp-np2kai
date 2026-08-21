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

struct RootConfig {
    const char *file_transfer_root;
    const char *staging_root;
    const char *fixture_root;
};

inline constexpr RootConfig kSpiNorRootConfig{
    kFileTransferRoot,
    kStagingRoot,
    kFixtureRoot,
};

#if defined(STORAGE_FATFS_TEST_HOOKS)
struct TestHooks {
    bool fail_mount_initialization = false;
    int fail_rename_call_1 = -1;
    int fail_rename_call_2 = -1;
    int fail_unlink_call = -1;
    int rename_call_count = 0;
    int unlink_call_count = 0;
};
#endif

class FatfsMountBackend {
public:
    virtual ~FatfsMountBackend() = default;

    virtual esp_err_t mount() = 0;
    virtual esp_err_t unmount() = 0;
    virtual bool mounted() const = 0;
};

class MountProvider final : public FatfsMountBackend {
public:
    esp_err_t mount() override;
    esp_err_t unmount() override;

    bool mounted() const override { return mounted_; }
    wl_handle_t wl_handle() const { return wl_handle_; }

#if defined(STORAGE_FATFS_TEST_HOOKS)
    void set_test_unmount_failure(bool enabled) { test_unmount_failure_ = enabled; }
#endif

private:
    bool mounted_ = false;
    wl_handle_t wl_handle_ = WL_INVALID_HANDLE;
#if defined(STORAGE_FATFS_TEST_HOOKS)
    bool test_unmount_failure_ = false;
#endif
};

class StorageFatfs {
public:
    explicit StorageFatfs(FatfsMountBackend &provider,
                          RootConfig roots = kSpiNorRootConfig)
        : provider_(provider), roots_(roots) {}

    esp_err_t mount();
    esp_err_t unmount();
    bool mounted() const { return provider_.mounted(); }
    bool has_active_sessions() const;

    storage::Storage api();

#if defined(STORAGE_FATFS_TEST_HOOKS)
    void set_test_hooks(TestHooks *hooks) { test_hooks_ = hooks; }
#endif

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

    FatfsMountBackend &provider_;
    const RootConfig roots_;
    std::uint32_t sequence_ = 1;
    ReadContext read_context_{};
    WriteContext write_context_{};
#if defined(STORAGE_FATFS_TEST_HOOKS)
    TestHooks *test_hooks_ = nullptr;
#endif

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
    bool make_physical_path(std::string_view, char *, std::size_t);
    static storage::Error map_errno(int, bool write_operation);
    static bool copy_component(char *, std::string_view);
    bool ensure_directory(const char *path);
    bool make_staging_path(char *, std::size_t, const char *prefix, const char *suffix);
    bool parent_is_directory(std::string_view);
    int rename_path(const char *, const char *);
    int unlink_path(const char *);
    void reset_read_context();
    void reset_write_context();
};

} // namespace storage_fatfs
