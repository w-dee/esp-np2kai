#include "file_transfer/file_transfer.hpp"

#include <cstring>

#include "file_transfer/path.hpp"

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
                           std::uint64_t size)
{
    terminal = Summary{};
    current = Summary{};
    current.valid = true;
    current.direction = direction;
    current.size_bytes = size;
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
        return Error::Ok;
    }
    endpoint = EndpointContext{};
    endpoint.service = this;
    endpoint.write = false;
    endpoint.file_offset = offset;
    endpoint.size = selected;
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
    copy_current(path_value, binary_data_plane::Direction::DeviceToHost, selected);
    current.transfer_id = info.transfer_id;
    result->info = info;
    result->synchronous = false;
    result->file_offset = offset;
    return Error::Ok;
}

Error Service::begin_write(std::string_view path_value, std::uint64_t size,
                           bool replace, BeginResult *result)
{
    if (result == nullptr) return Error::InternalError;
    if (!path::validate(path_value, false)) return Error::InvalidPath;
    if (active() || binary == nullptr) return Error::Busy;
    if (size > limits.max_file_bytes) return Error::NoSpace;
    if (size == 0) {
        if (storage.begin_write == nullptr) return Error::InternalError;
        storage::WriteSession session{};
        const storage::Error open = storage.begin_write(storage.context, path_value, 0, replace, &session);
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
        return Error::Ok;
    }
    endpoint = EndpointContext{};
    endpoint.service = this;
    endpoint.write = true;
    endpoint.replace = replace;
    endpoint.size = size;
    std::memcpy(endpoint.path, path_value.data(), path_value.size());
    endpoint.path[path_value.size()] = '\0';
    binary_data_plane::TransferInfo info{};
    const binary_data_plane::ManagerError manager_error =
        binary_data_plane::begin_rx(binary, size, make_endpoint(), &info);
    if (manager_error != binary_data_plane::ManagerError::Ok) {
        return endpoint.storage_error != storage::Error::Ok ?
            map_storage(endpoint.storage_error) :
            (manager_error == binary_data_plane::ManagerError::Busy ? Error::Busy : Error::InternalError);
    }
    copy_current(path_value, binary_data_plane::Direction::HostToDevice, size);
    current.transfer_id = info.transfer_id;
    result->info = info;
    result->synchronous = false;
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
    summary->transferred_bytes = info.transferred_bytes;
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
                                                                   path_value, ctx->size, ctx->replace, &ctx->write_session);
        ctx->storage_error = error;
        ctx->session_open = error == storage::Error::Ok;
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
    const storage::Error error = ctx->write_session.write(ctx->write_session.context, offset, data, length);
    if (error != storage::Error::Ok) {
        ctx->storage_error = error;
        ctx->failed = true;
        return binary_data_plane::EndpointResult::Failed;
    }
    return binary_data_plane::EndpointResult::Ok;
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
    summary.transferred_bytes = service->binary->terminal.transferred_bytes;
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
    summary.terminal_reason = reason;
    summary.valid = true;
    service->terminal = summary;
    service->current.valid = false;
}

} // namespace file_transfer
