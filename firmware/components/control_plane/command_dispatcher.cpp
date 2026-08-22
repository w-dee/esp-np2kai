#include "command_dispatcher.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "binary_data_plane/binary_data_plane.hpp"
#include "file_transfer/file_transfer.hpp"

namespace control_plane::dispatcher {
namespace {

Result failure(const char *code, const char *message)
{
    return Result{false, nullptr, code, message};
}

bool add_string(cJSON *object, const char *name, const char *value)
{
    return cJSON_AddStringToObject(object, name, value == nullptr ? "" : value) != nullptr;
}

cJSON *hello_result(const Request &request)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *capabilities = cJSON_CreateArray();
    if (result == nullptr || capabilities == nullptr) {
        cJSON_Delete(result);
        cJSON_Delete(capabilities);
        return nullptr;
    }

    cJSON_AddNumberToObject(result, "protocol_version", 1);
    if (!add_string(result, "project", request.metadata->project) ||
        !add_string(result, "target", request.metadata->target) ||
        cJSON_AddItemToObject(result, "capabilities", capabilities) == false) {
        cJSON_Delete(result);
        return nullptr;
    }

    const char *names[] = {
        "protocol.hello",
        "system.ping",
        "system.info",
        "binary.data-plane.v1",
        "file-transfer.v1",
        "file-transfer.zero-rle-v1",
    };
    for (const char *name : names) {
        if (cJSON_AddItemToArray(capabilities, cJSON_CreateString(name)) == false) {
            cJSON_Delete(result);
            return nullptr;
        }
    }
    return result;
}

binary_data_plane::TransferManager *binary_manager(const Request &request)
{
    const auto *context = static_cast<const ServiceContext *>(request.context);
    return context == nullptr ? nullptr : context->binary;
}

file_transfer::Service *file_service(const Request &request)
{
    const auto *context = static_cast<const ServiceContext *>(request.context);
    return context == nullptr ? nullptr : context->file;
}

bool read_u64(const cJSON *params, const char *name, std::uint64_t *value)
{
    constexpr double kMaxExactJsonInteger = 9007199254740991.0; // 2^53 - 1
    const cJSON *item = params == nullptr ? nullptr :
        cJSON_GetObjectItemCaseSensitive(params, name);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        item->valuedouble < 0 ||
        std::floor(item->valuedouble) != item->valuedouble ||
        item->valuedouble > kMaxExactJsonInteger) {
        return false;
    }
    *value = static_cast<std::uint64_t>(item->valuedouble);
    return true;
}

