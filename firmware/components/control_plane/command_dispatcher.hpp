#pragma once

#include <cstdint>
#include <string_view>

#include "cJSON.h"

#include "control_plane/control_plane.hpp"

namespace control_plane::dispatcher {

struct Request {
    std::int32_t id;
    std::string_view command;
    const cJSON *params;
    const Metadata *metadata;
    void *context;
};

struct Result {
    bool ok;
    cJSON *value;
    const char *error_code;
    const char *error_message;
};

Result dispatch(const Request &request);

} // namespace control_plane::dispatcher
