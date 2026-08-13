#pragma once

#include <cstddef>
#include <cstdint>

#include "binary_data_plane/binary_data_plane.hpp"

namespace binary_data_plane::codec {

struct ParsedFrame {
    bool structural_valid = false;
    bool crc_valid = false;
    FrameType type = FrameType::Data;
    std::uint16_t flags = 0;
    std::uint16_t header_length = 0;
    std::uint32_t transfer_id = 0;
    std::uint32_t sequence = 0;
    std::uint64_t offset = 0;
    std::uint16_t payload_length = 0;
    std::uint16_t status = 0;
    std::uint32_t wire_crc = 0;
    const std::uint8_t *payload = nullptr;
};

bool parse_decoded(const std::uint8_t *decoded,
                  std::size_t length,
                  ParsedFrame *frame);

bool encode_frame(FrameType type,
                  std::uint16_t flags,
                  std::uint32_t transfer_id,
                  std::uint32_t sequence,
                  std::uint64_t offset,
                  std::uint16_t status,
                  const std::uint8_t *payload,
                  std::size_t payload_length,
                  std::uint8_t *decoded,
                  std::size_t decoded_capacity,
                  std::uint8_t *encoded,
                  std::size_t encoded_capacity,
                  std::uint8_t *wire,
                  std::size_t wire_capacity,
                  std::size_t *wire_length);

} // namespace binary_data_plane::codec
