/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef P4_NANO_AUDIO86_TERMINAL_WORKER_TIMING_HPP
#define P4_NANO_AUDIO86_TERMINAL_WORKER_TIMING_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace p4_nano_audio86_terminal_worker_timing {

constexpr std::uint32_t kUnset = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t kPointCount = 11U;
constexpr std::size_t kLogicalPayloadBytes =
    kPointCount * sizeof(std::uint32_t) + 2U * sizeof(std::uint32_t);
constexpr std::uint32_t kConsumerSequenceBits = 9U;
constexpr std::uint32_t kConsumerSequenceMask =
    (1U << kConsumerSequenceBits) - 1U;

enum class Point : std::uint32_t {
    T0TerminalPairObserved = 0U,
    T1PreResetRenderComplete,
    T2ResetActionBegin,
    T3ResetActionComplete,
    T4ResetEvidenceComplete,
    T5ResetAckPublished,
    T6TerminalPredicateReady,
    T7PostResetRenderBegin,
    T8PostResetSynthesisComplete,
    T9Q399Published,
    T10PcmFinishComplete,
};

enum class Phase : std::uint32_t {
    None = 0U,
    TerminalObserved,
    PreResetRender,
    ResetApply,
    ResetEvidence,
    ResetAck,
    ResetEventConsume,
    PostResetRender,
    Q399Publish,
    PcmFinish,
};

struct Snapshot {
    std::array<std::uint32_t, kPointCount> timestamps{};
    std::atomic<std::uint32_t> current_phase{
        static_cast<std::uint32_t>(Phase::None)};
    std::atomic<std::uint32_t> first_qovf_phase{
        static_cast<std::uint32_t>(Phase::None)};
};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(kLogicalPayloadBytes == 52U);
static_assert(sizeof(Snapshot) == kLogicalPayloadBytes);

constexpr bool valid_phase(const std::uint32_t value)
{
    return value <= static_cast<std::uint32_t>(Phase::PcmFinish);
}

constexpr std::uint32_t pack_service_sequence(
    const std::uint32_t consumer_sequence, const Phase phase)
{
    return (consumer_sequence & kConsumerSequenceMask) |
           (static_cast<std::uint32_t>(phase) << kConsumerSequenceBits);
}

constexpr std::uint32_t unpack_service_phase(
    const std::uint32_t packed_sequence)
{
    return packed_sequence >> kConsumerSequenceBits;
}

constexpr std::uint32_t unpack_consumer_sequence(
    const std::uint32_t packed_sequence)
{
    return packed_sequence & kConsumerSequenceMask;
}

constexpr std::size_t point_index(const Point point)
{
    return static_cast<std::size_t>(point);
}

inline void initialize(Snapshot *const snapshot)
{
    if (snapshot == nullptr) return;
    snapshot->timestamps.fill(kUnset);
    snapshot->current_phase.store(static_cast<std::uint32_t>(Phase::None),
                                  std::memory_order_relaxed);
    snapshot->first_qovf_phase.store(static_cast<std::uint32_t>(Phase::None),
                                     std::memory_order_relaxed);
}

inline void publish_phase(Snapshot *const snapshot, const Phase phase)
{
    if (snapshot == nullptr) return;
    snapshot->current_phase.store(static_cast<std::uint32_t>(phase),
                                  std::memory_order_release);
}

/* Timestamps are worker-owned.  The non-atomic array is intentionally bounded
 * and single-writer; callback-visible state is confined to the two uint32_t
 * atomics above. */
inline bool record_once(Snapshot *const snapshot, const Point point,
                        const std::uint32_t relative_us)
{
    if (snapshot == nullptr || relative_us == kUnset ||
        point_index(point) >= snapshot->timestamps.size())
        return false;
    std::uint32_t &slot = snapshot->timestamps[point_index(point)];
    if (slot != kUnset) return false;
    slot = relative_us;
    return true;
}

/* The physical callback owns first-occurrence selection.  This copies only
 * its already-frozen phase into runtime-owned final evidence. */
inline bool freeze_first_qovf_phase(Snapshot *const snapshot,
                                    const std::uint32_t frozen_phase)
{
    if (snapshot == nullptr || !valid_phase(frozen_phase)) return false;
    std::uint32_t expected = static_cast<std::uint32_t>(Phase::None);
    return snapshot->first_qovf_phase.compare_exchange_strong(
        expected, frozen_phase, std::memory_order_release,
        std::memory_order_acquire) || expected == frozen_phase;
}

constexpr std::uint32_t duration(const std::uint32_t begin,
                                 const std::uint32_t end)
{
    return begin == kUnset || end == kUnset || end < begin
        ? kUnset : end - begin;
}

inline bool reached_prefix_is_monotonic(const Snapshot &snapshot)
{
    bool saw_unset = false;
    std::uint32_t previous = 0U;
    bool have_previous = false;
    for (const std::uint32_t value : snapshot.timestamps) {
        if (value == kUnset) {
            saw_unset = true;
            continue;
        }
        if (saw_unset || (have_previous && value < previous)) return false;
        previous = value;
        have_previous = true;
    }
    return true;
}

} // namespace p4_nano_audio86_terminal_worker_timing

#endif /* P4_NANO_AUDIO86_TERMINAL_WORKER_TIMING_HPP */
