#include "binary_codec.hpp"

#include "cobs.hpp"
#include "crc32.hpp"

namespace binary_data_plane::codec {
namespace {

void put_u16(std::uint8_t *data, std::size_t offset, std::uint16_t value)
{
    data[offset] = static_cast<std::uint8_t>(value & 0xffu);
    data[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put_u32(std::uint8_t *data, std::size_t offset, std::uint32_t value)
{
    for (std::size_t index = 0; index < 4; ++index) {
        data[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

void put_u64(std::uint8_t *data, std::size_t offset, std::uint64_t value)
{
    for (std::size_t index = 0; index < 8; ++index) {
        data[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

std::uint16_t get_u16(const std::uint8_t *data, std::size_t offset)
{
    return static_cast<std::uint16_t>(data[offset]) |
           static_cast<std::uint16_t>(data[offset + 1]) << 8;
}

std::uint32_t get_u32(const std::uint8_t *data, std::size_t offset)
{
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(data[offset + index]) << (index * 8);
    }
    return value;
}

std::uint64_t get_u64(const std::uint8_t *data, std::size_t offset)
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(data[offset + index]) << (index * 8);
    }
    return value;
}

bool is_known_type(FrameType type)
{
    return type == FrameType::Data || type == FrameType::Ack || type == FrameType::Nack;
}

bool has_valid_type_fields(FrameType type, std::uint16_t status, std::uint16_t payload_length)
{
    switch (type) {
    case FrameType::Data:
        return payload_length != 0 && status == 0;
    case FrameType::Ack:
        return payload_length == 0 && status == 0;
    case FrameType::Nack:
        return payload_length == 0 && status != 0;
    }
    return false;
}

} // namespace

bool parse_decoded(const std::uint8_t *decoded,
                  std::size_t length,
                  ParsedFrame *frame)
{
    if (decoded == nullptr || frame == nullptr) {
        return false;
    }
    *frame = ParsedFrame{};
    if (length < kHeaderBytes + kCrcBytes) {
        return false;
    }

    frame->type = static_cast<FrameType>(decoded[3]);
    frame->flags = get_u16(decoded, 4);
    frame->header_length = get_u16(decoded, 6);
    frame->transfer_id = get_u32(decoded, 8);
    frame->sequence = get_u32(decoded, 12);
    frame->offset = get_u64(decoded, 16);
    frame->payload_length = get_u16(decoded, 24);
    frame->status = get_u16(decoded, 26);
    frame->wire_crc = get_u32(decoded, length - kCrcBytes);

    if (decoded[0] != kMagic0 || decoded[1] != kMagic1 ||
        decoded[2] != kDataPlaneVersion || frame->flags != 0 ||
        frame->header_length != kHeaderBytes ||
        !is_known_type(frame->type) ||
        frame->payload_length > kMaxPayloadBytes ||
        !has_valid_type_fields(frame->type, frame->status, frame->payload_length) ||
        frame->transfer_id == 0 ||
        length != kHeaderBytes + frame->payload_length + kCrcBytes) {
        return false;
    }

    frame->payload = decoded + kHeaderBytes;
    frame->structural_valid = true;
    frame->crc_valid = crc32::calculate(decoded, kHeaderBytes + frame->payload_length) ==
                       frame->wire_crc;
    return true;
}

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
                  std::size_t *wire_length)
{
    if (decoded == nullptr || encoded == nullptr || wire == nullptr || wire_length == nullptr ||
        (payload == nullptr && payload_length != 0) || flags != 0 ||
        !is_known_type(type) ||
        transfer_id == 0 || payload_length > kMaxPayloadBytes ||
        !has_valid_type_fields(type, status, static_cast<std::uint16_t>(payload_length)) ||
        decoded_capacity < kHeaderBytes + payload_length + kCrcBytes ||
        wire_capacity < 3) {
        return false;
    }

    decoded[0] = kMagic0;
    decoded[1] = kMagic1;
    decoded[2] = kDataPlaneVersion;
    decoded[3] = static_cast<std::uint8_t>(type);
    put_u16(decoded, 4, flags);
    put_u16(decoded, 6, kHeaderBytes);
    put_u32(decoded, 8, transfer_id);
    put_u32(decoded, 12, sequence);
    put_u64(decoded, 16, offset);
    put_u16(decoded, 24, static_cast<std::uint16_t>(payload_length));
    put_u16(decoded, 26, status);
    for (std::size_t index = 0; index < payload_length; ++index) {
        decoded[kHeaderBytes + index] = payload[index];
    }

    const std::size_t crc_offset = kHeaderBytes + payload_length;
    put_u32(decoded, crc_offset, crc32::calculate(decoded, crc_offset));

    std::size_t encoded_length = 0;
    if (!cobs::encode(decoded,
                      crc_offset + kCrcBytes,
                      encoded,
                      encoded_capacity,
                      &encoded_length) ||
        wire_capacity < encoded_length + 3) {
        return false;
    }

    wire[0] = 0;
    wire[1] = 0;
    for (std::size_t index = 0; index < encoded_length; ++index) {
        wire[index + 2] = encoded[index];
    }
    wire[encoded_length + 2] = 0;
    *wire_length = encoded_length + 3;
    return true;
}

} // namespace binary_data_plane::codec
