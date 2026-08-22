#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "binary_data_plane/binary_data_plane.hpp"
#include "file_transfer/zero_rle_v1.hpp"
#include "storage/storage.hpp"

namespace file_transfer {

enum class Error : std::uint8_t {
    Ok,
    InvalidPath,
    Busy,
    NotFound,
    AlreadyExists,
    NotAFile,
    NotADirectory,
    ParentNotFound,
    NoSpace,
    ReadFailed,
    WriteFailed,
    CommitFailed,
    OutOfRange,
    Unsupported,
    ResponseTooLarge,
    UnknownTransfer,
    InternalError,
};

enum class Encoding : std::uint8_t {
    Raw,
    ZeroRleV1,
};

enum class CodecError : std::uint8_t {
    None,
    MalformedEncoding,
};

const char *encoding_name(Encoding encoding);
const char *transport_name(binary_data_plane::TransportMode transport);
const char *codec_error_code(CodecError error);
const char *codec_error_message(CodecError error);

struct WriteOptions {
    std::uint64_t logical_size_bytes = 0;
    std::uint64_t wire_size_bytes = 0;
    bool replace = false;
    Encoding encoding = Encoding::Raw;
    binary_data_plane::TransportMode transport =
        binary_data_plane::TransportMode::StopAndWait;
    std::uint8_t window_frames = 1;
};

struct ListPage {
    storage::DirectoryEntry entries[16]{};
    std::size_t count = 0;
    bool has_more = false;
};

struct BeginResult {
    binary_data_plane::TransferInfo info{};
    bool synchronous = false;
    std::uint64_t file_offset = 0;
    Encoding encoding = Encoding::Raw;
    std::uint64_t logical_size_bytes = 0;
    std::uint64_t wire_size_bytes = 0;
    binary_data_plane::TransportMode transport =
        binary_data_plane::TransportMode::StopAndWait;
    std::uint8_t window_frames = 1;
};

struct Sha256Result {
    std::uint64_t size_bytes = 0;
    char sha256[65]{};
};

struct Summary {
    bool valid = false;
    std::uint32_t transfer_id = 0;
    char path[storage::kMaxPathBytes + 1]{};
    binary_data_plane::Direction direction = binary_data_plane::Direction::HostToDevice;
    binary_data_plane::TransferState transport_state = binary_data_plane::TransferState::Idle;
    enum class FileState : std::uint8_t { Active, Completed, Failed, Aborted } file_state = FileState::Active;
    std::uint64_t size_bytes = 0;
    std::uint64_t transferred_bytes = 0;
    Encoding encoding = Encoding::Raw;
    std::uint64_t wire_size_bytes = 0;
    std::uint64_t wire_transferred_bytes = 0;
    std::uint32_t crc32 = 0;
    bool has_crc32 = false;
    storage::Error storage_error = storage::Error::Ok;
    CodecError codec_error = CodecError::None;
    binary_data_plane::TerminalReason terminal_reason =
        binary_data_plane::TerminalReason::Completed;
};

const char *error_code(Error error);
const char *file_state_name(Summary::FileState state);
const char *terminal_error_code(binary_data_plane::TerminalReason reason);
const char *terminal_error_message(binary_data_plane::TerminalReason reason);

inline constexpr std::uint64_t kDefaultMaxFileBytes = 192 * 1024;

struct Limits {
    std::uint64_t max_file_bytes = kDefaultMaxFileBytes;
};

struct Service {
    storage::Storage storage{};
    binary_data_plane::TransferManager *binary = nullptr;
    Limits limits{};
    Summary current{};
    Summary terminal{};

    void init(storage::Storage backend, binary_data_plane::TransferManager *manager,
              Limits configured_limits = {});
    Error stat(std::string_view path, storage::Metadata *metadata) const;
    Error sha256(std::string_view path, Sha256Result *result);
    Error list(std::string_view path, std::string_view cursor, std::size_t limit,
               ListPage *page) const;
    Error begin_read(std::string_view path, std::uint64_t offset,
                     bool has_length, std::uint64_t length, BeginResult *result);
    Error begin_write(std::string_view path, const WriteOptions &options,
                      BeginResult *result);
    Error begin_write(std::string_view path, std::uint64_t size, bool replace,
                      BeginResult *result)
    {
        return begin_write(path, WriteOptions{size, size, replace, Encoding::Raw}, result);
    }
    Error status(std::uint32_t transfer_id, Summary *summary);
    bool active() const;

private:
    struct EndpointContext {
        Service *service = nullptr;
        bool write = false;
        bool replace = false;
        char path[storage::kMaxPathBytes + 1]{};
        std::uint64_t file_offset = 0;
        std::uint64_t logical_size = 0;
        std::uint64_t wire_size = 0;
        Encoding encoding = Encoding::Raw;
        storage::ReadSession read{};
        storage::WriteSession write_session{};
        zero_rle_v1::Decoder decoder{};
        storage::Error storage_error = storage::Error::Ok;
        CodecError codec_error = CodecError::None;
        bool decoder_initialized = false;
        bool session_open = false;
        bool finish_succeeded = false;
        bool failed = false;
    } endpoint{};

    static binary_data_plane::EndpointResult endpoint_begin(void *, binary_data_plane::Direction, std::uint64_t);
    static binary_data_plane::EndpointResult endpoint_consume(void *, std::uint64_t, const std::uint8_t *, std::size_t);
    static bool decoder_emit(void *, std::uint64_t, const std::uint8_t *, std::size_t);
    static binary_data_plane::EndpointResult endpoint_produce(void *, std::uint64_t, std::uint8_t *, std::size_t, std::size_t *);
    static binary_data_plane::EndpointResult endpoint_finish(void *);
    static void endpoint_abort(void *, binary_data_plane::TerminalReason);
    static void endpoint_terminal(void *, binary_data_plane::TerminalReason);
    binary_data_plane::TransferEndpoint make_endpoint();
    Error map_storage(storage::Error error) const;
    void copy_current(std::string_view path, binary_data_plane::Direction direction,
                      Encoding encoding, std::uint64_t logical_size,
                      std::uint64_t wire_size);
};

} // namespace file_transfer
