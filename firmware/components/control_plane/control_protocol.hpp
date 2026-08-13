#pragma once

#include "control_framing.hpp"

namespace control_plane::protocol {

void handle_frame(ControlPlane *control, framing::Frame frame);

} // namespace control_plane::protocol