bool read_optional_size(const cJSON *params, std::uint64_t *size)
{
    if (params == nullptr) {
        *size = binary_data_plane::kTestTransferBytes;
        return true;
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(params, "size_bytes");
    if (item == nullptr) {
        *size = binary_data_plane::kTestTransferBytes;
        return true;
    }
    return read_u64(params, "size_bytes", size);
}

bool add_transfer_info(cJSON *result, const binary_data_plane::TransferInfo &info)
{
    if (cJSON_AddNumberToObject(result, "transfer_id", info.transfer_id) == nullptr ||
        cJSON_AddStringToObject(result, "direction",
                                binary_data_plane::direction_name(info.direction)) == nullptr ||
        cJSON_AddStringToObject(result, "state",
                                binary_data_plane::state_name(info.state)) == nullptr ||
        cJSON_AddNumberToObject(result, "size_bytes",
                                static_cast<double>(info.total_bytes)) == nullptr ||
        cJSON_AddNumberToObject(result, "transferred_bytes",
                                static_cast<double>(info.transferred_bytes)) == nullptr ||
        cJSON_AddNumberToObject(result, "expected_sequence", info.expected_sequence) == nullptr ||
        cJSON_AddNumberToObject(result, "expected_offset",
                                static_cast<double>(info.expected_offset)) == nullptr ||
        cJSON_AddNumberToObject(result, "chunk_size",
                                binary_data_plane::kMaxPayloadBytes) == nullptr ||
        cJSON_AddNumberToObject(result, "data_plane_version",
                                binary_data_plane::kDataPlaneVersion) == nullptr) {
        return false;
    }
    if (info.has_crc32 &&
        cJSON_AddNumberToObject(result, "crc32", info.crc32) == nullptr) {
        return false;
    }
    return true;
}

bool add_file_begin_info(cJSON *result, const file_transfer::BeginResult &begin)
{
    if (!add_transfer_info(result, begin.info)) return false;
    if (begin.encoding == file_transfer::Encoding::Raw) return true;
    if (cJSON_ReplaceItemInObject(result, "size_bytes",
                                  cJSON_CreateNumber(static_cast<double>(begin.logical_size_bytes))) == 0 ||
        cJSON_AddStringToObject(result, "encoding",
                                file_transfer::encoding_name(begin.encoding)) == nullptr ||
        cJSON_AddNumberToObject(result, "wire_size_bytes",
                                static_cast<double>(begin.wire_size_bytes)) == nullptr) {
        return false;
    }
    return true;
}

Result begin_dispatch(const Request &request, bool device_to_host)
{
    binary_data_plane::TransferManager *manager = binary_manager(request);
    if (manager == nullptr) {
        return failure("INTERNAL_ERROR", "binary transfer manager is unavailable");
    }
    std::uint64_t size = 0;
    if (!read_optional_size(request.params, &size)) {
        return failure("INVALID_PARAMS", "size_bytes must be a non-negative integer");
    }
    binary_data_plane::TransferInfo info{};
    const binary_data_plane::ManagerError error = device_to_host ?
        binary_data_plane::begin_test_tx(manager, size, &info) :
        binary_data_plane::begin_test_rx(manager, size, &info);
    if (error != binary_data_plane::ManagerError::Ok) {
        return failure(binary_data_plane::error_code(error),
                       binary_data_plane::error_message(error));
    }
    cJSON *result = cJSON_CreateObject();
    if (result == nullptr || !add_transfer_info(result, info)) {
        cJSON_Delete(result);
        return failure("INTERNAL_ERROR", "unable to allocate transfer result");
    }
    return Result{true, result, nullptr, nullptr};
}

Result status_dispatch(const Request &request)
{
    binary_data_plane::TransferManager *manager = binary_manager(request);
    std::uint64_t id = 0;
    if (manager == nullptr) {
        return failure("INTERNAL_ERROR", "binary transfer manager is unavailable");
    }
    if (!read_u64(request.params, "transfer_id", &id) ||
        id > std::numeric_limits<std::uint32_t>::max()) {
        return failure("INVALID_PARAMS", "transfer_id must be a non-negative 32-bit integer");
    }
    binary_data_plane::TransferInfo info{};
    const binary_data_plane::ManagerError error =
        binary_data_plane::get_status(manager, static_cast<std::uint32_t>(id), &info);
    if (error != binary_data_plane::ManagerError::Ok) {
        return failure(binary_data_plane::error_code(error),
                       binary_data_plane::error_message(error));
    }
    cJSON *result = cJSON_CreateObject();
    if (result == nullptr || !add_transfer_info(result, info)) {
        cJSON_Delete(result);
        return failure("INTERNAL_ERROR", "unable to allocate transfer status");
    }
    return Result{true, result, nullptr, nullptr};
}

Result abort_dispatch(const Request &request)
{
    binary_data_plane::TransferManager *manager = binary_manager(request);
    std::uint64_t id = 0;
    if (manager == nullptr) {
        return failure("INTERNAL_ERROR", "binary transfer manager is unavailable");
    }
    if (!read_u64(request.params, "transfer_id", &id) ||
        id > std::numeric_limits<std::uint32_t>::max()) {
        return failure("INVALID_PARAMS", "transfer_id must be a non-negative 32-bit integer");
    }
    binary_data_plane::TransferInfo info{};
    const binary_data_plane::ManagerError error =
        binary_data_plane::abort(manager, static_cast<std::uint32_t>(id), &info);
    if (error != binary_data_plane::ManagerError::Ok) {
        return failure(binary_data_plane::error_code(error),
                       binary_data_plane::error_message(error));
    }
    cJSON *result = cJSON_CreateObject();
    if (result == nullptr || !add_transfer_info(result, info)) {
        cJSON_Delete(result);
        return failure("INTERNAL_ERROR", "unable to allocate transfer status");
    }
    return Result{true, result, nullptr, nullptr};
}

cJSON *ping_result()
{
    cJSON *result = cJSON_CreateObject();
    if (result == nullptr || cJSON_AddTrueToObject(result, "pong") == nullptr) {
        cJSON_Delete(result);
        return nullptr;
    }
    return result;
}

cJSON *info_result(const Request &request)
{
    cJSON *result = cJSON_CreateObject();
    if (result == nullptr ||
        !add_string(result, "project", request.metadata->project) ||
        !add_string(result, "firmware_version", request.metadata->firmware_version) ||
        !add_string(result, "idf_version", request.metadata->idf_version) ||
        !add_string(result, "target", request.metadata->target)) {
        cJSON_Delete(result);
        return nullptr;
    }
    return result;
}

bool read_string(const cJSON *params, const char *name, std::string_view *value)
{
    const cJSON *item = params == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(params, name);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) return false;
    *value = item->valuestring;
    return true;
}

