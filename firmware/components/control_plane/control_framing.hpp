#pragma once

#include <string_view>

#include "control_plane/control_plane.hpp"

namespace control_plane::framing {

struct Frame {
    std::string_view bytes;
    bool protocol_candidate;
    bool too_long;
};

using FrameCallback = void (*)(ControlPlane *control, Frame frame);

void consume(ControlPlane *control,
             const std::uint8_t *data,
             std::size_t length,
             FrameCallback callback);

} // namespace control_plane::framing
