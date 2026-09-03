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
bool physical_s1_snapshot_healthy(
    const Snapshot &snapshot, const std::uint64_t expected_frames)
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
        snapshot.controller_accepted_frames == expected_frames &&
        snapshot.controller_accepted_bytes == expected_frames *
            P4_NANO_AUDIO86_PHYSICAL_BYTES_PER_FRAME &&
        sink.semantic_accepted_frames == snapshot.controller_accepted_frames &&
        sink.semantic_accepted_bytes == snapshot.controller_accepted_bytes &&
        sink.accepted_pending_drain_frames == 0U &&
        sink.physically_drained_frames == expected_frames &&
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

constexpr bool normal_terminal_healthy(
    const bool common_ok, const bool selected_sink_ok)
{
    return common_ok && selected_sink_ok;
}

} // namespace p4_nano_audio86_terminal_predicate

#endif /* P4_NANO_AUDIO86_TERMINAL_PREDICATE_HPP */
