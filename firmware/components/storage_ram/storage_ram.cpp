#include "storage_ram/storage_ram.hpp"

#include <cstring>
#include <cstdio>

namespace storage_ram {
namespace {

bool starts_with(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool copy_path(char *out, std::string_view path)
{
    if (path.empty() || path.size() > storage::kMaxPathBytes) {
        return false;
    }
    std::memcpy(out, path.data(), path.size());
    out[path.size()] = '\0';
    return true;
}

} // namespace

void StorageRam::init()
{
    for (auto &entry : entries_) entry = Entry{};
    for (auto &block : blocks_) block = Block{};
    std::memset(arena_, 0, sizeof(arena_));
    read_context_ = ReadContext{};
    write_context_ = WriteContext{};
    blocks_[0] = Block{0, kArenaBytes, false};
    add_entry("/", storage::EntryType::Directory);
    add_entry("/seed", storage::EntryType::Directory);
    add_entry("/upload", storage::EntryType::Directory);
    add_entry("/long", storage::EntryType::Directory);

    const char *seed_path = "/seed/existing.bin";
    const int existing = add_entry(seed_path, storage::EntryType::File);
    if (existing >= 0) {
        entries_[existing].size = 37;
        entries_[existing].block = allocate_block(37);
        if (entries_[existing].block >= 0) {
            for (std::size_t i = 0; i < 37; ++i) {
                arena_[blocks_[entries_[existing].block].offset + i] =
                    static_cast<std::uint8_t>(0xa0u + i);
            }
        }
    }
    for (int page = 0; page < 12; ++page) {
        char path[storage::kMaxPathBytes + 1]{};
        const int written = std::snprintf(path, sizeof(path), "/seed/page-%02d.bin", page);
        if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(path)) {
            continue;
        }
        const int index = add_entry(path, storage::EntryType::File);
        if (index >= 0) {
            entries_[index].size = static_cast<std::uint64_t>(16 + page);
            entries_[index].block = allocate_block(static_cast<std::size_t>(entries_[index].size));
            if (entries_[index].block >= 0) {
                const std::size_t base = blocks_[entries_[index].block].offset;
                for (std::size_t i = 0; i < entries_[index].size; ++i) {
                    arena_[base + i] = static_cast<std::uint8_t>(page * 17 + i);
                }
            }
        }
    }
    for (int page = 0; page < 6; ++page) {
        char path[storage::kMaxPathBytes + 1]{};
        const int written = std::snprintf(
            path, sizeof(path),
            "/long/%02d-abcdefghijklmnopqrstuvwxyz-ABCDEFGHIJKLMNOPQRSTUVWXYZ.bin", page);
        if (written > 0 && static_cast<std::size_t>(written) < sizeof(path)) {
            add_entry(path, storage::EntryType::File);
        }
    }
}

storage::Storage StorageRam::api()
{
    return storage::Storage{
        this,
        stat_cb,
        list_cb,
        begin_read_cb,
        begin_write_cb,
    };
}

StorageRam *StorageRam::self(void *context)
{
    return static_cast<StorageRam *>(context);
}

storage::Error StorageRam::stat_cb(void *context, std::string_view path,
                                   storage::Metadata *metadata)
{
    return self(context)->stat_impl(path, metadata);
}

storage::Error StorageRam::list_cb(void *context, std::string_view path,
                                   std::string_view cursor, std::size_t limit,
                                   storage::DirectoryEntry *out, std::size_t capacity,
                                   std::size_t *count, bool *more)
{
    return self(context)->list_impl(path, cursor, limit, out, capacity, count, more);
}

storage::Error StorageRam::begin_read_cb(void *context, std::string_view path,
                                         storage::ReadSession *session)
{
    return self(context)->begin_read_impl(path, session);
}

storage::Error StorageRam::begin_write_cb(void *context, std::string_view path,
                                          std::uint64_t size, bool replace,
                                          storage::WriteSession *session)
{
    return self(context)->begin_write_impl(path, size, replace, session);
}

storage::Error StorageRam::read_cb(void *context, std::uint64_t offset,
                                   std::uint8_t *out, std::size_t requested,
                                   std::size_t *read)
{
    return static_cast<ReadContext *>(context)->owner->read_impl(offset, out, requested, read);
}

void StorageRam::close_read_cb(void *context)
{
    auto *ctx = static_cast<ReadContext *>(context);
    if (ctx != nullptr && ctx->owner != nullptr) {
        ctx->owner->read_context_ = ReadContext{};
    }
}

storage::Error StorageRam::write_cb(void *context, std::uint64_t offset,
                                    const std::uint8_t *data, std::size_t length)
{
    return static_cast<WriteContext *>(context)->owner->write_impl(offset, data, length);
}

storage::Error StorageRam::commit_cb(void *context)
{
    return static_cast<WriteContext *>(context)->owner->commit_impl();
}

void StorageRam::abort_cb(void *context)
{
    auto *ctx = static_cast<WriteContext *>(context);
    if (ctx != nullptr && ctx->owner != nullptr) {
        ctx->owner->abort_impl();
    }
}

int StorageRam::find(std::string_view path) const
{
    for (int i = 0; i < static_cast<int>(kMaxEntries); ++i) {
        if (entries_[i].used && entries_[i].committed &&
            std::string_view(entries_[i].path) == path) {
            return i;
        }
    }
    return -1;
}

int StorageRam::find_free_entry() const
{
    for (int i = 0; i < static_cast<int>(kMaxEntries); ++i) {
        if (!entries_[i].used) {
            return i;
        }
    }
    return -1;
}

bool StorageRam::parent_exists(std::string_view path) const
{
    if (path == "/") {
        return false;
    }
    const std::size_t slash = path.find_last_of('/');
    const std::string_view parent = slash == 0 ? std::string_view("/") : path.substr(0, slash);
    const int index = find(parent);
    return index >= 0 && entries_[index].type == storage::EntryType::Directory;
}

int StorageRam::allocate_block(std::size_t size)
{
    if (size == 0 || size > kArenaBytes) {
        return -1;
    }
    for (int i = 0; i < static_cast<int>(kMaxBlocks); ++i) {
        Block &block = blocks_[i];
        if (block.size == 0 || block.used || block.size < size) {
            continue;
        }
        if (block.size == size) {
            block.used = true;
            return i;
        }
        int split = -1;
        for (int j = 0; j < static_cast<int>(kMaxBlocks); ++j) {
            if (blocks_[j].size == 0) {
                split = j;
                break;
            }
        }
        if (split < 0) {
            continue;
        }
        blocks_[split] = Block{block.offset + size, block.size - size, false};
        block.size = size;
        block.used = true;
        return i;
    }
    return -1;
}

void StorageRam::release_block(int index)
{
    if (index < 0 || index >= static_cast<int>(kMaxBlocks) || blocks_[index].size == 0) {
        return;
    }
    blocks_[index].used = false;
    for (int pass = 0; pass < static_cast<int>(kMaxBlocks); ++pass) {
        bool merged = false;
        for (int i = 0; i < static_cast<int>(kMaxBlocks); ++i) {
            if (blocks_[i].size == 0 || blocks_[i].used) {
                continue;
            }
            for (int j = i + 1; j < static_cast<int>(kMaxBlocks); ++j) {
                if (blocks_[j].size == 0 || blocks_[j].used) {
                    continue;
                }
                if (blocks_[i].offset + blocks_[i].size == blocks_[j].offset) {
                    blocks_[i].size += blocks_[j].size;
                    blocks_[j] = Block{};
                    merged = true;
                } else if (blocks_[j].offset + blocks_[j].size == blocks_[i].offset) {
                    blocks_[j].size += blocks_[i].size;
                    blocks_[i] = Block{};
                    merged = true;
                }
            }
        }
        if (!merged) {
            break;
        }
    }
}

int StorageRam::add_entry(std::string_view path, storage::EntryType type)
{
    const int index = find_free_entry();
    if (index < 0 || !copy_path(entries_[index].path, path)) {
        return -1;
    }
    entries_[index] = Entry{};
    entries_[index].used = true;
    entries_[index].committed = true;
    entries_[index].type = type;
    copy_path(entries_[index].path, path);
    return index;
}

storage::Error StorageRam::stat_impl(std::string_view path, storage::Metadata *metadata)
{
    if (metadata == nullptr) return storage::Error::InvalidPath;
    const int index = find(path);
    if (index < 0) return storage::Error::NotFound;
    metadata->type = entries_[index].type;
    metadata->size_bytes = entries_[index].type == storage::EntryType::File ? entries_[index].size : 0;
    return storage::Error::Ok;
}

storage::Error StorageRam::list_impl(std::string_view path, std::string_view cursor,
                                     std::size_t limit, storage::DirectoryEntry *out,
                                     std::size_t capacity, std::size_t *count, bool *more)
{
    if (count == nullptr || more == nullptr || out == nullptr || capacity == 0) {
        return storage::Error::InvalidPath;
    }
    *count = 0;
    *more = false;
    const int parent = find(path);
    if (parent < 0) return storage::Error::NotFound;
    if (entries_[parent].type != storage::EntryType::Directory) return storage::Error::NotADirectory;
    if (limit == 0) return storage::Error::InvalidPath;

    char prefix[storage::kMaxPathBytes + 2]{};
    const std::size_t prefix_length = path == "/" ? 1 : path.size() + 1;
    if (prefix_length > storage::kMaxPathBytes + 1) return storage::Error::InvalidPath;
    if (path == "/") {
        prefix[0] = '/';
    } else {
        std::memcpy(prefix, path.data(), path.size());
        prefix[path.size()] = '/';
    }
    const std::string_view prefix_view(prefix, prefix_length);
    int candidates[kMaxEntries]{};
    std::size_t candidate_count = 0;
    for (int i = 0; i < static_cast<int>(kMaxEntries); ++i) {
        if (!entries_[i].used || !entries_[i].committed || entries_[i].path[0] == '\0') continue;
        const std::string_view full(entries_[i].path);
        if (!starts_with(full, prefix_view) || full.size() <= prefix_view.size()) continue;
        const std::string_view remainder = full.substr(prefix_view.size());
        const std::size_t slash = remainder.find('/');
        if (slash != std::string_view::npos) continue;
        if (!cursor.empty() && remainder <= cursor) continue;
        if (remainder.size() > storage::kMaxComponentBytes) continue;
        candidates[candidate_count++] = i;
    }
    for (std::size_t i = 1; i < candidate_count; ++i) {
        const int value = candidates[i];
        const std::string_view value_name(entries_[value].path + prefix_length);
        std::size_t j = i;
        while (j > 0) {
            const std::string_view previous(entries_[candidates[j - 1]].path + prefix_length);
            if (previous <= value_name) break;
            candidates[j] = candidates[j - 1];
            --j;
        }
        candidates[j] = value;
    }
    const std::size_t output_count =
        candidate_count < capacity && candidate_count < limit ? candidate_count :
        (capacity < limit ? capacity : limit);
    for (std::size_t i = 0; i < output_count; ++i) {
        const Entry &entry = entries_[candidates[i]];
        const std::string_view name(entry.path + prefix_length);
        std::memcpy(out[i].name, name.data(), name.size());
        out[i].name[name.size()] = '\0';
        out[i].metadata.type = entry.type;
        out[i].metadata.size_bytes = entry.type == storage::EntryType::File ? entry.size : 0;
    }
    *count = output_count;
    *more = output_count < candidate_count;
    return storage::Error::Ok;
}

storage::Error StorageRam::begin_read_impl(std::string_view path, storage::ReadSession *session)
{
    if (session == nullptr) return storage::Error::ReadFailed;
    if (read_context_.owner != nullptr) return storage::Error::Busy;
    const int index = find(path);
    if (index < 0) return storage::Error::NotFound;
    if (entries_[index].type != storage::EntryType::File) return storage::Error::NotAFile;
    read_context_ = ReadContext{this, index};
    *session = storage::ReadSession{&read_context_, read_cb, close_read_cb};
    return storage::Error::Ok;
}

storage::Error StorageRam::begin_write_impl(std::string_view path, std::uint64_t size,
                                            bool replace, storage::WriteSession *session)
{
    if (session == nullptr) return storage::Error::WriteFailed;
    if (write_context_.owner != nullptr) return storage::Error::Busy;
    if (size > kMaxFileBytes) return storage::Error::NoSpace;
    if (!parent_exists(path)) return storage::Error::ParentNotFound;
    const int target = find(path);
    if (target >= 0 && entries_[target].type != storage::EntryType::File) return storage::Error::NotAFile;
    if (target >= 0 && !replace) return storage::Error::AlreadyExists;
    const int staging = find_free_entry();
    if (staging < 0) return storage::Error::NoSpace;
    int block = -1;
    if (size != 0) {
        block = allocate_block(static_cast<std::size_t>(size));
        if (block < 0) return storage::Error::NoSpace;
    }
    entries_[staging] = Entry{};
    entries_[staging].used = true;
    entries_[staging].committed = false;
    entries_[staging].type = storage::EntryType::File;
    entries_[staging].size = size;
    entries_[staging].block = block;
    copy_path(entries_[staging].path, path);
    write_context_ = WriteContext{this, staging, target, false};
    *session = storage::WriteSession{&write_context_, write_cb, commit_cb, abort_cb};
    return storage::Error::Ok;
}

storage::Error StorageRam::read_impl(std::uint64_t offset, std::uint8_t *out,
                                     std::size_t requested, std::size_t *read)
{
    if (read == nullptr || out == nullptr || read_context_.entry < 0) return storage::Error::ReadFailed;
    const Entry &entry = entries_[read_context_.entry];
    if (offset > entry.size) return storage::Error::OutOfRange;
    const std::size_t available = static_cast<std::size_t>(entry.size - offset);
    const std::size_t amount = requested < available ? requested : available;
    if (amount != 0 && entry.block < 0) return storage::Error::ReadFailed;
    if (amount != 0) std::memcpy(out, arena_ + blocks_[entry.block].offset + offset, amount);
    *read = amount;
    return storage::Error::Ok;
}

storage::Error StorageRam::write_impl(std::uint64_t offset, const std::uint8_t *data,
                                      std::size_t length)
{
    if (data == nullptr || write_context_.staging < 0) return storage::Error::WriteFailed;
    const Entry &entry = entries_[write_context_.staging];
    if (offset > entry.size || length > entry.size - offset || (length != 0 && entry.block < 0)) {
        return storage::Error::OutOfRange;
    }
    if (length != 0) std::memcpy(arena_ + blocks_[entry.block].offset + offset, data, length);
    return storage::Error::Ok;
}

storage::Error StorageRam::commit_impl()
{
    if (write_context_.owner != this || write_context_.staging < 0 || write_context_.committed) {
        return storage::Error::CommitFailed;
    }
    Entry &staging = entries_[write_context_.staging];
    if (write_context_.target >= 0) {
        Entry &target = entries_[write_context_.target];
        release_block(target.block);
        target.block = staging.block;
        target.size = staging.size;
        staging.block = -1;
        staging.used = false;
    } else {
        staging.committed = true;
    }
    write_context_.committed = true;
    write_context_ = WriteContext{};
    return storage::Error::Ok;
}

void StorageRam::abort_impl()
{
    if (write_context_.owner != this) return;
    if (write_context_.staging >= 0 && entries_[write_context_.staging].used) {
        release_block(entries_[write_context_.staging].block);
        entries_[write_context_.staging] = Entry{};
    }
    write_context_ = WriteContext{};
}

} // namespace storage_ram
