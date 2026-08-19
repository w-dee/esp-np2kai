#pragma once

#include <cstddef>
#include <cstdint>

#include "binary_data_plane/binary_data_plane.hpp"
#include "control_plane/control_plane.hpp"

namespace control_stream {

inline constexpr std::size_t kTransportSyncLength = 4;

// Four consecutive NUL bytes are reserved for byte-stream resynchronization.
// The detector stays active until a non-NUL byte arrives, so longer runs are
// treated as one token and do not leak any extra NULs into the framing parser.
struct TransportSyncDetector {
    std::uint8_t consecutive_zeros = 0;

    bool feed(std::uint8_t byte)
    {
        if (byte != 0) {
            consecutive_zeros = 0;
            return false;
        }
        if (consecutive_zeros < kTransportSyncLength) {
            ++consecutive_zeros;
            return consecutive_zeros == kTransportSyncLength;
        }
        return false;
    }

    bool active() const
    {
        return consecutive_zeros >= kTransportSyncLength;
    }
};

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
    TransportSyncDetector transport_sync{};
};

void init(ControlStream *stream,
          control_plane::ControlPlane *control,
          binary_data_plane::TransferManager *binary);

void feed(ControlStream *stream,
          const std::uint8_t *data,
          std::size_t length,
          std::uint32_t now_ms);

} // namespace control_stream
