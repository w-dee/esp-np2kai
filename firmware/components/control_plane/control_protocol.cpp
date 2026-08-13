#include "control_protocol.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "cJSON.h"

#include "command_dispatcher.hpp"

namespace control_plane::protocol {
namespace {

constexpr std::size_t kPrefixLength = sizeof(kFramePrefix) - 1;
constexpr std::size_t kMaxJsonBytes = kMaxOutputFrameBytes - kPrefixLength - 1;

bool is_integer_number(const cJSON *item)
{
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble)) {
        return false;
    }
    return std::floor(item->valuedouble) == item->valuedouble;
}

bool parse_id(const cJSON *root, std::int32_t *id)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!is_integer_number(item) || item->valuedouble < 0 ||
        item->valuedouble > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    *id = static_cast<std::int32_t>(item->valuedouble);
    return true;
}

bool write_frame(ControlPlane *control, const cJSON *root)
{
    char json[kMaxJsonBytes]{};
    if (!cJSON_PrintPreallocated(const_cast<cJSON *>(root), json, sizeof(json), false)) {
        return false;
    }

    const std::size_t json_length = std::strlen(json);
    if (kPrefixLength + json_length + 1 > kMaxOutputFrameBytes) {
        return false;
    }

    char frame[kMaxOutputFrameBytes]{};
    std::memcpy(frame, kFramePrefix, kPrefixLength);
    std::memcpy(frame + kPrefixLength, json, json_length);
    frame[kPrefixLength + json_length] = '\n';
    return control->output.write != nullptr &&
           control->output.write(control->output.context,
                                 frame,
                                 kPrefixLength + json_length + 1);
}

void send_error(ControlPlane *control,
                bool has_id,
                std::int32_t id,
                const char *code,
                const char *message)
{
    cJSON *response = cJSON_CreateObject();
    cJSON *error = cJSON_CreateObject();
    if (response == nullptr || error == nullptr) {
        cJSON_Delete(response);
        cJSON_Delete(error);
        return;
    }

    cJSON_AddStringToObject(response, "type", "response");
    cJSON_AddNumberToObject(response, "v", 1);
    if (has_id) {
        cJSON_AddNumberToObject(response, "id", id);
    } else {
        cJSON_AddNullToObject(response, "id");
    }
    cJSON_AddFalseToObject(response, "ok");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(response, "error", error);
    write_frame(control, response);
    cJSON_Delete(response);
}

void send_success(ControlPlane *control, std::int32_t id, cJSON *result)
{
    cJSON *response = cJSON_CreateObject();
    if (response == nullptr) {
        cJSON_Delete(result);
        return;
    }

    cJSON_AddStringToObject(response, "type", "response");
    cJSON_AddNumberToObject(response, "v", 1);
    cJSON_AddNumberToObject(response, "id", id);
    cJSON_AddTrueToObject(response, "ok");
    cJSON_AddItemToObject(response, "result", result);
    write_frame(control, response);
    cJSON_Delete(response);
}

} // namespace

void handle_frame(ControlPlane *control, framing::Frame frame)
{
    if (control == nullptr || !frame.protocol_candidate) {
        return;
    }

    if (frame.too_long) {
        send_error(control, false, 0, "LINE_TOO_LONG", "request line exceeds 512 bytes");
        return;
    }

    if (frame.bytes.size() < kPrefixLength ||
        frame.bytes.compare(0, kPrefixLength, kFramePrefix, kPrefixLength) != 0) {
        return;
    }

    std::string_view json = frame.bytes.substr(kPrefixLength);
    if (!json.empty() && json.back() == '\r') {
        json.remove_suffix(1);
    }

    cJSON *root = cJSON_ParseWithLengthOpts(json.data(), json.size(), nullptr, false);
    if (root == nullptr) {
        send_error(control, false, 0, "MALFORMED_JSON", "request is not valid JSON");
        return;
    }

    if (!cJSON_IsObject(root)) {
        send_error(control, false, 0, "INVALID_REQUEST", "request must be a JSON object");
        cJSON_Delete(root);
        return;
    }

    std::int32_t id = 0;
    const bool has_id = parse_id(root, &id);
    if (!has_id) {
        send_error(control, false, 0, "INVALID_REQUEST", "id must be a non-negative integer");
        cJSON_Delete(root);
        return;
    }

    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "v");
    if (!is_integer_number(version)) {
        send_error(control, true, id, "INVALID_REQUEST", "v must be an integer");
        cJSON_Delete(root);
        return;
    }
    if (version->valuedouble != 1) {
        send_error(control, true, id, "UNSUPPORTED_PROTOCOL_VERSION", "only protocol version 1 is supported");
        cJSON_Delete(root);
        return;
    }

    const cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(command) || command->valuestring == nullptr || command->valuestring[0] == '\0') {
        send_error(control, true, id, "INVALID_REQUEST", "cmd must be a non-empty string");
        cJSON_Delete(root);
        return;
    }

    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        send_error(control, true, id, "INVALID_PARAMS", "params must be an object");
        cJSON_Delete(root);
        return;
    }

    const dispatcher::Request request{
        id,
        std::string_view(command->valuestring),
        params,
        &control->metadata,
    };
    const dispatcher::Result result = dispatcher::dispatch(request);
    if (result.ok) {
        send_success(control, id, result.value);
    } else {
        send_error(control, true, id, result.error_code, result.error_message);
    }
    cJSON_Delete(root);
}

} // namespace control_plane::protocol
