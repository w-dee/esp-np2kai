#pragma once

#include <cstddef>
#include <cstdint>

#include "binary_data_plane/binary_data_plane.hpp"
#include "control_plane/control_plane.hpp"

namespace control_stream {

enum class State : std::uint8_t {
    Text,
    StartZero,
    BinaryCollect,
    BinaryDiscard,
};

struct ControlStream {
    control_plane::ControlPlane *control = nullptr;
    binary_data_plane::TransferManager *binary = nullptr;
    State state = State::Text;
    std::size_t encoded_length = 0;
    std::uint8_t encoded[binary_data_plane::kMaxEncodedBodyBytes]{};
    std::uint8_t decoded[binary_data_plane::kMaxDecodedFrameBytes]{};
};

void init(ControlStream *stream,
          control_plane::ControlPlane *control,
          binary_data_plane::TransferManager *binary);

void feed(ControlStream *stream,
          const std::uint8_t *data,
          std::size_t length,
          std::uint32_t now_ms);

} // namespace control_stream
