/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef P4_NANO_AUDIO86_TERMINAL_PREDICATE_HPP
#define P4_NANO_AUDIO86_TERMINAL_PREDICATE_HPP

#include <cstdint>
#include <cstring>

#include "p4_nano_audio86_physical_sink/p4_nano_audio86_physical_sink.h"

namespace p4_nano_audio86_terminal_predicate {

template <typename Observer>
bool virtual_sink_observer_healthy(
    const Observer &observer, const std::uint32_t expected_slots,
    const std::uint32_t expected_partial_slots,
    const std::uint32_t minimum_first_submit_occupancy)
{
    return observer.pcm_consumed_slots == expected_slots &&
        observer.pcm_partial_slots == expected_partial_slots &&
        observer.pcm_sink_started == 1U &&
        observer.pcm_sink_finished == 1U &&
        observer.pcm_ack_after_finish == 1U &&
        observer.pcm_first_submit_occupancy >=
            minimum_first_submit_occupancy &&
        std::memcmp(observer.full_pcm, observer.ring_pcm,
                    sizeof(observer.full_pcm)) == 0;
}

template <typename Snapshot>
bool physical_snapshot_core_healthy(const Snapshot &snapshot)
{
    const p4_nano_audio86_physical_telemetry &sink = snapshot.sink;
    const std::uint32_t drain_post_snapshot_eofs =
        sink.drain_completion_epoch - sink.drain_snapshot_epoch;
    const std::uint32_t quiescent_post_snapshot_eofs =
        sink.quiescent_eof_epoch - sink.drain_snapshot_epoch;
    return snapshot.captured && snapshot.sink_destroyed == 1U &&
        snapshot.controller_state == NP2_PCM_OUTPUT_FINISHED &&
        sink.state == P4_NANO_AUDIO86_PHYSICAL_QUIESCENT &&
        sink.prepare_completed && sink.stream_started &&
        sink.finish_completed &&
        sink.semantic_accepted_frames == snapshot.controller_accepted_frames &&
        sink.semantic_accepted_bytes == snapshot.controller_accepted_bytes &&
        snapshot.controller_accepted_bytes ==
            snapshot.controller_accepted_frames *
                P4_NANO_AUDIO86_PHYSICAL_BYTES_PER_FRAME &&
        sink.accepted_pending_drain_frames == 0U &&
        sink.physically_drained_frames == sink.semantic_accepted_frames &&
        sink.physically_discarded_accepted_frames == 0U &&
        sink.running_queue_overflow_count == 0U &&
        drain_post_snapshot_eofs >=
            P4_NANO_AUDIO86_PHYSICAL_DMA_DESCRIPTORS &&
        drain_post_snapshot_eofs < 0x80000000U &&
        quiescent_post_snapshot_eofs < 0x80000000U &&
        quiescent_post_snapshot_eofs >= drain_post_snapshot_eofs &&
        sink.draining_queue_overflow_count <= quiescent_post_snapshot_eofs &&
        !sink.sticky_error && sink.callback_refcount == 0U &&
        !sink.callbacks_active && sink.codec_final_muted &&
        sink.pa_final_low && !sink.i2s_enabled && !sink.i2s_created &&
        snapshot.first_error == 0U && snapshot.forced_abort == 0U;
}

template <typename Snapshot>
bool physical_snapshot_healthy(const Snapshot &snapshot)
{
    const p4_nano_audio86_physical_telemetry &sink = snapshot.sink;
    return physical_snapshot_core_healthy(snapshot) &&
        sink.pa_initial_low && sink.codec_initialized_muted &&
        sink.i2s_initialized && sink.muted_warmup_completed &&
        sink.callbacks_registered && sink.codec_unmute_completed;
}

template <typename Snapshot>
bool physical_s1_snapshot_healthy(
    const Snapshot &snapshot, const std::uint64_t expected_frames)
{
    return physical_snapshot_core_healthy(snapshot) &&
        snapshot.controller_accepted_frames == expected_frames &&
        snapshot.controller_accepted_bytes == expected_frames *
            P4_NANO_AUDIO86_PHYSICAL_BYTES_PER_FRAME;
}

template <typename Snapshot>
bool physical_s2_snapshot_healthy(
    const Snapshot &snapshot, const std::uint64_t expected_frames,
    const std::uint64_t expected_units,
    const std::uint32_t expected_preloaded_units)
{
    const p4_nano_audio86_physical_telemetry &sink = snapshot.sink;
    return physical_snapshot_healthy(snapshot) &&
        snapshot.semantic_frames == expected_frames &&
        snapshot.semantic_bytes == expected_frames *
            P4_NANO_AUDIO86_PHYSICAL_BYTES_PER_FRAME &&
        snapshot.controller_accepted_frames == expected_frames &&
        snapshot.controller_accepted_bytes == snapshot.semantic_bytes &&
        sink.semantic_accepted_frames == expected_frames &&
        sink.semantic_accepted_bytes == snapshot.semantic_bytes &&
        sink.physical_units_copied == expected_units &&
        sink.physical_bytes_copied == expected_units *
            P4_NANO_AUDIO86_PHYSICAL_UNIT_BYTES &&
        sink.full_units == expected_units &&
        sink.final_partial_units == 0U && sink.final_valid_frames == 0U &&
        sink.physical_padding_frames == 0U &&
        sink.preloaded_units == expected_preloaded_units &&
        expected_units >= expected_preloaded_units &&
        sink.physical_units_copied - sink.preloaded_units ==
            expected_units - expected_preloaded_units &&
        sink.submit_attempts >= sink.physical_units_copied &&
        sink.submit_attempts - sink.physical_units_copied ==
            sink.retry_count &&
        sink.drain_duration_ms <
            P4_NANO_AUDIO86_PHYSICAL_DRAIN_TIMEOUT_MS;
}

constexpr bool normal_terminal_healthy(
    const bool common_ok, const bool selected_sink_ok)
{
    return common_ok && selected_sink_ok;
}

} // namespace p4_nano_audio86_terminal_predicate

#endif /* P4_NANO_AUDIO86_TERMINAL_PREDICATE_HPP */
