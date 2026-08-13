#include "control_plane/control_plane.hpp"

#include "control_framing.hpp"
#include "control_protocol.hpp"

namespace control_plane {

void init(ControlPlane *control,
          OutputSink output,
          Metadata metadata,
          void *context)
{
    if (control == nullptr) {
        return;
    }
    control->output = output;
    control->metadata = metadata;
    control->context = context;
    reset_input(control);
}

void reset_input(ControlPlane *control)
{
    if (control == nullptr) {
        return;
    }
    control->frame_length = 0;
    control->discarding = false;
    control->prefix_candidate = true;
    control->frame[0] = '\0';
}

void feed(ControlPlane *control, const std::uint8_t *data, std::size_t length)
{
    framing::consume(control, data, length, protocol::handle_frame);
}

} // namespace control_plane