bool read_bool_optional(const cJSON *params, const char *name, bool default_value, bool *value)
{
    if (params == nullptr) { *value = default_value; return true; }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(params, name);
    if (item == nullptr) { *value = default_value; return true; }
    if (!cJSON_IsBool(item)) return false;
    *value = cJSON_IsTrue(item);
    return true;
}

std::size_t json_escaped_size(std::string_view value)
{
    std::size_t size = 0;
    for (const unsigned char byte : value) {
        size += byte == '"' || byte == '\\' ? 2 : 1;
    }
    return size;
}

std::size_t decimal_size(std::uint64_t value)
{
    std::size_t size = 1;
    while (value >= 10) {
        value /= 10;
        ++size;
    }
    return size;
}

Result file_stat_dispatch(const Request &request)
{
    file_transfer::Service *service = file_service(request);
    std::string_view path;
    if (service == nullptr) return failure("INTERNAL_ERROR", "file transfer service is unavailable");
    if (!read_string(request.params, "path", &path)) return failure("INVALID_PARAMS", "path must be a string");
    storage::Metadata metadata{};
    const file_transfer::Error error = service->stat(path, &metadata);
    if (error != file_transfer::Error::Ok) return failure(file_transfer::error_code(error), "file.stat failed");
    cJSON *result = cJSON_CreateObject();
    if (result == nullptr || cJSON_AddStringToObject(result, "path", path.data()) == nullptr ||
        cJSON_AddStringToObject(result, "type", metadata.type == storage::EntryType::File ? "file" : "directory") == nullptr ||
        (metadata.type == storage::EntryType::File &&
         cJSON_AddNumberToObject(result, "size_bytes", static_cast<double>(metadata.size_bytes)) == nullptr)) {
        cJSON_Delete(result);
        return failure("INTERNAL_ERROR", "unable to allocate stat result");
    }
    return Result{true, result, nullptr, nullptr};
}

Result file_sha256_dispatch(const Request &request)
{
    file_transfer::Service *service = file_service(request);
    std::string_view path;
    if (service == nullptr) return failure("INTERNAL_ERROR", "file transfer service is unavailable");
    if (!read_string(request.params, "path", &path)) return failure("INVALID_PARAMS", "path must be a string");
    file_transfer::Sha256Result digest{};
    const file_transfer::Error error = service->sha256(path, &digest);
    if (error != file_transfer::Error::Ok) return failure(file_transfer::error_code(error), "file.sha256 failed");
    cJSON *result = cJSON_CreateObject();
    if (result == nullptr ||
        cJSON_AddStringToObject(result, "path", path.data()) == nullptr ||
        cJSON_AddNumberToObject(result, "size_bytes", static_cast<double>(digest.size_bytes)) == nullptr ||
        cJSON_AddStringToObject(result, "sha256", digest.sha256) == nullptr) {
        cJSON_Delete(result);
        return failure("INTERNAL_ERROR", "unable to allocate sha256 result");
    }
    return Result{true, result, nullptr, nullptr};
}

