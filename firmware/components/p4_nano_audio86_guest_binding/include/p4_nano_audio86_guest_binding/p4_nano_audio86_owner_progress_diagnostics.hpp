#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace p4_nano_audio86_guest_binding {

constexpr std::size_t kOwnerCheckpointHistoryDepth = 4U;
constexpr std::size_t kOwnerCheckpointIntervalCount =
    kOwnerCheckpointHistoryDepth - 1U;
constexpr std::uint32_t kOwnerDiagnosticUnavailable = UINT32_MAX;

enum class OwnerSubphase : std::uint32_t {
    Unavailable = 0U,
    CpuExec,
    NeventProgress,
    CooperativeDelay,
    ProgressCheckpointEnter,
    ProgressCheckpointWait,
    ProgressCheckpointExit,
    TransactionWait,
    StatusCheck,
    TerminalTransition,
};

enum class OwnerProgressPattern : std::uint32_t {
    InsufficientHistory = 0U,
    SteadySlow,
    Decelerating,
    Stopped,
};

struct OwnerCheckpointRecord {
    std::uint32_t valid;
    std::uint32_t checkpoint_sequence;
    std::uint64_t wall_time_us;
    std::uint64_t guest_cycle;
    std::uint64_t guest_frame;
    std::uint16_t cs;
    std::uint16_t ip;
    std::uint16_t flags;
    std::uint16_t cx;
    std::uint16_t bp;
    std::uint8_t interrupt_enabled;
    std::uint8_t next_opcode;
    std::uint8_t hlt;
    std::uint8_t timer_a_running;
    std::uint8_t timer_b_running;
    std::uint8_t opna_status;
    std::uint16_t pic_pending;
    std::uint16_t pic_mask;
    std::uint32_t next_nevent_id;
    std::int32_t next_nevent_remaining;
    std::uint64_t last_guest_io_ordinal;
    std::uint16_t last_guest_io_port;
    std::uint64_t last_guest_io_frame;
    std::uint64_t last_guest_io_cycle;
    std::uint64_t published_horizon;
    std::uint64_t rendered_frame;
};

struct OwnerCheckpointInterval {
    std::uint32_t valid;
    std::uint32_t from_sequence;
    std::uint32_t to_sequence;
    std::uint64_t delta_wall_us;
    std::uint64_t delta_guest_cycles;
    std::uint64_t delta_guest_frames;
    std::uint64_t guest_cycles_per_second;
    std::uint64_t guest_frames_per_second;
};

struct OwnerCheckpointInput {
    std::uint64_t wall_time_us;
    std::uint64_t guest_cycle;
    std::uint64_t guest_frame;
    std::uint16_t cs;
    std::uint16_t ip;
    std::uint16_t flags;
    std::uint16_t cx;
    std::uint16_t bp;
    std::uint8_t interrupt_enabled;
    std::uint8_t next_opcode;
    std::uint8_t hlt;
    std::uint8_t timer_a_running;
    std::uint8_t timer_b_running;
    std::uint8_t opna_status;
    std::uint16_t pic_pending;
    std::uint16_t pic_mask;
    std::uint32_t next_nevent_id;
    std::int32_t next_nevent_remaining;
    std::uint64_t last_guest_io_ordinal;
    std::uint16_t last_guest_io_port;
    std::uint64_t last_guest_io_frame;
    std::uint64_t last_guest_io_cycle;
    std::uint64_t published_horizon;
    std::uint64_t rendered_frame;
};

struct OwnerProgressSnapshot {
    std::uint32_t coherent;
    OwnerSubphase subphase;
    OwnerProgressPattern pattern;
    std::uint32_t checkpoint_count;
    std::uint32_t checkpoint_call_count;
    std::uint32_t checkpoint_success_count;
    std::uint64_t checkpoint_last_enter_us;
    std::uint64_t checkpoint_last_exit_us;
    std::uint64_t checkpoint_max_duration_us;
    std::uint32_t checkpoint_last_result;
    std::uint64_t checkpoint_last_frame;
    OwnerCheckpointRecord history[kOwnerCheckpointHistoryDepth];
    OwnerCheckpointInterval intervals[kOwnerCheckpointIntervalCount];
};

