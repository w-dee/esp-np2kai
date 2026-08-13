#include "binary_data_plane/binary_data_plane.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "binary_codec.hpp"
#include "crc32.hpp"

namespace binary_data_plane {
namespace {

constexpr std::string_view kPatternPrefix = "@ESP-NP2 ";
constexpr std::string_view kPatternLog = "I (123) ESP-IDF log text\n";

std::uint32_t get_u32(const std::uint8_t *data, std::size_t offset)
{
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(data[offset + index]) << (index * 8);
    }
    return value;
}

bool elapsed(std::uint32_t now, std::uint32_t then, std::uint32_t duration)
{
    return static_cast<std::uint32_t>(now - then) >= duration;
}

TransferInfo current_info(const TransferManager *manager)
{
    TransferInfo info{};
    info.transfer_id = manager->transfer_id;
    info.direction = manager->direction;
    info.state = manager->state;
    info.total_bytes = manager->total_bytes;
    info.transferred_bytes = manager->transferred_bytes;
    info.expected_sequence = manager->expected_sequence;
    info.expected_offset = manager->expected_offset;
    if (!manager->active && manager->terminal_has_crc) {
        info.crc32 = manager->terminal_crc;
        info.has_crc32 = true;
    }
    return info;
}

void finish_active(TransferManager *manager, TransferState state)
{
    manager->state = state;
    manager->active = false;
    manager->tx_pending_start = false;
    manager->tx_frame_ready = false;
    manager->tx_waiting_ack = false;
    manager->terminal = current_info(manager);
    manager->terminal.state = state;
    manager->terminal.crc32 = manager->state == TransferState::Completed ?
        crc32::finish(manager->direction == Direction::DeviceToHost ? manager->tx_crc : manager->running_crc) : 0;
    manager->terminal.has_crc32 = manager->state == TransferState::Completed;
    manager->terminal_crc = manager->terminal.crc32;
    manager->terminal_has_crc = manager->terminal.has_crc32;
    manager->terminal_valid = true;
}

bool write_wire(TransferManager *manager,
                FrameType type,
                std::uint32_t transfer_id,
                std::uint32_t sequence,
                std::uint64_t offset,
                std::uint16_t status,
                const std::uint8_t *payload,
                std::size_t payload_length)
{
    std::size_t wire_length = 0;
    if (!codec::encode_frame(type,
                             0,
                             transfer_id,
                             sequence,
                             offset,
                             status,
                             payload,
                             payload_length,
                             manager->decoded_frame,
                             sizeof(manager->decoded_frame),
                             manager->encoded_body,
                             sizeof(manager->encoded_body),
                             manager->wire_frame,
                             sizeof(manager->wire_frame),
                             &wire_length)) {
        return false;
    }
    return manager->output.write != nullptr &&
           manager->output.write(manager->output.context, manager->wire_frame, wire_length);
}

void send_ack(TransferManager *manager)
{
    if (!write_wire(manager,
                    FrameType::Ack,
                    manager->transfer_id,
                    manager->expected_sequence,
                    manager->expected_offset,
                    0,
                    nullptr,
                    0)) {
        finish_active(manager, TransferState::Aborted);
    }
}

void send_nack(TransferManager *manager, NackReason reason)
{
    if (!write_wire(manager,
                    FrameType::Nack,
                    manager->transfer_id,
                    manager->expected_sequence,
                    manager->expected_offset,
                    static_cast<std::uint16_t>(reason),
                    nullptr,
                    0)) {
        finish_active(manager, TransferState::Aborted);
    }
}

bool is_valid_nack_reason(std::uint16_t reason)
{
    switch (static_cast<NackReason>(reason)) {
    case NackReason::BadCrc:
    case NackReason::BadSequence:
    case NackReason::BadOffset:
    case NackReason::InvalidLength:
    case NackReason::UnknownTransfer:
    case NackReason::TransferClosed:
    case NackReason::BadHeader:
    case NackReason::WrongDirection:
        return true;
    }
    return false;
}

std::uint16_t pattern_value(std::uint64_t offset)
{
    const std::size_t block_offset = static_cast<std::size_t>(offset % 1024);
    if (block_offset >= 256 && block_offset < 256 + kPatternPrefix.size()) {
        return static_cast<std::uint8_t>(kPatternPrefix[block_offset - 256]);
    }
    if (block_offset >= 320 && block_offset < 320 + kPatternLog.size()) {
        return static_cast<std::uint8_t>(kPatternLog[block_offset - 320]);
    }
    if (block_offset == 384) {
        return 0x00;
    }
    if (block_offset == 385) {
        return 0x0a;
    }
    if (block_offset == 386) {
        return 0x0d;
    }
    if (block_offset == 387) {
        return 0xff;
    }
    if (block_offset == 388) {
        return 0x00;
    }
    return static_cast<std::uint8_t>(block_offset & 0xff);
}

bool validate_pattern(std::uint64_t offset,
                      const std::uint8_t *payload,
                      std::size_t length)
{
    if (payload == nullptr) {
        return false;
    }
    for (std::size_t index = 0; index < length; ++index) {
        if (payload[index] != pattern_value(offset + index)) {
            return false;
        }
    }
    return true;
}

void prepare_tx_payload(TransferManager *manager)
{
    manager->tx_length = static_cast<std::uint16_t>(
        manager->total_bytes - manager->tx_offset > kMaxPayloadBytes ?
            kMaxPayloadBytes : manager->total_bytes - manager->tx_offset);
    for (std::size_t index = 0; index < manager->tx_length; ++index) {
        manager->tx_payload[index] = pattern_value(manager->tx_offset + index);
    }
    manager->tx_retries = 0;
    manager->tx_crc = crc32::update(manager->tx_crc, manager->tx_payload, manager->tx_length);
    manager->tx_frame_ready = true;
}

void send_current_tx(TransferManager *manager, std::uint32_t now_ms)
{
    if (!manager->tx_frame_ready ||
        !write_wire(manager,
                    FrameType::Data,
                    manager->transfer_id,
                    manager->tx_sequence,
                    manager->tx_offset,
                    0,
                    manager->tx_payload,
                    manager->tx_length)) {
        finish_active(manager, TransferState::Aborted);
        return;
    }
    manager->tx_waiting_ack = true;
    manager->tx_last_sent_ms = now_ms;
}

void retry_current_tx(TransferManager *manager, std::uint32_t now_ms)
{
    if (manager->tx_retries >= kMaxRetransmissions) {
        finish_active(manager, TransferState::Aborted);
        return;
    }
    ++manager->tx_retries;
    send_current_tx(manager, now_ms);
}

void handle_rx_data(TransferManager *manager, const codec::ParsedFrame &frame, std::uint32_t now_ms)
{
    if (frame.transfer_id != manager->transfer_id) {
        return;
    }
    if (!frame.crc_valid) {
        send_nack(manager, NackReason::BadCrc);
        return;
    }
    if (frame.payload_length == 0 ||
        manager->transferred_bytes + frame.payload_length > manager->total_bytes) {
        send_nack(manager, NackReason::InvalidLength);
        return;
    }

    if (manager->previous_data_valid &&
        frame.sequence == manager->previous_data_sequence &&
        frame.offset == manager->previous_data_offset &&
        frame.payload_length == manager->previous_data_length &&
        frame.wire_crc == manager->previous_data_crc) {
        send_ack(manager);
        manager->last_activity_ms = now_ms;
        return;
    }

    if (frame.sequence != manager->expected_sequence) {
        send_nack(manager, NackReason::BadSequence);
        return;
    }
    if (frame.offset != manager->expected_offset) {
        send_nack(manager, NackReason::BadOffset);
        return;
    }
    if (!validate_pattern(frame.offset, frame.payload, frame.payload_length)) {
        send_nack(manager, NackReason::BadHeader);
        return;
    }

    manager->running_crc = crc32::update(manager->running_crc,
                                          frame.payload,
                                          frame.payload_length);
    manager->previous_data_valid = true;
    manager->previous_data_sequence = frame.sequence;
    manager->previous_data_offset = frame.offset;
    manager->previous_data_length = frame.payload_length;
    manager->previous_data_crc = frame.wire_crc;
    manager->transferred_bytes += frame.payload_length;
    ++manager->expected_sequence;
    manager->expected_offset += frame.payload_length;
    manager->last_activity_ms = now_ms;
    send_ack(manager);
    if (manager->active && manager->transferred_bytes == manager->total_bytes) {
        finish_active(manager, TransferState::Completed);
    }
}

void handle_tx_ack(TransferManager *manager,
                   const codec::ParsedFrame &frame,
                   std::uint32_t now_ms)
{
    if (frame.transfer_id != manager->transfer_id || !frame.crc_valid ||
        frame.payload_length != 0 || !manager->tx_waiting_ack) {
        return;
    }

    if (frame.type == FrameType::Nack) {
        if (!is_valid_nack_reason(frame.status)) {
            finish_active(manager, TransferState::Aborted);
            return;
        }
        if (frame.sequence != manager->tx_sequence ||
            frame.offset != manager->tx_offset) {
            finish_active(manager, TransferState::Aborted);
            return;
        }
        retry_current_tx(manager, now_ms);
        return;
    }

    if (frame.type != FrameType::Ack || frame.status != 0) {
        return;
    }

    const std::uint32_t expected_sequence = manager->tx_sequence + 1;
    const std::uint64_t expected_offset = manager->tx_offset + manager->tx_length;
    if (frame.sequence != expected_sequence) {
        send_nack(manager, NackReason::BadSequence);
        return;
    }
    if (frame.offset != expected_offset) {
        send_nack(manager, NackReason::BadOffset);
        return;
    }

    manager->transferred_bytes = expected_offset;
    manager->expected_sequence = expected_sequence;
    manager->expected_offset = expected_offset;
    manager->tx_waiting_ack = false;
    manager->tx_frame_ready = false;
    manager->last_activity_ms = now_ms;
    if (manager->transferred_bytes == manager->total_bytes) {
        finish_active(manager, TransferState::Completed);
        return;
    }
    manager->tx_sequence = expected_sequence;
    manager->tx_offset = expected_offset;
}

} // namespace