Result file_list_dispatch(const Request &request)
{
    file_transfer::Service *service = file_service(request);
    std::string_view path;
    std::string_view cursor;
    std::uint64_t limit_value = 8;
    if (service == nullptr) return failure("INTERNAL_ERROR", "file transfer service is unavailable");
    if (!read_string(request.params, "path", &path)) return failure("INVALID_PARAMS", "path must be a string");
    const cJSON *cursor_item = request.params == nullptr ? nullptr :
        cJSON_GetObjectItemCaseSensitive(request.params, "cursor");
    if (cursor_item != nullptr && (!cJSON_IsString(cursor_item) || cursor_item->valuestring == nullptr)) {
        return failure("INVALID_PARAMS", "cursor must be a string");
    }
    if (cursor_item != nullptr) cursor = cursor_item->valuestring;
    if (request.params != nullptr && cJSON_GetObjectItemCaseSensitive(request.params, "limit") != nullptr &&
        !read_u64(request.params, "limit", &limit_value)) {
        return failure("INVALID_PARAMS", "limit must be an integer");
    }
    file_transfer::ListPage page{};
    const file_transfer::Error error = service->list(path, cursor, static_cast<std::size_t>(limit_value), &page);
    if (error != file_transfer::Error::Ok) return failure(file_transfer::error_code(error), "file.list failed");
    cJSON *result = cJSON_CreateObject();
    cJSON *entries = cJSON_CreateArray();
    if (result == nullptr || entries == nullptr || cJSON_AddItemToObject(result, "entries", entries) == 0) {
        cJSON_Delete(result); cJSON_Delete(entries);
        return failure("INTERNAL_ERROR", "unable to allocate list result");
    }
    // Reserve space for the response envelope and the trailing cursor/done fields.
    // The central writer still enforces the absolute 1024-byte frame bound.
    constexpr std::size_t kResponseBudget = 768;
    constexpr std::size_t kEnvelopeAndPageFields = 160;
    std::size_t estimated = kEnvelopeAndPageFields;
    std::size_t emitted = 0;
    for (; emitted < page.count; ++emitted) {
        const storage::DirectoryEntry &candidate = page.entries[emitted];
        const std::size_t name_size = json_escaped_size(candidate.name);
        const std::size_t entry_size = 24 + name_size +
            (candidate.metadata.type == storage::EntryType::File ?
             31 + decimal_size(candidate.metadata.size_bytes) : 22) +
            (emitted == 0 ? 0 : 1);
        const std::size_t cursor_size = 18 + name_size;
        if (estimated + entry_size + cursor_size > kResponseBudget) break;
        estimated += entry_size;
        cJSON *entry = cJSON_CreateObject();
        if (entry == nullptr || cJSON_AddStringToObject(entry, "name", candidate.name) == nullptr ||
            cJSON_AddStringToObject(entry, "type", candidate.metadata.type == storage::EntryType::File ? "file" : "directory") == nullptr ||
            (candidate.metadata.type == storage::EntryType::File &&
             cJSON_AddNumberToObject(entry, "size_bytes", static_cast<double>(candidate.metadata.size_bytes)) == nullptr) ||
            cJSON_AddItemToArray(entries, entry) == 0) {
            cJSON_Delete(entry); cJSON_Delete(result);
            return failure("INTERNAL_ERROR", "unable to allocate list entry");
        }
    }
    if (emitted == 0 && page.count != 0) {
        cJSON_Delete(result);
        return failure("RESPONSE_TOO_LARGE", "directory entry exceeds response budget");
    }
    const bool has_more = page.has_more || emitted < page.count;
    bool completed = false;
    if (has_more) {
        completed = cJSON_AddStringToObject(
            result, "next_cursor", emitted == 0 ? cursor.data() : page.entries[emitted - 1].name) != nullptr &&
            cJSON_AddFalseToObject(result, "done") != nullptr;
    } else {
        completed = cJSON_AddNullToObject(result, "next_cursor") != nullptr &&
            cJSON_AddTrueToObject(result, "done") != nullptr;
    }
    if (!completed) {
        cJSON_Delete(result);
        return failure("INTERNAL_ERROR", "unable to allocate list continuation");
    }
    return Result{true, result, nullptr, nullptr};
}

