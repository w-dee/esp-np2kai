#pragma once

#include <cstddef>
#include <cstdint>

namespace binary_data_plane {

inline constexpr std::uint8_t kMagic0 = 0x4e;
inline constexpr std::uint8_t kMagic1 = 0x42;
inline constexpr std::uint8_t kDataPlaneVersion = 1;
inline constexpr std::size_t kHeaderBytes = 28;
inline constexpr std::size_t kCrcBytes = 4;
inline constexpr std::size_t kMaxPayloadBytes = 1024;
inline constexpr std::size_t kMaxDecodedFrameBytes = kHeaderBytes + kMaxPayloadBytes + kCrcBytes;
inline constexpr std::size_t kMaxEncodedBodyBytes = 1061;
inline constexpr std::size_t kMaxWireFrameBytes = 1064;
inline constexpr std::uint64_t kTestTransferBytes = 65536;
inline constexpr std::uint64_t kMaxTransferBytes =
    static_cast<std::uint64_t>(0xffffffffu) * kMaxPayloadBytes;
inline constexpr std::uint32_t kAckTimeoutMs = 1000;
inline constexpr std::uint32_t kMaxRetransmissions = 3;
inline constexpr std::uint32_t kReceiverTimeoutMs = 10000;

enum class FrameType : std::uint8_t {
    Data = 0x01,
    Ack = 0x02,
    Nack = 0x03,
};

enum class Direction : std::uint8_t {
    HostToDevice = 1,
    DeviceToHost = 2,
};

enum class TransferState : std::uint8_t {
    Idle,
    Active,
    Completed,
    Aborted,
};

enum class ManagerError : std::uint8_t {
    Ok,
    Busy,
    InvalidSize,
    UnknownTransfer,
    TransferClosed,
    InvalidParams,
};

enum class EndpointResult : std::uint8_t { Ok, Failed };

enum class TerminalReason : std::uint8_t {
    Completed,
    ExplicitAbort,
    ReceiverTimeout,
    RetryExhausted,
    ProtocolError,
    OutputError,
    EndpointIoError,
    EndpointFinishError,
};

enum class NackReason : std::uint16_t {
    BadCrc = 1,
    BadSequence = 2,
    BadOffset = 3,
    InvalidLength = 4,
    UnknownTransfer = 5,
    TransferClosed = 6,
    BadHeader = 7,
    WrongDirection = 8,
};

struct OutputSink {
    void *context;
    bool (*write)(void *context, const std::uint8_t *data, std::size_t length);
};

struct TransferEndpoint {
    void *context = nullptr;
    EndpointResult (*begin)(void *, Direction, std::uint64_t) = nullptr;
    EndpointResult (*consume)(void *, std::uint64_t, const std::uint8_t *, std::size_t) = nullptr;
    EndpointResult (*produce)(void *, std::uint64_t, std::uint8_t *, std::size_t, std::size_t *) = nullptr;
    EndpointResult (*finish)(void *) = nullptr;
    void (*abort)(void *, TerminalReason) = nullptr;
    void (*terminal)(void *, TerminalReason) = nullptr;
};

struct TransferInfo {
    std::uint32_t transfer_id = 0;
    Direction direction = Direction::HostToDevice;
    TransferState state = TransferState::Idle;
    std::uint64_t total_bytes = 0;
    std::uint64_t transferred_bytes = 0;
    std::uint32_t expected_sequence = 0;
    std::uint64_t expected_offset = 0;
    std::uint32_t crc32 = 0;
    bool has_crc32 = false;
};

struct TransferManager {
    OutputSink output{};
    TransferEndpoint endpoint{};
    std::uint32_t next_transfer_id = 1;
    bool active = false;
    Direction direction = Direction::HostToDevice;
    TransferState state = TransferState::Idle;
    std::uint32_t transfer_id = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t transferred_bytes = 0;
    std::uint32_t expected_sequence = 0;
    std::uint64_t expected_offset = 0;
    std::uint32_t running_crc = 0xffffffffu;
    std::uint32_t terminal_crc = 0;
    bool terminal_has_crc = false;

    bool tx_pending_start = false;
    bool tx_frame_ready = false;
    bool tx_waiting_ack = false;
    std::uint32_t tx_sequence = 0;
    std::uint64_t tx_offset = 0;
    std::uint16_t tx_length = 0;
    std::uint32_t tx_retries = 0;
    std::uint32_t tx_last_sent_ms = 0;
    std::uint32_t tx_crc = 0xffffffffu;
    std::uint8_t tx_payload[kMaxPayloadBytes]{};

    bool previous_data_valid = false;
    std::uint32_t previous_data_sequence = 0;
    std::uint64_t previous_data_offset = 0;
    std::uint16_t previous_data_length = 0;
    std::uint32_t previous_data_crc = 0;

    std::uint32_t last_activity_ms = 0;
    bool terminal_valid = false;
    TransferInfo terminal{};
    bool endpoint_started = false;
    bool endpoint_finalized = false;
    bool terminal_notified = false;

    struct FinalAckReplay {
        bool valid = false;
        std::uint32_t transfer_id = 0;
        std::uint32_t sequence = 0;
        std::uint64_t offset = 0;
        std::uint16_t payload_length = 0;
        std::uint32_t wire_crc = 0;
        std::uint32_t acknowledged_sequence = 0;
        std::uint64_t acknowledged_offset = 0;
    } final_ack_replay{};

    std::uint8_t decoded_frame[kMaxDecodedFrameBytes]{};
    std::uint8_t encoded_body[kMaxEncodedBodyBytes]{};
    std::uint8_t wire_frame[kMaxWireFrameBytes]{};
};

void init(TransferManager *manager, OutputSink output);

ManagerError begin_rx(TransferManager *manager,
                      std::uint64_t size_bytes,
                      TransferEndpoint endpoint,
                      TransferInfo *info);
ManagerError begin_tx(TransferManager *manager,
                      std::uint64_t size_bytes,
                      TransferEndpoint endpoint,
                      TransferInfo *info);
ManagerError begin_test_rx(TransferManager *manager,
                           std::uint64_t size_bytes,
                           TransferInfo *info);
ManagerError begin_test_tx(TransferManager *manager,
                           std::uint64_t size_bytes,
                           TransferInfo *info);
ManagerError get_status(const TransferManager *manager,
                        std::uint32_t transfer_id,
                        TransferInfo *info);
ManagerError abort(TransferManager *manager,
                   std::uint32_t transfer_id,
                   TransferInfo *info);

void handle_decoded_frame(TransferManager *manager,
                          const std::uint8_t *decoded,
                          std::size_t length,
                          std::uint32_t now_ms);
void poll(TransferManager *manager, std::uint32_t now_ms);

std::uint8_t test_pattern_byte(std::uint64_t offset);
const char *error_code(ManagerError error);
const char *error_message(ManagerError error);
const char *direction_name(Direction direction);
const char *state_name(TransferState state);

} // namespace binary_data_plane