void init(TransferManager *manager, OutputSink output)
{
    if (manager == nullptr) {
        return;
    }
    const std::uint32_t next_id = manager->next_transfer_id == 0 ? 1 : manager->next_transfer_id;
    *manager = TransferManager{};
    manager->output = output;
    manager->next_transfer_id = next_id;
}

ManagerError begin_rx(TransferManager *manager,
                      std::uint64_t size_bytes,
                      TransferInfo *info)
{
    if (manager == nullptr || info == nullptr) {
        return ManagerError::InvalidParams;
    }
    if (manager->active) {
        return ManagerError::Busy;
    }
    if (size_bytes != kTestTransferBytes) {
        return ManagerError::InvalidSize;
    }

    manager->active = true;
    manager->direction = Direction::HostToDevice;
    manager->state = TransferState::Active;
    manager->transfer_id = manager->next_transfer_id++;
    if (manager->next_transfer_id == 0) {
        manager->next_transfer_id = 1;
    }
    manager->total_bytes = size_bytes;
    manager->transferred_bytes = 0;
    manager->expected_sequence = 0;
    manager->expected_offset = 0;
    manager->running_crc = crc32::init();
    manager->previous_data_valid = false;
    manager->last_activity_ms = 0;
    manager->tx_retries = 0;
    manager->tx_pending_start = false;
    manager->tx_frame_ready = false;
    manager->tx_waiting_ack = false;
    manager->terminal_valid = false;
    manager->terminal_has_crc = false;
    *info = current_info(manager);
    return ManagerError::Ok;
}

