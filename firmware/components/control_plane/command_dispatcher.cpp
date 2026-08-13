#include "command_dispatcher.hpp"

#include <cstring>

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

    const char *names[] = {"protocol.hello", "system.ping", "system.info"};
    for (const char *name : names) {
        if (cJSON_AddItemToArray(capabilities, cJSON_CreateString(name)) == false) {
            cJSON_Delete(result);
            return nullptr;
        }
    }
    return result;
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