Result file_begin_dispatch(const Request &request, bool write)
{
    file_transfer::Service *service = file_service(request);
    std::string_view path;
    if (service == nullptr) return failure("INTERNAL_ERROR", "file transfer service is unavailable");
    if (!read_string(request.params, "path", &path)) return failure("INVALID_PARAMS", "path must be a string");
    file_transfer::BeginResult begin{};
    file_transfer::Error error = file_transfer::Error::InternalError;
    if (write) {
        std::uint64_t size = 0;
        std::uint64_t wire_size = 0;
        bool replace = false;
        file_transfer::Encoding encoding = file_transfer::Encoding::Raw;
        if (!read_u64(request.params, "size_bytes", &size) ||
            !read_bool_optional(request.params, "replace", false, &replace)) {
            return failure("INVALID_PARAMS", "size_bytes or replace is invalid");
        }
        const cJSON *encoding_item = request.params == nullptr ? nullptr :
            cJSON_GetObjectItemCaseSensitive(request.params, "encoding");
        if (encoding_item != nullptr) {
            if (!cJSON_IsString(encoding_item) || encoding_item->valuestring == nullptr) {
                return failure("INVALID_PARAMS", "encoding must be a string");
            }
            if (std::strcmp(encoding_item->valuestring, "raw") == 0) {
                encoding = file_transfer::Encoding::Raw;
            } else if (std::strcmp(encoding_item->valuestring, "zero-rle-v1") == 0) {
                encoding = file_transfer::Encoding::ZeroRleV1;
            } else {
                return failure("UNSUPPORTED", "file transfer encoding is unsupported");
            }
        }
        const cJSON *wire_item = request.params == nullptr ? nullptr :
            cJSON_GetObjectItemCaseSensitive(request.params, "wire_size_bytes");
        if (encoding == file_transfer::Encoding::ZeroRleV1) {
            if (!read_u64(request.params, "wire_size_bytes", &wire_size)) {
                return failure("INVALID_PARAMS", "wire_size_bytes is required for compressed writes");
            }
        } else if (wire_item != nullptr) {
            if (!read_u64(request.params, "wire_size_bytes", &wire_size) || wire_size != size) {
                return failure("INVALID_PARAMS", "raw wire_size_bytes must equal size_bytes");
            }
        } else {
            wire_size = size;
        }
        error = service->begin_write(path,
                                     file_transfer::WriteOptions{size, wire_size, replace, encoding},
                                     &begin);
    } else {
        std::uint64_t offset = 0;
        std::uint64_t length = 0;
        const bool has_offset = request.params != nullptr && cJSON_GetObjectItemCaseSensitive(request.params, "offset_bytes") != nullptr;
        const bool has_length = request.params != nullptr && cJSON_GetObjectItemCaseSensitive(request.params, "length_bytes") != nullptr;
        if ((has_offset && !read_u64(request.params, "offset_bytes", &offset)) ||
            (has_length && !read_u64(request.params, "length_bytes", &length))) {
            return failure("INVALID_PARAMS", "offset_bytes or length_bytes is invalid");
        }
        error = service->begin_read(path, offset, has_length, length, &begin);
    }
    if (error != file_transfer::Error::Ok) return failure(file_transfer::error_code(error), "file transfer begin failed");
    cJSON *result = cJSON_CreateObject();
    if (result == nullptr) return failure("INTERNAL_ERROR", "unable to allocate transfer result");
    if (begin.synchronous) {
        cJSON_AddNullToObject(result, "transfer_id");
        cJSON_AddStringToObject(result, "state", "completed");
        cJSON_AddNumberToObject(result, "size_bytes", static_cast<double>(begin.logical_size_bytes));
        if (begin.encoding != file_transfer::Encoding::Raw &&
            (cJSON_AddStringToObject(result, "encoding",
                                     file_transfer::encoding_name(begin.encoding)) == nullptr ||
             cJSON_AddNumberToObject(result, "wire_size_bytes",
                                     static_cast<double>(begin.wire_size_bytes)) == nullptr)) {
            cJSON_Delete(result);
            return failure("INTERNAL_ERROR", "unable to allocate transfer result");
        }
    } else if (!add_file_begin_info(result, begin)) {
        cJSON_Delete(result);
        return failure("INTERNAL_ERROR", "unable to allocate transfer result");
    }
    if (!write && cJSON_AddNumberToObject(result, "file_offset_bytes", static_cast<double>(begin.file_offset)) == nullptr) {
        cJSON_Delete(result);
        return failure("INTERNAL_ERROR", "unable to allocate range result");
    }
    return Result{true, result, nullptr, nullptr};
}