ManagerError begin_tx(TransferManager *manager,
                      std::uint64_t size_bytes,
                      TransferInfo *info)
{
    if (manager == nullptr || info == nullptr) {
        return ManagerError::InvalidParams;
    }
    if (manager->active) {
        return ManagerError::Busy;
    }
    if (size_bytes != kTestTransferBytes) {
        return ManagerError::InvalidSize;
    }

    manager->active = true;
    manager->direction = Direction::DeviceToHost;
    manager->state = TransferState::Active;
    manager->transfer_id = manager->next_transfer_id++;
    if (manager->next_transfer_id == 0) {
        manager->next_transfer_id = 1;
    }
    manager->total_bytes = size_bytes;
    manager->transferred_bytes = 0;
    manager->expected_sequence = 0;
    manager->expected_offset = 0;
    manager->tx_sequence = 0;
    manager->tx_offset = 0;
    manager->tx_crc = crc32::init();
    manager->last_activity_ms = 0;
    manager->tx_retries = 0;
    manager->tx_pending_start = true;
    manager->tx_frame_ready = false;
    manager->tx_waiting_ack = false;
    manager->terminal_valid = false;
    manager->terminal_has_crc = false;
    *info = current_info(manager);
    return ManagerError::Ok;
}

ManagerError get_status(const TransferManager *manager,
                        std::uint32_t transfer_id,
                        TransferInfo *info)
{
    if (manager == nullptr || info == nullptr) {
        return ManagerError::InvalidParams;
    }
    if (manager->active && transfer_id == manager->transfer_id) {
        *info = current_info(manager);
        return ManagerError::Ok;
    }
    if (manager->terminal_valid && transfer_id == manager->terminal.transfer_id) {
        *info = manager->terminal;
        return ManagerError::Ok;
    }
    return ManagerError::UnknownTransfer;
}

