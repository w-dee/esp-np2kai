#include "control_stream/control_stream.hpp"

#include "binary_data_plane/cobs.hpp"

namespace control_stream {
namespace {

void reset_binary(ControlStream *stream)
{
    stream->encoded_length = 0;
    stream->state = State::Text;
}

void process_text_byte(ControlStream *stream, std::uint8_t byte)
{
    if (byte == 0) {
        control_plane::reset_input(stream->control);
        stream->state = State::StartZero;
        return;
    }
    control_plane::feed(stream->control, &byte, 1);
}

void finish_binary_candidate(ControlStream *stream, std::uint32_t now_ms)
{
    if (stream->encoded_length != 0) {
        std::size_t decoded_length = 0;
        if (binary_data_plane::cobs::decode(stream->encoded,
                                            stream->encoded_length,
                                            stream->decoded,
                                            sizeof(stream->decoded),
                                            &decoded_length)) {
            binary_data_plane::handle_decoded_frame(stream->binary,
                                                    stream->decoded,
                                                    decoded_length,
                                                    now_ms);
        }
    }
    reset_binary(stream);
}

void feed_byte(ControlStream *stream, std::uint8_t byte, std::uint32_t now_ms)
{
    switch (stream->state) {
    case State::Text:
        process_text_byte(stream, byte);
        break;
    case State::StartZero:
        if (byte == 0) {
            stream->encoded_length = 0;
            stream->state = State::BinaryCollect;
        } else {
            stream->state = State::Text;
            process_text_byte(stream, byte);
        }
        break;
    case State::BinaryCollect:
        if (byte == 0) {
            finish_binary_candidate(stream, now_ms);
        } else if (stream->encoded_length < sizeof(stream->encoded)) {
            stream->encoded[stream->encoded_length++] = byte;
        } else {
            stream->state = State::BinaryDiscard;
        }
        break;
    case State::BinaryDiscard:
        if (byte == 0) {
            reset_binary(stream);
        }
        break;
    }
}

} // namespace

void init(ControlStream *stream,
          control_plane::ControlPlane *control,
          binary_data_plane::TransferManager *binary)
{
    if (stream == nullptr) {
        return;
    }
    *stream = ControlStream{};
    stream->control = control;
    stream->binary = binary;
}

void feed(ControlStream *stream,
          const std::uint8_t *data,
          std::size_t length,
          std::uint32_t now_ms)
{
    if (stream == nullptr || data == nullptr) {
        return;
    }
    for (std::size_t index = 0; index < length; ++index) {
        feed_byte(stream, data[index], now_ms);
    }
}

} // namespace control_stream
