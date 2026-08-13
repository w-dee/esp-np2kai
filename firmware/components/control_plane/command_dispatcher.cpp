#include "command_dispatcher.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

#include "binary_data_plane/binary_data_plane.hpp"

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
    return static_cast<binary_data_plane::TransferManager *>(request.context);
}

bool read_u64(const cJSON *params, const char *name, std::uint64_t *value)
{
    const cJSON *item = params == nullptr ? nullptr :
        cJSON_GetObjectItemCaseSensitive(params, name);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        item->valuedouble < 0 ||
        std::floor(item->valuedouble) != item->valuedouble ||
        item->valuedouble > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
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
        binary_data_plane::begin_tx(manager, size, &info) :
        binary_data_plane::begin_rx(manager, size, &info);
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