ManagerError abort(TransferManager *manager,
                   std::uint32_t transfer_id,
                   TransferInfo *info)
{
    if (manager == nullptr || info == nullptr) {
        return ManagerError::InvalidParams;
    }
    if (manager->active && transfer_id == manager->transfer_id) {
        finish_active(manager, TransferState::Aborted);
        *info = manager->terminal;
        return ManagerError::Ok;
    }
    if (manager->terminal_valid && transfer_id == manager->terminal.transfer_id) {
        *info = manager->terminal;
        return ManagerError::Ok;
    }
    return ManagerError::UnknownTransfer;
}

void handle_decoded_frame(TransferManager *manager,
                          const std::uint8_t *decoded,
                          std::size_t length,
                          std::uint32_t now_ms)
{
    if (manager == nullptr || decoded == nullptr) {
        return;
    }

    codec::ParsedFrame frame{};
    if (!codec::parse_decoded(decoded, length, &frame)) {
        if (manager->active && length >= 12 && get_u32(decoded, 8) == manager->transfer_id) {
            send_nack(manager, NackReason::BadHeader);
        }
        return;
    }
    if (!manager->active) {
        return;
    }
    manager->last_activity_ms = now_ms;

    if (!frame.crc_valid) {
        if (frame.transfer_id == manager->transfer_id) {
            send_nack(manager, NackReason::BadCrc);
        }
        return;
    }

    if (manager->direction == Direction::HostToDevice) {
        if (frame.type != FrameType::Data) {
            send_nack(manager, NackReason::WrongDirection);
            return;
        }
        handle_rx_data(manager, frame, now_ms);
    } else {
        if (frame.type != FrameType::Ack && frame.type != FrameType::Nack) {
            send_nack(manager, NackReason::WrongDirection);
            return;
        }
        handle_tx_ack(manager, frame, now_ms);
    }
}

void poll(TransferManager *manager, std::uint32_t now_ms)
{
    if (manager == nullptr || !manager->active) {
        return;
    }

    if (manager->last_activity_ms == 0) {
        manager->last_activity_ms = now_ms;
    }
    if (elapsed(now_ms, manager->last_activity_ms, kReceiverTimeoutMs)) {
        finish_active(manager, TransferState::Aborted);
        return;
    }

    if (manager->direction != Direction::DeviceToHost) {
        return;
    }
    if (manager->tx_pending_start) {
        manager->tx_pending_start = false;
        prepare_tx_payload(manager);
        send_current_tx(manager, now_ms);
        return;
    }
    if (!manager->tx_waiting_ack) {
        if (!manager->tx_frame_ready) {
            prepare_tx_payload(manager);
        }
        send_current_tx(manager, now_ms);
        return;
    }
    if (elapsed(now_ms, manager->tx_last_sent_ms, kAckTimeoutMs)) {
        retry_current_tx(manager, now_ms);
    }
}

std::uint8_t test_pattern_byte(std::uint64_t offset)
{
    return static_cast<std::uint8_t>(pattern_value(offset));
}

const char *error_code(ManagerError error)
{
    switch (error) {
    case ManagerError::Ok: return "OK";
    case ManagerError::Busy: return "BUSY";
    case ManagerError::InvalidSize: return "INVALID_SIZE";
    case ManagerError::UnknownTransfer: return "UNKNOWN_TRANSFER";
    case ManagerError::TransferClosed: return "TRANSFER_CLOSED";
    case ManagerError::InvalidParams: return "INVALID_PARAMS";
    }
    return "INTERNAL_ERROR";
}

const char *error_message(ManagerError error)
{
    switch (error) {
    case ManagerError::Ok: return "operation completed";
    case ManagerError::Busy: return "another transfer is active";
    case ManagerError::InvalidSize: return "only the 65536-byte test transfer is supported";
    case ManagerError::UnknownTransfer: return "transfer is not known";
    case ManagerError::TransferClosed: return "transfer is closed";
    case ManagerError::InvalidParams: return "invalid binary transfer parameters";
    }
    return "binary transfer failed";
}

const char *direction_name(Direction direction)
{
    return direction == Direction::HostToDevice ? "host_to_device" : "device_to_host";
}

const char *state_name(TransferState state)
{
    switch (state) {
    case TransferState::Idle: return "idle";
    case TransferState::Active: return "active";
    case TransferState::Completed: return "completed";
    case TransferState::Aborted: return "aborted";
    }
    return "unknown";
}

} // namespace binary_data_plane
