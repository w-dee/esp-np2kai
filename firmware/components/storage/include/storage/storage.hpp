#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace storage {

inline constexpr std::size_t kMaxPathBytes = 192;
inline constexpr std::size_t kMaxComponentBytes = 64;

enum class Error : std::uint8_t {
    Ok,
    NotFound,
    AlreadyExists,
    InvalidPath,
    NotAFile,
    NotADirectory,
    ParentNotFound,
    NoSpace,
    ReadFailed,
    WriteFailed,
    CommitFailed,
    Busy,
    OutOfRange,
    Unsupported,
};

enum class EntryType : std::uint8_t { File, Directory };

struct Metadata {
    EntryType type = EntryType::File;
    std::uint64_t size_bytes = 0;
};

struct DirectoryEntry {
    char name[kMaxComponentBytes + 1]{};
    Metadata metadata{};
};

struct ReadSession {
    void *context = nullptr;
    Error (*read)(void *, std::uint64_t, std::uint8_t *, std::size_t, std::size_t *) = nullptr;
    void (*close)(void *) = nullptr;
};

struct WriteSession {
    void *context = nullptr;
    Error (*write)(void *, std::uint64_t, const std::uint8_t *, std::size_t) = nullptr;
    Error (*commit)(void *) = nullptr;
    void (*abort)(void *) = nullptr;
};

struct Storage {
    void *context = nullptr;
    Error (*stat)(void *, std::string_view, Metadata *) = nullptr;
    Error (*list_page)(void *, std::string_view, std::string_view,
                       std::size_t, DirectoryEntry *, std::size_t,
                       std::size_t *, bool *) = nullptr;
    Error (*begin_read)(void *, std::string_view, ReadSession *) = nullptr;
    Error (*begin_write)(void *, std::string_view, std::uint64_t, bool,
                         WriteSession *) = nullptr;
};

const char *error_code(Error error);

} // namespace storage