Result file_status_dispatch(const Request &request)
{
    file_transfer::Service *service = file_service(request);
    std::uint64_t id = 0;
    if (service == nullptr) return failure("INTERNAL_ERROR", "file transfer service is unavailable");
    if (!read_u64(request.params, "transfer_id", &id) || id > 0xffffffffu) {
        return failure("INVALID_PARAMS", "transfer_id must be a 32-bit integer");
    }
    file_transfer::Summary summary{};
    const file_transfer::Error error = service->status(static_cast<std::uint32_t>(id), &summary);
    if (error != file_transfer::Error::Ok) return failure(file_transfer::error_code(error), "file transfer is not known");
    cJSON *result = cJSON_CreateObject();
    if (result == nullptr || cJSON_AddNumberToObject(result, "transfer_id", summary.transfer_id) == nullptr ||
        cJSON_AddStringToObject(result, "path", summary.path) == nullptr ||
        cJSON_AddStringToObject(result, "direction", binary_data_plane::direction_name(summary.direction)) == nullptr ||
        cJSON_AddStringToObject(result, "transport_state", binary_data_plane::state_name(summary.transport_state)) == nullptr ||
        cJSON_AddStringToObject(result, "file_state", file_transfer::file_state_name(summary.file_state)) == nullptr ||
        cJSON_AddNumberToObject(result, "size_bytes", static_cast<double>(summary.size_bytes)) == nullptr ||
        cJSON_AddNumberToObject(result, "transferred_bytes", static_cast<double>(summary.transferred_bytes)) == nullptr) {
        cJSON_Delete(result);
        return failure("INTERNAL_ERROR", "unable to allocate file status");
    }
    if (summary.encoding != file_transfer::Encoding::Raw &&
        (cJSON_AddStringToObject(result, "encoding",
                                 file_transfer::encoding_name(summary.encoding)) == nullptr ||
         cJSON_AddNumberToObject(result, "wire_size_bytes",
                                 static_cast<double>(summary.wire_size_bytes)) == nullptr ||
         cJSON_AddNumberToObject(result, "wire_transferred_bytes",
                                 static_cast<double>(summary.wire_transferred_bytes)) == nullptr)) {
        cJSON_Delete(result);
        return failure("INTERNAL_ERROR", "unable to allocate compressed file status");
    }
    if (summary.storage_error != storage::Error::Ok ||
        summary.codec_error != file_transfer::CodecError::None ||
        summary.transport_state == binary_data_plane::TransferState::Aborted) {
        cJSON *error_object = cJSON_CreateObject();
        const char *code = summary.codec_error != file_transfer::CodecError::None ?
            file_transfer::codec_error_code(summary.codec_error) :
            summary.storage_error != storage::Error::Ok ?
            storage::error_code(summary.storage_error) :
            file_transfer::terminal_error_code(summary.terminal_reason);
        const char *message = summary.codec_error != file_transfer::CodecError::None ?
            file_transfer::codec_error_message(summary.codec_error) :
            summary.storage_error != storage::Error::Ok ?
            "storage operation failed" :
            file_transfer::terminal_error_message(summary.terminal_reason);
        if (error_object == nullptr ||
            cJSON_AddStringToObject(error_object, "code", code) == nullptr ||
            cJSON_AddStringToObject(error_object, "message", message) == nullptr ||
            cJSON_AddItemToObject(result, "error", error_object) == 0) {
            cJSON_Delete(error_object);
            cJSON_Delete(result);
            return failure("INTERNAL_ERROR", "unable to allocate file error status");
        }
    }
    return Result{true, result, nullptr, nullptr};
}

using Handler = cJSON *(*)(const Request &request);

struct CommandDescriptor {
    const char *name;
    Handler handler;
};

constexpr CommandDescriptor kCommands[] = {
    {"protocol.hello", hello_result},
    {"system.ping", [](const Request &) { return ping_result(); }},
    {"system.info", info_result},
};

} // namespace

Result dispatch(const Request &request)
{
    if (request.command == "binary.test.rx.begin") {
        return begin_dispatch(request, false);
    }
    if (request.command == "binary.test.tx.begin") {
        return begin_dispatch(request, true);
    }
    if (request.command == "binary.transfer.status") {
        return status_dispatch(request);
    }
    if (request.command == "binary.transfer.abort") {
        return abort_dispatch(request);
    }
    if (request.command == "file.stat") return file_stat_dispatch(request);
    if (request.command == "file.sha256") return file_sha256_dispatch(request);
    if (request.command == "file.list") return file_list_dispatch(request);
    if (request.command == "file.read.begin") return file_begin_dispatch(request, false);
    if (request.command == "file.write.begin") return file_begin_dispatch(request, true);
    if (request.command == "file.transfer.status") return file_status_dispatch(request);
    for (const CommandDescriptor &command : kCommands) {
        if (request.command == command.name) {
            cJSON *value = command.handler(request);
            if (value == nullptr) {
                return failure("INTERNAL_ERROR", "unable to allocate command result");
            }
            return Result{true, value, nullptr, nullptr};
        }
    }
    return failure("UNKNOWN_COMMAND", "command is not registered");
}

} // namespace control_plane::dispatcher
