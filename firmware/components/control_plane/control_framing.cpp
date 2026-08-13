#include "control_framing.hpp"

#include <cstring>

namespace control_plane::framing {
namespace {

void reset(ControlPlane *control)
{
    control->frame_length = 0;
    control->discarding = false;
    control->prefix_candidate = true;
}

void emit_line(ControlPlane *control, FrameCallback callback)
{
    if (control->discarding) {
        callback(control, Frame{{}, control->prefix_candidate, true});
    } else {
        control->frame[control->frame_length] = '\0';
        callback(control,
                 Frame{std::string_view(control->frame, control->frame_length),
                       control->prefix_candidate,
                       false});
    }
    reset(control);
}

void consume_byte(ControlPlane *control, std::uint8_t byte, FrameCallback callback)
{
    if (byte == '\n') {
        emit_line(control, callback);
        return;
    }

    if (control->discarding) {
        return;
    }

    if (control->frame_length < kMaxFrameBytes) {
        control->frame[control->frame_length] = static_cast<char>(byte);
        ++control->frame_length;

        const std::size_t prefix_length = sizeof(kFramePrefix) - 1;
        const std::size_t index = control->frame_length - 1;
        if (index < prefix_length && control->frame[index] != kFramePrefix[index]) {
            control->prefix_candidate = false;
        }
        return;
    }

    // The 513th non-LF byte is never stored. Everything through the next LF is
    // discarded, and the callback reports whether the line began as a frame.
    control->discarding = true;
}

} // namespace

void consume(ControlPlane *control,
             const std::uint8_t *data,
             std::size_t length,
             FrameCallback callback)
{
    if (control == nullptr || data == nullptr || callback == nullptr) {
        return;
    }

    for (std::size_t index = 0; index < length; ++index) {
        consume_byte(control, data[index], callback);
    }
}

} // namespace control_plane::framing
