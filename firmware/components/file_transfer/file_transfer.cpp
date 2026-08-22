#include "file_transfer/file_transfer.hpp"

#include <algorithm>
#include <cstring>

#include "file_transfer/path.hpp"
#include "mbedtls/sha256.h"

namespace {

constexpr std::size_t kSha256ChunkBytes = 4096;
constexpr std::size_t kZeroRunBufferBytes = 4096;
constexpr char kHex[] = "0123456789abcdef";
alignas(4) std::uint8_t zero_run_buffer[kZeroRunBufferBytes]{};

void format_sha256(const std::uint8_t digest[32], char out[65])
{
    for (std::size_t index = 0; index < 32; ++index) {
        out[index * 2] = kHex[digest[index] >> 4];
        out[index * 2 + 1] = kHex[digest[index] & 0x0f];
    }
    out[64] = '\0';
}

} // namespace

namespace file_transfer {

const char *error_code(Error error)
{
    switch (error) {
    case Error::Ok: return "OK";
    case Error::InvalidPath: return "INVALID_PATH";
    case Error::Busy: return "BUSY";
    case Error::NotFound: return "NOT_FOUND";
    case Error::AlreadyExists: return "ALREADY_EXISTS";
    case Error::NotAFile: return "NOT_A_FILE";
    case Error::NotADirectory: return "NOT_A_DIRECTORY";
    case Error::ParentNotFound: return "PARENT_NOT_FOUND";
    case Error::NoSpace: return "NO_SPACE";
    case Error::ReadFailed: return "READ_FAILED";
    case Error::WriteFailed: return "WRITE_FAILED";
    case Error::CommitFailed: return "COMMIT_FAILED";
    case Error::OutOfRange: return "OUT_OF_RANGE";
    case Error::Unsupported: return "UNSUPPORTED";
    case Error::ResponseTooLarge: return "RESPONSE_TOO_LARGE";
    case Error::UnknownTransfer: return "UNKNOWN_TRANSFER";
    case Error::InternalError: return "INTERNAL_ERROR";
    }
    return "INTERNAL_ERROR";
}

const char *encoding_name(Encoding encoding)
{
    switch (encoding) {
    case Encoding::Raw: return "raw";
    case Encoding::ZeroRleV1: return "zero-rle-v1";
    }
    return "raw";
}

const char *transport_name(binary_data_plane::TransportMode transport)
{
    switch (transport) {
    case binary_data_plane::TransportMode::StopAndWait: return "stop-and-wait-v1";
    case binary_data_plane::TransportMode::WindowedGbnV1: return "windowed-gbn-v1";
    }
    return "stop-and-wait-v1";
}

const char *codec_error_code(CodecError error)
{
    switch (error) {
    case CodecError::None: return "OK";
    case CodecError::MalformedEncoding: return "MALFORMED_ENCODING";
    }
    return "MALFORMED_ENCODING";
}

const char *codec_error_message(CodecError error)
{
    switch (error) {
    case CodecError::None: return "encoding is valid";
    case CodecError::MalformedEncoding: return "zero-rle-v1 stream is malformed";
    }
    return "file transfer encoding is malformed";
}

const char *file_state_name(Summary::FileState state)
{
    switch (state) {
    case Summary::FileState::Active: return "active";
    case Summary::FileState::Completed: return "completed";
    case Summary::FileState::Failed: return "failed";
    case Summary::FileState::Aborted: return "aborted";
    }
    return "unknown";
}

const char *terminal_error_code(binary_data_plane::TerminalReason reason)
{
    switch (reason) {
    case binary_data_plane::TerminalReason::Completed: return "OK";
    case binary_data_plane::TerminalReason::ExplicitAbort: return "TRANSFER_ABORTED";
    case binary_data_plane::TerminalReason::ReceiverTimeout: return "RECEIVER_TIMEOUT";
    case binary_data_plane::TerminalReason::RetryExhausted: return "RETRY_EXHAUSTED";
    case binary_data_plane::TerminalReason::ProtocolError: return "PROTOCOL_ERROR";
    case binary_data_plane::TerminalReason::OutputError: return "OUTPUT_ERROR";
    case binary_data_plane::TerminalReason::EndpointIoError: return "ENDPOINT_IO_ERROR";
    case binary_data_plane::TerminalReason::EndpointFinishError: return "ENDPOINT_FINISH_ERROR";
    }
    return "INTERNAL_ERROR";
}

const char *terminal_error_message(binary_data_plane::TerminalReason reason)
{
    switch (reason) {
    case binary_data_plane::TerminalReason::Completed: return "transfer completed";
    case binary_data_plane::TerminalReason::ExplicitAbort: return "transfer was explicitly aborted";
    case binary_data_plane::TerminalReason::ReceiverTimeout: return "receiver activity timed out";
    case binary_data_plane::TerminalReason::RetryExhausted: return "retransmission budget was exhausted";
    case binary_data_plane::TerminalReason::ProtocolError: return "binary transport protocol error";
    case binary_data_plane::TerminalReason::OutputError: return "machine output failed";
    case binary_data_plane::TerminalReason::EndpointIoError: return "storage data operation failed";
    case binary_data_plane::TerminalReason::EndpointFinishError: return "storage finalization failed";
    }
    return "file transfer failed";
}

void Service::init(storage::Storage backend, binary_data_plane::TransferManager *manager,
                   Limits configured_limits)
{
    storage = backend;
    binary = manager;
    limits = configured_limits;
    current = Summary{};
    terminal = Summary{};
    endpoint = EndpointContext{};
}

bool Service::active() const
{
    return binary != nullptr && binary->active;
}

Error Service::map_storage(storage::Error error) const
{
    switch (error) {
    case storage::Error::Ok: return Error::Ok;
    case storage::Error::NotFound: return Error::NotFound;
    case storage::Error::AlreadyExists: return Error::AlreadyExists;
    case storage::Error::InvalidPath: return Error::InvalidPath;
    case storage::Error::NotAFile: return Error::NotAFile;
    case storage::Error::NotADirectory: return Error::NotADirectory;
    case storage::Error::ParentNotFound: return Error::ParentNotFound;
    case storage::Error::NoSpace: return Error::NoSpace;
    case storage::Error::ReadFailed: return Error::ReadFailed;
    case storage::Error::WriteFailed: return Error::WriteFailed;
    case storage::Error::CommitFailed: return Error::CommitFailed;
    case storage::Error::Busy: return Error::Busy;
    case storage::Error::OutOfRange: return Error::OutOfRange;
    case storage::Error::Unsupported: return Error::Unsupported;
    }
    return Error::InternalError;
}

Error Service::stat(std::string_view path_value, storage::Metadata *metadata) const
{
    if (!path::validate(path_value, true)) return Error::InvalidPath;
    if (storage.stat == nullptr) return Error::InternalError;
    return map_storage(storage.stat(storage.context, path_value, metadata));
}

Error Service::sha256(std::string_view path_value, Sha256Result *result)
{
    if (result == nullptr) return Error::InternalError;
    *result = Sha256Result{};
    if (!path::validate(path_value, false)) return Error::InvalidPath;
    if (active()) return Error::Busy;
    if (storage.stat == nullptr || storage.begin_read == nullptr) {
        return Error::InternalError;
    }

    storage::Metadata metadata{};
    const Error stat_error = stat(path_value, &metadata);
    if (stat_error != Error::Ok) return stat_error;
    if (metadata.type != storage::EntryType::File) return Error::NotAFile;

    storage::ReadSession session{};
    const storage::Error open_error = storage.begin_read(
        storage.context, path_value, &session);
    if (open_error != storage::Error::Ok) return map_storage(open_error);

    Error operation = Error::Ok;
    mbedtls_sha256_context context;
    std::uint8_t digest[32]{};
    std::uint8_t buffer[kSha256ChunkBytes];

    mbedtls_sha256_init(&context);
    if (session.read == nullptr || session.close == nullptr) {
        operation = Error::ReadFailed;
    } else if (mbedtls_sha256_starts(&context, 0) != 0) {
        operation = Error::ReadFailed;
    } else {
        std::uint64_t offset = 0;
        while (offset < metadata.size_bytes) {
            const std::size_t requested = static_cast<std::size_t>(std::min<std::uint64_t>(
                metadata.size_bytes - offset, sizeof(buffer)));
            std::size_t produced = 0;
            const storage::Error read_error = session.read(
                session.context, offset, buffer, requested, &produced);
            if (read_error != storage::Error::Ok) {
                operation = map_storage(read_error);
                break;
            }
            if (produced != requested ||
                mbedtls_sha256_update(&context, buffer, produced) != 0) {
                operation = Error::ReadFailed;
                break;
            }
            offset += produced;
        }
        if (operation == Error::Ok && mbedtls_sha256_finish(&context, digest) != 0) {
            operation = Error::ReadFailed;
        }
    }

    mbedtls_sha256_free(&context);
    if (session.close != nullptr) session.close(session.context);
    if (operation != Error::Ok) return operation;

    result->size_bytes = metadata.size_bytes;
    format_sha256(digest, result->sha256);
    return Error::Ok;
}

Error Service::list(std::string_view path_value, std::string_view cursor,
                    std::size_t limit, ListPage *page) const
{
    if (active()) return Error::Busy;
    if (page == nullptr || !path::validate(path_value, true) ||
        (!cursor.empty() && !path::valid_component(cursor)) || limit == 0 || limit > 16) {
        return page == nullptr ? Error::InternalError : Error::InvalidPath;
    }
    if (storage.list_page == nullptr) return Error::InternalError;
    page->count = 0;
    page->has_more = false;
    return map_storage(storage.list_page(storage.context, path_value, cursor, limit,
                                         page->entries, 16, &page->count, &page->has_more));
}

void Service::copy_current(std::string_view path_value,
                           binary_data_plane::Direction direction,
                           Encoding encoding, std::uint64_t logical_size,
                           std::uint64_t wire_size)
{
    terminal = Summary{};
    current = Summary{};
    current.valid = true;
    current.direction = direction;
    current.size_bytes = logical_size;
    current.encoding = encoding;
    current.wire_size_bytes = wire_size;
    current.file_state = Summary::FileState::Active;
    if (path_value.size() <= storage::kMaxPathBytes) {
        std::memcpy(current.path, path_value.data(), path_value.size());
        current.path[path_value.size()] = '\0';
    }
}

binary_data_plane::TransferEndpoint Service::make_endpoint()
{
    return binary_data_plane::TransferEndpoint{
        &endpoint,
        endpoint_begin,
        endpoint_consume,
        endpoint_produce,
        endpoint_finish,
        endpoint_abort,
        endpoint_terminal,
    };
}

Error Service::begin_read(std::string_view path_value, std::uint64_t offset,
                          bool has_length, std::uint64_t length, BeginResult *result)
{
    if (result == nullptr) return Error::InternalError;
    if (!path::validate(path_value, false)) return Error::InvalidPath;
    if (active() || binary == nullptr) return Error::Busy;
    storage::Metadata metadata{};
    const Error stat_error = stat(path_value, &metadata);
    if (stat_error != Error::Ok) return stat_error;
    if (metadata.type != storage::EntryType::File) return Error::NotAFile;
    if (offset > metadata.size_bytes) return Error::OutOfRange;
    const std::uint64_t remaining = metadata.size_bytes - offset;
    const std::uint64_t selected = has_length ? length : remaining;
    if (selected > remaining) return Error::OutOfRange;
    if (selected == 0) {
        if (storage.begin_read == nullptr) return Error::InternalError;
        storage::ReadSession session{};
        const storage::Error open = storage.begin_read(storage.context, path_value, &session);
        if (open != storage::Error::Ok) return map_storage(open);
        if (session.close != nullptr) session.close(session.context);
        current = Summary{};
        terminal = Summary{};
        result->synchronous = true;
        result->file_offset = offset;
        result->info = binary_data_plane::TransferInfo{};
        result->info.direction = binary_data_plane::Direction::DeviceToHost;
        result->info.state = binary_data_plane::TransferState::Completed;
        result->info.total_bytes = 0;
        result->encoding = Encoding::Raw;
        result->logical_size_bytes = 0;
        result->wire_size_bytes = 0;
        return Error::Ok;
    }
    endpoint = EndpointContext{};
    endpoint.service = this;
    endpoint.write = false;
    endpoint.file_offset = offset;
    endpoint.logical_size = selected;
    endpoint.wire_size = selected;
    endpoint.encoding = Encoding::Raw;
    std::memcpy(endpoint.path, path_value.data(), path_value.size());
    endpoint.path[path_value.size()] = '\0';
    binary_data_plane::TransferInfo info{};
    const binary_data_plane::ManagerError manager_error =
        binary_data_plane::begin_tx(binary, selected, make_endpoint(), &info);
    if (manager_error != binary_data_plane::ManagerError::Ok) {
        return endpoint.storage_error != storage::Error::Ok ?
            map_storage(endpoint.storage_error) :
            (manager_error == binary_data_plane::ManagerError::Busy ? Error::Busy : Error::InternalError);
    }
    copy_current(path_value, binary_data_plane::Direction::DeviceToHost,
                 Encoding::Raw, selected, selected);
    current.transfer_id = info.transfer_id;
    result->info = info;
    result->synchronous = false;
    result->file_offset = offset;
    result->encoding = Encoding::Raw;
    result->logical_size_bytes = selected;
    result->wire_size_bytes = selected;
    return Error::Ok;
}

Error Service::begin_write(std::string_view path_value, const WriteOptions &options,
                           BeginResult *result)
{
    if (result == nullptr) return Error::InternalError;
    if (!path::validate(path_value, false)) return Error::InvalidPath;
    if (active() || binary == nullptr) return Error::Busy;
    if ((options.transport == binary_data_plane::TransportMode::StopAndWait &&
         options.window_frames != 1) ||
        (options.transport == binary_data_plane::TransportMode::WindowedGbnV1 &&
         options.window_frames != binary_data_plane::kMaxWindowFrames)) {
        return Error::Unsupported;
    }
    if (options.logical_size_bytes > limits.max_file_bytes ||
        options.wire_size_bytes > limits.max_file_bytes) return Error::NoSpace;
    if ((options.logical_size_bytes == 0) != (options.wire_size_bytes == 0)) {
        return Error::InvalidPath;
    }
    if (options.encoding == Encoding::Raw &&
        options.logical_size_bytes != options.wire_size_bytes) {
        return Error::InvalidPath;
    }
    if (options.logical_size_bytes == 0) {
        if (storage.begin_write == nullptr) return Error::InternalError;
        storage::WriteSession session{};
        const storage::Error open = storage.begin_write(storage.context, path_value, 0,
                                                        options.replace, &session);
        if (open != storage::Error::Ok) return map_storage(open);
        const storage::Error committed = session.commit == nullptr ? storage::Error::CommitFailed :
            session.commit(session.context);
        if (committed != storage::Error::Ok) {
            if (session.abort != nullptr) session.abort(session.context);
            return map_storage(committed);
        }
        current = Summary{};
        terminal = Summary{};
        result->synchronous = true;
        result->info = binary_data_plane::TransferInfo{};
        result->info.direction = binary_data_plane::Direction::HostToDevice;
        result->info.state = binary_data_plane::TransferState::Completed;
        result->encoding = options.encoding;
        result->logical_size_bytes = 0;
        result->wire_size_bytes = 0;
        result->transport = options.transport;
        result->window_frames = options.window_frames;
        return Error::Ok;
    }
    endpoint = EndpointContext{};
    endpoint.service = this;
    endpoint.write = true;
    endpoint.replace = options.replace;
    endpoint.logical_size = options.logical_size_bytes;
    endpoint.wire_size = options.wire_size_bytes;
    endpoint.encoding = options.encoding;
    std::memcpy(endpoint.path, path_value.data(), path_value.size());
    endpoint.path[path_value.size()] = '\0';
    binary_data_plane::TransferInfo info{};
    const binary_data_plane::ManagerError manager_error =
        binary_data_plane::begin_rx(
            binary, options.wire_size_bytes, make_endpoint(),
            binary_data_plane::TransferOptions{options.transport, options.window_frames}, &info);
    if (manager_error != binary_data_plane::ManagerError::Ok) {
        return endpoint.storage_error != storage::Error::Ok ?
            map_storage(endpoint.storage_error) :
            (manager_error == binary_data_plane::ManagerError::Busy ? Error::Busy : Error::InternalError);
    }
    copy_current(path_value, binary_data_plane::Direction::HostToDevice,
                 options.encoding, options.logical_size_bytes, options.wire_size_bytes);
    current.transfer_id = info.transfer_id;
    result->info = info;
    result->synchronous = false;
    result->encoding = options.encoding;
    result->logical_size_bytes = options.logical_size_bytes;
    result->wire_size_bytes = options.wire_size_bytes;
    result->transport = options.transport;
    result->window_frames = options.window_frames;
    return Error::Ok;
}

Error Service::status(std::uint32_t transfer_id, Summary *summary)
{
    if (summary == nullptr || binary == nullptr) return Error::InternalError;
    if (terminal.valid && terminal.transfer_id == transfer_id) {
        *summary = terminal;
        return Error::Ok;
    }
    if (!current.valid || current.transfer_id != transfer_id) return Error::UnknownTransfer;
    binary_data_plane::TransferInfo info{};
    if (binary_data_plane::get_status(binary, transfer_id, &info) != binary_data_plane::ManagerError::Ok) {
        return Error::UnknownTransfer;
    }
    *summary = current;
    summary->transport_state = info.state;
    summary->wire_transferred_bytes = info.transferred_bytes;
    summary->transferred_bytes = current.encoding == Encoding::ZeroRleV1 &&
        endpoint.decoder_initialized ? endpoint.decoder.logical_produced() : info.transferred_bytes;
    summary->crc32 = info.crc32;
    summary->has_crc32 = info.has_crc32;
    return Error::Ok;
}

binary_data_plane::EndpointResult Service::endpoint_begin(void *context,
                                                           binary_data_plane::Direction,
                                                           std::uint64_t)
{
    auto *ctx = static_cast<EndpointContext *>(context);
    if (ctx == nullptr || ctx->service == nullptr) return binary_data_plane::EndpointResult::Failed;
    Service *service = ctx->service;
    const std::string_view path_value(ctx->path);
    if (ctx->write) {
        if (service->storage.begin_write == nullptr) {
            ctx->storage_error = storage::Error::WriteFailed;
            return binary_data_plane::EndpointResult::Failed;
        }
        const storage::Error error = service->storage.begin_write(service->storage.context,
                                                                   path_value, ctx->logical_size,
                                                                   ctx->replace, &ctx->write_session);
        ctx->storage_error = error;
        ctx->session_open = error == storage::Error::Ok;
        if (ctx->session_open && ctx->encoding == Encoding::ZeroRleV1) {
            ctx->decoder.init(ctx->logical_size, ctx->wire_size, decoder_emit, ctx,
                              zero_run_buffer, sizeof(zero_run_buffer));
            ctx->decoder_initialized = true;
        }
        return error == storage::Error::Ok ? binary_data_plane::EndpointResult::Ok :
            binary_data_plane::EndpointResult::Failed;
    }
    if (service->storage.begin_read == nullptr) {
        ctx->storage_error = storage::Error::ReadFailed;
        return binary_data_plane::EndpointResult::Failed;
    }
    const storage::Error error = service->storage.begin_read(service->storage.context,
                                                             path_value, &ctx->read);
    ctx->storage_error = error;
    ctx->session_open = error == storage::Error::Ok;
    return error == storage::Error::Ok ? binary_data_plane::EndpointResult::Ok :
        binary_data_plane::EndpointResult::Failed;
}

binary_data_plane::EndpointResult Service::endpoint_consume(void *context,
                                                             std::uint64_t offset,
                                                             const std::uint8_t *data,
                                                             std::size_t length)
{
    auto *ctx = static_cast<EndpointContext *>(context);
    if (ctx == nullptr || !ctx->session_open || !ctx->write_session.write) return binary_data_plane::EndpointResult::Failed;
    // The binary transport offset is an encoded-stream offset.  The
    // zero-rle decoder separately tracks logical output offsets.
    if (ctx->encoding == Encoding::ZeroRleV1 &&
        (!ctx->decoder_initialized || offset != ctx->decoder.wire_consumed())) {
        ctx->codec_error = CodecError::MalformedEncoding;
        ctx->failed = true;
        return binary_data_plane::EndpointResult::Failed;
    }
    if (ctx->encoding == Encoding::ZeroRleV1) {
        const zero_rle_v1::Result result = ctx->decoder.consume(offset, data, length);
        if (result == zero_rle_v1::Result::Malformed) {
            ctx->codec_error = CodecError::MalformedEncoding;
            ctx->failed = true;
            return binary_data_plane::EndpointResult::Failed;
        }
        if (result == zero_rle_v1::Result::OutputFailed) {
            ctx->failed = true;
            return binary_data_plane::EndpointResult::Failed;
        }
        return binary_data_plane::EndpointResult::Ok;
    }
    const storage::Error error = ctx->write_session.write(ctx->write_session.context, offset, data, length);
    if (error != storage::Error::Ok) {
        ctx->storage_error = error;
        ctx->failed = true;
        return binary_data_plane::EndpointResult::Failed;
    }
    return binary_data_plane::EndpointResult::Ok;
}

bool Service::decoder_emit(void *context, std::uint64_t offset,
                           const std::uint8_t *data, std::size_t length)
{
    auto *ctx = static_cast<EndpointContext *>(context);
    if (ctx == nullptr || !ctx->session_open || !ctx->write_session.write) return false;
    const storage::Error error = ctx->write_session.write(ctx->write_session.context,
                                                          offset, data, length);
    if (error != storage::Error::Ok) {
        ctx->storage_error = error;
        ctx->failed = true;
        return false;
    }
    return true;
}

binary_data_plane::EndpointResult Service::endpoint_produce(void *context,
                                                             std::uint64_t offset,
                                                             std::uint8_t *out,
                                                             std::size_t requested,
                                                             std::size_t *produced)
{
    auto *ctx = static_cast<EndpointContext *>(context);
    if (ctx == nullptr || !ctx->session_open || !ctx->read.read) return binary_data_plane::EndpointResult::Failed;
    const storage::Error error = ctx->read.read(ctx->read.context,
                                                ctx->file_offset + offset, out,
                                                requested, produced);
    if (error != storage::Error::Ok || produced == nullptr || *produced != requested) {
        ctx->storage_error = error == storage::Error::Ok ? storage::Error::ReadFailed : error;
        ctx->failed = true;
        return binary_data_plane::EndpointResult::Failed;
    }
    return binary_data_plane::EndpointResult::Ok;
}

binary_data_plane::EndpointResult Service::endpoint_finish(void *context)
{
    auto *ctx = static_cast<EndpointContext *>(context);
    if (ctx == nullptr || !ctx->session_open) return binary_data_plane::EndpointResult::Failed;
    storage::Error error = storage::Error::Ok;
    if (ctx->write) {
        if (ctx->encoding == Encoding::ZeroRleV1 && ctx->decoder_initialized) {
            const zero_rle_v1::Result decoded = ctx->decoder.finish();
            if (decoded == zero_rle_v1::Result::Malformed) {
                ctx->codec_error = CodecError::MalformedEncoding;
                ctx->failed = true;
                if (ctx->write_session.abort != nullptr) ctx->write_session.abort(ctx->write_session.context);
                ctx->session_open = false;
                ctx->storage_error = storage::Error::Ok;
                ctx->finish_succeeded = false;
                return binary_data_plane::EndpointResult::Failed;
            }
            if (decoded == zero_rle_v1::Result::OutputFailed) {
                ctx->failed = true;
                if (ctx->write_session.abort != nullptr) ctx->write_session.abort(ctx->write_session.context);
                ctx->session_open = false;
                ctx->finish_succeeded = false;
                return binary_data_plane::EndpointResult::Failed;
            }
        }
        error = ctx->write_session.commit == nullptr ? storage::Error::CommitFailed :
            ctx->write_session.commit(ctx->write_session.context);
        if (error != storage::Error::Ok && ctx->write_session.abort != nullptr) {
            ctx->write_session.abort(ctx->write_session.context);
        }
    } else if (ctx->read.close != nullptr) {
        ctx->read.close(ctx->read.context);
    }
    ctx->session_open = false;
    ctx->storage_error = error;
    ctx->failed = error != storage::Error::Ok;
    ctx->finish_succeeded = error == storage::Error::Ok;
    return error == storage::Error::Ok ? binary_data_plane::EndpointResult::Ok :
        binary_data_plane::EndpointResult::Failed;
}

void Service::endpoint_abort(void *context, binary_data_plane::TerminalReason)
{
    auto *ctx = static_cast<EndpointContext *>(context);
    if (ctx == nullptr) return;
    if (ctx->session_open) {
        if (ctx->write && ctx->write_session.abort != nullptr) ctx->write_session.abort(ctx->write_session.context);
        if (!ctx->write && ctx->read.close != nullptr) ctx->read.close(ctx->read.context);
        ctx->session_open = false;
    }
    if (!ctx->failed) ctx->storage_error = storage::Error::Ok;
}

void Service::endpoint_terminal(void *context, binary_data_plane::TerminalReason reason)
{
    auto *ctx = static_cast<EndpointContext *>(context);
    if (ctx == nullptr || ctx->service == nullptr || ctx->service->binary == nullptr) return;
    Service *service = ctx->service;
    Summary summary = service->current;
    summary.transport_state = service->binary->terminal.state;
    summary.wire_transferred_bytes = service->binary->terminal.transferred_bytes;
    summary.transferred_bytes = ctx->encoding == Encoding::ZeroRleV1 && ctx->decoder_initialized ?
        ctx->decoder.logical_produced() : service->binary->terminal.transferred_bytes;
    summary.encoding = ctx->encoding;
    summary.size_bytes = ctx->logical_size;
    summary.wire_size_bytes = ctx->wire_size;
    summary.crc32 = service->binary->terminal.crc32;
    summary.has_crc32 = service->binary->terminal.has_crc32;
    if (ctx->finish_succeeded) {
        summary.file_state = Summary::FileState::Completed;
    } else if (ctx->failed) {
        summary.file_state = Summary::FileState::Failed;
    } else if (reason == binary_data_plane::TerminalReason::Completed) {
        summary.file_state = Summary::FileState::Completed;
    } else {
        summary.file_state = Summary::FileState::Aborted;
    }
    summary.storage_error = ctx->storage_error;
    summary.codec_error = ctx->codec_error;
    summary.terminal_reason = reason;
    summary.valid = true;
    service->terminal = summary;
    service->current.valid = false;
}

} // namespace file_transfer
