#include "p4_nano_audio86_guest_binding/p4_nano_audio86_owner_progress_diagnostics.hpp"

#include <algorithm>
#include <limits>

namespace p4_nano_audio86_guest_binding {
namespace {

std::uint64_t join_u64(const std::uint32_t low,
                       const std::uint32_t high) noexcept
{
    return (static_cast<std::uint64_t>(high) << 32U) | low;
}

void store_u64(std::atomic<std::uint32_t> &low,
               std::atomic<std::uint32_t> &high,
               const std::uint64_t value) noexcept
{
    low.store(static_cast<std::uint32_t>(value), std::memory_order_relaxed);
    high.store(static_cast<std::uint32_t>(value >> 32U),
               std::memory_order_relaxed);
}

std::uint64_t load_u64(const std::atomic<std::uint32_t> &low,
                       const std::atomic<std::uint32_t> &high) noexcept
{
    return join_u64(low.load(std::memory_order_relaxed),
                    high.load(std::memory_order_relaxed));
}

std::uint64_t rate_per_second(const std::uint64_t delta,
                              const std::uint64_t wall_us) noexcept
{
    if (wall_us == 0U)
        return 0U;
    const std::uint64_t whole = delta / wall_us;
    const std::uint64_t remainder = delta % wall_us;
    if (whole > std::numeric_limits<std::uint64_t>::max() / 1000000U)
        return std::numeric_limits<std::uint64_t>::max();
    return whole * 1000000U + remainder * 1000000U / wall_us;
}

bool rate_strictly_less(const OwnerCheckpointInterval &newer,
                        const OwnerCheckpointInterval &older) noexcept
{
    return newer.delta_wall_us != 0U && older.delta_wall_us != 0U &&
           rate_per_second(newer.delta_guest_cycles, newer.delta_wall_us) <
               rate_per_second(older.delta_guest_cycles, older.delta_wall_us);
}

} // namespace

void OwnerProgressDiagnostics::reset() noexcept
{
    published_count_.store(0U, std::memory_order_relaxed);
    subphase_.store(static_cast<std::uint32_t>(OwnerSubphase::Unavailable),
                    std::memory_order_relaxed);
    checkpoint_call_count_.store(0U, std::memory_order_relaxed);
    checkpoint_success_count_.store(0U, std::memory_order_relaxed);
    checkpoint_last_result_.store(kOwnerDiagnosticUnavailable,
                                  std::memory_order_relaxed);
    store_u64(checkpoint_last_enter_low_, checkpoint_last_enter_high_, 0U);
    store_u64(checkpoint_last_exit_low_, checkpoint_last_exit_high_, 0U);
    store_u64(checkpoint_max_duration_low_, checkpoint_max_duration_high_, 0U);
    store_u64(checkpoint_last_frame_low_, checkpoint_last_frame_high_,
              UINT64_MAX);
    for (auto &record : history_)
        record.guard.store(0U, std::memory_order_relaxed);
    checkpoint_stats_guard_.store(0U, std::memory_order_release);
}

void OwnerProgressDiagnostics::publish_subphase(
    const OwnerSubphase subphase) noexcept
{
    subphase_.store(static_cast<std::uint32_t>(subphase),
                    std::memory_order_release);
}

void OwnerProgressDiagnostics::publish_checkpoint(
    const OwnerCheckpointInput &input) noexcept
{
    const std::uint32_t sequence =
        published_count_.load(std::memory_order_relaxed) + 1U;
    AtomicRecord &record = history_[(sequence - 1U) %
                                    kOwnerCheckpointHistoryDepth];
    std::uint32_t guard = record.guard.load(std::memory_order_relaxed);
    if ((guard & 1U) != 0U)
        ++guard;
    (void)record.guard.exchange(guard + 1U, std::memory_order_acq_rel);
    record.checkpoint_sequence.store(sequence, std::memory_order_relaxed);
    store_u64(record.wall_low, record.wall_high, input.wall_time_us);
    store_u64(record.cycle_low, record.cycle_high, input.guest_cycle);
    store_u64(record.frame_low, record.frame_high, input.guest_frame);
    record.registers.store(static_cast<std::uint32_t>(input.cs) |
                               (static_cast<std::uint32_t>(input.ip) << 16U),
                           std::memory_order_relaxed);
    record.flags_cx.store(static_cast<std::uint32_t>(input.flags) |
                              (static_cast<std::uint32_t>(input.cx) << 16U),
                          std::memory_order_relaxed);
    record.bp_cpu.store(static_cast<std::uint32_t>(input.bp) |
                            (static_cast<std::uint32_t>(input.interrupt_enabled)
                             << 16U) |
                            (static_cast<std::uint32_t>(input.next_opcode)
                             << 17U) |
                            (static_cast<std::uint32_t>(input.hlt) << 25U),
                        std::memory_order_relaxed);
    record.timer_status.store(
        static_cast<std::uint32_t>(input.timer_a_running) |
            (static_cast<std::uint32_t>(input.timer_b_running) << 1U) |
            (static_cast<std::uint32_t>(input.opna_status) << 8U),
        std::memory_order_relaxed);
    record.pic.store(static_cast<std::uint32_t>(input.pic_pending) |
                         (static_cast<std::uint32_t>(input.pic_mask) << 16U),
                     std::memory_order_relaxed);
    record.next_nevent_id.store(input.next_nevent_id,
                                std::memory_order_relaxed);
    record.next_nevent_remaining.store(
        static_cast<std::uint32_t>(input.next_nevent_remaining),
        std::memory_order_relaxed);
    store_u64(record.io_ordinal_low, record.io_ordinal_high,
              input.last_guest_io_ordinal);
    record.io_port.store(input.last_guest_io_port, std::memory_order_relaxed);
    store_u64(record.io_frame_low, record.io_frame_high,
              input.last_guest_io_frame);
    store_u64(record.io_cycle_low, record.io_cycle_high,
              input.last_guest_io_cycle);
    store_u64(record.horizon_low, record.horizon_high,
              input.published_horizon);
    store_u64(record.rendered_low, record.rendered_high,
              input.rendered_frame);
    record.guard.store(guard + 2U, std::memory_order_release);
    published_count_.store(sequence, std::memory_order_release);
}

void OwnerProgressDiagnostics::checkpoint_enter(
    const std::uint64_t now_us, const std::uint64_t guest_frame) noexcept
{
    std::uint32_t guard = checkpoint_stats_guard_.load(
        std::memory_order_relaxed);
    if ((guard & 1U) != 0U)
        ++guard;
    (void)checkpoint_stats_guard_.exchange(guard + 1U,
                                           std::memory_order_acq_rel);
    checkpoint_call_count_.fetch_add(1U, std::memory_order_relaxed);
    store_u64(checkpoint_last_enter_low_, checkpoint_last_enter_high_, now_us);
    store_u64(checkpoint_last_frame_low_, checkpoint_last_frame_high_,
              guest_frame);
    checkpoint_stats_guard_.store(guard + 2U, std::memory_order_release);
}

void OwnerProgressDiagnostics::checkpoint_exit(
    const std::uint64_t now_us, const std::uint32_t result,
    const bool success) noexcept
{
    std::uint32_t guard = checkpoint_stats_guard_.load(
        std::memory_order_relaxed);
    if ((guard & 1U) != 0U)
        ++guard;
    (void)checkpoint_stats_guard_.exchange(guard + 1U,
                                           std::memory_order_acq_rel);
    const std::uint64_t entered = load_u64(checkpoint_last_enter_low_,
                                           checkpoint_last_enter_high_);
    const std::uint64_t duration = now_us >= entered ? now_us - entered : 0U;
    const std::uint64_t prior_max = load_u64(checkpoint_max_duration_low_,
                                             checkpoint_max_duration_high_);
    store_u64(checkpoint_last_exit_low_, checkpoint_last_exit_high_, now_us);
    checkpoint_last_result_.store(result, std::memory_order_relaxed);
    if (duration > prior_max)
        store_u64(checkpoint_max_duration_low_,
                  checkpoint_max_duration_high_, duration);
    if (success)
        checkpoint_success_count_.fetch_add(1U, std::memory_order_relaxed);
    checkpoint_stats_guard_.store(guard + 2U, std::memory_order_release);
}

bool OwnerProgressDiagnostics::read_record(
    const AtomicRecord &source, const std::uint32_t expected_sequence,
    OwnerCheckpointRecord *record) noexcept
{
    for (std::uint32_t attempt = 0U; attempt < 8U; ++attempt) {
        const std::uint32_t before = source.guard.load(std::memory_order_acquire);
        if ((before & 1U) != 0U)
            continue;
        OwnerCheckpointRecord candidate{};
        candidate.checkpoint_sequence = source.checkpoint_sequence.load(
            std::memory_order_relaxed);
        candidate.wall_time_us = load_u64(source.wall_low, source.wall_high);
        candidate.guest_cycle = load_u64(source.cycle_low, source.cycle_high);
        candidate.guest_frame = load_u64(source.frame_low, source.frame_high);
        const std::uint32_t registers = source.registers.load(
            std::memory_order_relaxed);
        const std::uint32_t flags_cx = source.flags_cx.load(
            std::memory_order_relaxed);
        const std::uint32_t bp_cpu = source.bp_cpu.load(
            std::memory_order_relaxed);
        const std::uint32_t timer_status = source.timer_status.load(
            std::memory_order_relaxed);
        const std::uint32_t pic = source.pic.load(std::memory_order_relaxed);
        candidate.cs = static_cast<std::uint16_t>(registers);
        candidate.ip = static_cast<std::uint16_t>(registers >> 16U);
        candidate.flags = static_cast<std::uint16_t>(flags_cx);
        candidate.cx = static_cast<std::uint16_t>(flags_cx >> 16U);
        candidate.bp = static_cast<std::uint16_t>(bp_cpu);
        candidate.interrupt_enabled = static_cast<std::uint8_t>(
            (bp_cpu >> 16U) & 1U);
        candidate.next_opcode = static_cast<std::uint8_t>(bp_cpu >> 17U);
        candidate.hlt = static_cast<std::uint8_t>((bp_cpu >> 25U) & 1U);
        candidate.timer_a_running = static_cast<std::uint8_t>(timer_status & 1U);
        candidate.timer_b_running = static_cast<std::uint8_t>(
            (timer_status >> 1U) & 1U);
        candidate.opna_status = static_cast<std::uint8_t>(timer_status >> 8U);
        candidate.pic_pending = static_cast<std::uint16_t>(pic);
        candidate.pic_mask = static_cast<std::uint16_t>(pic >> 16U);
        candidate.next_nevent_id = source.next_nevent_id.load(
            std::memory_order_relaxed);
        candidate.next_nevent_remaining = static_cast<std::int32_t>(
            source.next_nevent_remaining.load(std::memory_order_relaxed));
        candidate.last_guest_io_ordinal = load_u64(
            source.io_ordinal_low, source.io_ordinal_high);
        candidate.last_guest_io_port = static_cast<std::uint16_t>(
            source.io_port.load(std::memory_order_relaxed));
        candidate.last_guest_io_frame = load_u64(source.io_frame_low,
                                                  source.io_frame_high);
        candidate.last_guest_io_cycle = load_u64(source.io_cycle_low,
                                                  source.io_cycle_high);
        candidate.published_horizon = load_u64(source.horizon_low,
                                                source.horizon_high);
        candidate.rendered_frame = load_u64(source.rendered_low,
                                             source.rendered_high);
        const std::uint32_t after = source.guard.load(std::memory_order_acquire);
        if (before == after && candidate.checkpoint_sequence ==
                                   expected_sequence) {
            candidate.valid = 1U;
            *record = candidate;
            return true;
        }
    }
    *record = {};
    record->checkpoint_sequence = expected_sequence;
    record->wall_time_us = UINT64_MAX;
    record->guest_cycle = UINT64_MAX;
    record->guest_frame = UINT64_MAX;
    record->last_guest_io_ordinal = UINT64_MAX;
    record->last_guest_io_frame = UINT64_MAX;
    record->last_guest_io_cycle = UINT64_MAX;
    record->published_horizon = UINT64_MAX;
    record->rendered_frame = UINT64_MAX;
    record->next_nevent_id = kOwnerDiagnosticUnavailable;
    return false;
}

OwnerProgressSnapshot OwnerProgressDiagnostics::snapshot() const noexcept
{
    OwnerProgressSnapshot result{};
    result.coherent = 1U;
    result.subphase = static_cast<OwnerSubphase>(
        subphase_.load(std::memory_order_acquire));
    result.pattern = OwnerProgressPattern::InsufficientHistory;
    const std::uint32_t published = published_count_.load(
        std::memory_order_acquire);
    result.checkpoint_count = std::min<std::uint32_t>(
        published, kOwnerCheckpointHistoryDepth);
    bool stats_coherent = false;
    for (std::uint32_t attempt = 0U; attempt < 8U; ++attempt) {
        const std::uint32_t before = checkpoint_stats_guard_.load(
            std::memory_order_acquire);
        if ((before & 1U) != 0U)
            continue;
        result.checkpoint_call_count = checkpoint_call_count_.load(
            std::memory_order_relaxed);
        result.checkpoint_success_count = checkpoint_success_count_.load(
            std::memory_order_relaxed);
        result.checkpoint_last_enter_us = load_u64(
            checkpoint_last_enter_low_, checkpoint_last_enter_high_);
        result.checkpoint_last_exit_us = load_u64(
            checkpoint_last_exit_low_, checkpoint_last_exit_high_);
        result.checkpoint_max_duration_us = load_u64(
            checkpoint_max_duration_low_, checkpoint_max_duration_high_);
        result.checkpoint_last_result = checkpoint_last_result_.load(
            std::memory_order_relaxed);
        result.checkpoint_last_frame = load_u64(
            checkpoint_last_frame_low_, checkpoint_last_frame_high_);
        const std::uint32_t after = checkpoint_stats_guard_.load(
            std::memory_order_acquire);
        if (before == after) {
            stats_coherent = true;
            break;
        }
    }
    if (!stats_coherent)
        result.coherent = 0U;

    const std::uint32_t first = published >= kOwnerCheckpointHistoryDepth
                                    ? published - kOwnerCheckpointHistoryDepth + 1U
                                    : 1U;
    for (std::uint32_t index = 0U; index < result.checkpoint_count; ++index) {
        const std::uint32_t sequence = first + index;
        if (!read_record(history_[(sequence - 1U) %
                                  kOwnerCheckpointHistoryDepth],
                         sequence, &result.history[index]))
            result.coherent = 0U;
    }
    for (std::uint32_t index = 1U; index < result.checkpoint_count; ++index) {
        const OwnerCheckpointRecord &older = result.history[index - 1U];
        const OwnerCheckpointRecord &newer = result.history[index];
        OwnerCheckpointInterval &interval = result.intervals[index - 1U];
        if (older.valid == 0U || newer.valid == 0U ||
            newer.wall_time_us <= older.wall_time_us ||
            newer.guest_cycle < older.guest_cycle ||
            newer.guest_frame < older.guest_frame)
            continue;
        interval.valid = 1U;
        interval.from_sequence = older.checkpoint_sequence;
        interval.to_sequence = newer.checkpoint_sequence;
        interval.delta_wall_us = newer.wall_time_us - older.wall_time_us;
        interval.delta_guest_cycles = newer.guest_cycle - older.guest_cycle;
        interval.delta_guest_frames = newer.guest_frame - older.guest_frame;
        interval.guest_cycles_per_second = rate_per_second(
            interval.delta_guest_cycles, interval.delta_wall_us);
        interval.guest_frames_per_second = rate_per_second(
            interval.delta_guest_frames, interval.delta_wall_us);
    }

    if (result.checkpoint_count >= 2U) {
        const OwnerCheckpointInterval &latest =
            result.intervals[result.checkpoint_count - 2U];
        if (latest.valid != 0U && latest.delta_guest_cycles == 0U &&
            latest.delta_guest_frames == 0U) {
            result.pattern = OwnerProgressPattern::Stopped;
        } else if (result.checkpoint_count == kOwnerCheckpointHistoryDepth &&
                   result.intervals[0].valid != 0U &&
                   result.intervals[1].valid != 0U &&
                   result.intervals[2].valid != 0U &&
                   result.intervals[0].delta_guest_cycles != 0U &&
                   result.intervals[1].delta_guest_cycles != 0U &&
                   result.intervals[2].delta_guest_cycles != 0U &&
                   rate_strictly_less(result.intervals[1],
                                      result.intervals[0]) &&
                   rate_strictly_less(result.intervals[2],
                                      result.intervals[1])) {
            result.pattern = OwnerProgressPattern::Decelerating;
        } else if (latest.valid != 0U &&
                   latest.delta_guest_cycles != 0U) {
            result.pattern = OwnerProgressPattern::SteadySlow;
        }
    }
    return result;
}

const char *owner_progress_pattern_name(
    const OwnerProgressPattern pattern) noexcept
{
    switch (pattern) {
    case OwnerProgressPattern::SteadySlow: return "STEADY_SLOW";
    case OwnerProgressPattern::Decelerating: return "DECELERATING";
    case OwnerProgressPattern::Stopped: return "STOPPED";
    default: return "INSUFFICIENT_HISTORY";
    }
}

const char *owner_subphase_name(const OwnerSubphase subphase) noexcept
{
    switch (subphase) {
    case OwnerSubphase::CpuExec: return "CPU_EXEC";
    case OwnerSubphase::NeventProgress: return "NEVENT_PROGRESS";
    case OwnerSubphase::CooperativeDelay: return "COOPERATIVE_DELAY";
    case OwnerSubphase::ProgressCheckpointEnter:
        return "PROGRESS_CHECKPOINT_ENTER";
    case OwnerSubphase::ProgressCheckpointWait:
        return "PROGRESS_CHECKPOINT_WAIT";
    case OwnerSubphase::ProgressCheckpointExit:
        return "PROGRESS_CHECKPOINT_EXIT";
    case OwnerSubphase::TransactionWait: return "TRANSACTION_WAIT";
    case OwnerSubphase::StatusCheck: return "STATUS_CHECK";
    case OwnerSubphase::TerminalTransition: return "TERMINAL_TRANSITION";
    default: return "UNAVAILABLE";
    }
}

} // namespace p4_nano_audio86_guest_binding