class OwnerProgressDiagnostics final {
public:
    void reset() noexcept;
    void publish_subphase(OwnerSubphase subphase) noexcept;
    void publish_checkpoint(const OwnerCheckpointInput &input) noexcept;
    void checkpoint_enter(std::uint64_t now_us,
                          std::uint64_t guest_frame) noexcept;
    void checkpoint_exit(std::uint64_t now_us, std::uint32_t result,
                         bool success) noexcept;
    OwnerProgressSnapshot snapshot() const noexcept;

private:
    struct AtomicRecord {
        std::atomic<std::uint32_t> guard{0U};
        std::atomic<std::uint32_t> checkpoint_sequence{0U};
        std::atomic<std::uint32_t> wall_low{0U};
        std::atomic<std::uint32_t> wall_high{0U};
        std::atomic<std::uint32_t> cycle_low{0U};
        std::atomic<std::uint32_t> cycle_high{0U};
        std::atomic<std::uint32_t> frame_low{0U};
        std::atomic<std::uint32_t> frame_high{0U};
        std::atomic<std::uint32_t> registers{0U};
        std::atomic<std::uint32_t> flags_cx{0U};
        std::atomic<std::uint32_t> bp_cpu{0U};
        std::atomic<std::uint32_t> timer_status{0U};
        std::atomic<std::uint32_t> pic{0U};
        std::atomic<std::uint32_t> next_nevent_id{0U};
        std::atomic<std::uint32_t> next_nevent_remaining{0U};
        std::atomic<std::uint32_t> io_ordinal_low{0U};
        std::atomic<std::uint32_t> io_ordinal_high{0U};
        std::atomic<std::uint32_t> io_port{0U};
        std::atomic<std::uint32_t> io_frame_low{0U};
        std::atomic<std::uint32_t> io_frame_high{0U};
        std::atomic<std::uint32_t> io_cycle_low{0U};
        std::atomic<std::uint32_t> io_cycle_high{0U};
        std::atomic<std::uint32_t> horizon_low{0U};
        std::atomic<std::uint32_t> horizon_high{0U};
        std::atomic<std::uint32_t> rendered_low{0U};
        std::atomic<std::uint32_t> rendered_high{0U};
    };

    static bool read_record(const AtomicRecord &source,
                            std::uint32_t expected_sequence,
                            OwnerCheckpointRecord *record) noexcept;

    AtomicRecord history_[kOwnerCheckpointHistoryDepth]{};
    std::atomic<std::uint32_t> published_count_{0U};
    std::atomic<std::uint32_t> subphase_{
        static_cast<std::uint32_t>(OwnerSubphase::Unavailable)};
    std::atomic<std::uint32_t> checkpoint_stats_guard_{0U};
    std::atomic<std::uint32_t> checkpoint_call_count_{0U};
    std::atomic<std::uint32_t> checkpoint_success_count_{0U};
    std::atomic<std::uint32_t> checkpoint_last_enter_low_{0U};
    std::atomic<std::uint32_t> checkpoint_last_enter_high_{0U};
    std::atomic<std::uint32_t> checkpoint_last_exit_low_{0U};
    std::atomic<std::uint32_t> checkpoint_last_exit_high_{0U};
    std::atomic<std::uint32_t> checkpoint_max_duration_low_{0U};
    std::atomic<std::uint32_t> checkpoint_max_duration_high_{0U};
    std::atomic<std::uint32_t> checkpoint_last_result_{
        kOwnerDiagnosticUnavailable};
    std::atomic<std::uint32_t> checkpoint_last_frame_low_{0U};
    std::atomic<std::uint32_t> checkpoint_last_frame_high_{0U};
};

const char *owner_progress_pattern_name(OwnerProgressPattern pattern) noexcept;
const char *owner_subphase_name(OwnerSubphase subphase) noexcept;

} // namespace p4_nano_audio86_guest_binding
