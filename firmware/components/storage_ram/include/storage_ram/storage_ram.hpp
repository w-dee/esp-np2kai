#pragma once

#include <cstddef>
#include <cstdint>

#include "storage/storage.hpp"

namespace storage_ram {

inline constexpr std::size_t kArenaBytes = 256 * 1024;
inline constexpr std::size_t kMaxEntries = 32;
inline constexpr std::size_t kMaxBlocks = 64;
inline constexpr std::size_t kMaxFileBytes = 192 * 1024;

class StorageRam {
public:
    void init();
    storage::Storage api();

private:
    struct Entry {
        bool used = false;
        bool committed = false;
        storage::EntryType type = storage::EntryType::File;
        char path[storage::kMaxPathBytes + 1]{};
        std::uint64_t size = 0;
        int block = -1;
    };
    struct Block { std::size_t offset = 0; std::size_t size = 0; bool used = false; };
    struct ReadContext { StorageRam *owner = nullptr; int entry = -1; };
    struct WriteContext {
        StorageRam *owner = nullptr;
        int staging = -1;
        int target = -1;
        bool committed = false;
    };

    Entry entries_[kMaxEntries]{};
    Block blocks_[kMaxBlocks]{};
    std::uint8_t arena_[kArenaBytes]{};
    ReadContext read_context_{};
    WriteContext write_context_{};

    static StorageRam *self(void *context);
    static storage::Error stat_cb(void *, std::string_view, storage::Metadata *);
    static storage::Error list_cb(void *, std::string_view, std::string_view,
                                  std::size_t, storage::DirectoryEntry *,
                                  std::size_t, std::size_t *, bool *);
    static storage::Error begin_read_cb(void *, std::string_view, storage::ReadSession *);
    static storage::Error begin_write_cb(void *, std::string_view, std::uint64_t,
                                         bool, storage::WriteSession *);
    static storage::Error read_cb(void *, std::uint64_t, std::uint8_t *, std::size_t, std::size_t *);
    static void close_read_cb(void *);
    static storage::Error write_cb(void *, std::uint64_t, const std::uint8_t *, std::size_t);
    static storage::Error commit_cb(void *);
    static void abort_cb(void *);

    int find(std::string_view path) const;
    int find_free_entry() const;
    bool parent_exists(std::string_view path) const;
    int allocate_block(std::size_t size);
    void release_block(int index);
    int add_entry(std::string_view path, storage::EntryType type);
    storage::Error stat_impl(std::string_view path, storage::Metadata *metadata);
    storage::Error list_impl(std::string_view path, std::string_view cursor,
                             std::size_t limit, storage::DirectoryEntry *out,
                             std::size_t capacity, std::size_t *count, bool *more);
    storage::Error begin_read_impl(std::string_view path, storage::ReadSession *session);
    storage::Error begin_write_impl(std::string_view path, std::uint64_t size,
                                    bool replace, storage::WriteSession *session);
    storage::Error read_impl(std::uint64_t offset, std::uint8_t *out,
                             std::size_t requested, std::size_t *read);
    storage::Error write_impl(std::uint64_t offset, const std::uint8_t *data,
                              std::size_t length);
    storage::Error commit_impl();
    void abort_impl();
};

} // namespace storage_ram
