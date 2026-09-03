/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "p4_nano_audio86_guest_binding/p4_nano_audio86_terminal_predicate.hpp"

namespace predicate = p4_nano_audio86_terminal_predicate;

namespace {

constexpr std::uint64_t kExpectedFrames = 13U;
constexpr std::size_t kPcmBytes = kExpectedFrames * 4U;

struct PhysicalSnapshot {
    p4_nano_audio86_physical_telemetry sink{};
    std::uint64_t controller_accepted_frames = 0U;
    std::uint64_t controller_accepted_bytes = 0U;
    np2_pcm_output_state controller_state = NP2_PCM_OUTPUT_INITIAL;
    std::uint32_t first_error = 0U;
    std::uint32_t forced_abort = 0U;
    std::uint32_t sink_destroyed = 0U;
    bool captured = false;
};

struct VirtualObserver {
    std::uint32_t pcm_consumed_slots = 0U;
    std::uint32_t pcm_partial_slots = 0U;
    std::uint32_t pcm_sink_started = 0U;
    std::uint32_t pcm_sink_finished = 0U;
    std::uint32_t pcm_ack_after_finish = 0U;
    std::uint32_t pcm_first_submit_occupancy = 0U;
    std::uint8_t full_pcm[kPcmBytes]{};
    std::uint8_t ring_pcm[kPcmBytes]{};
};

PhysicalSnapshot healthy_snapshot()
{
    PhysicalSnapshot snapshot{};
    snapshot.captured = true;
    snapshot.sink_destroyed = 1U;
    snapshot.controller_state = NP2_PCM_OUTPUT_FINISHED;
    snapshot.controller_accepted_frames = kExpectedFrames;
    snapshot.controller_accepted_bytes = kExpectedFrames * 4U;
    snapshot.sink.state = P4_NANO_AUDIO86_PHYSICAL_QUIESCENT;
    snapshot.sink.prepare_completed = true;
    snapshot.sink.stream_started = true;
    snapshot.sink.finish_completed = true;
    snapshot.sink.semantic_accepted_frames = kExpectedFrames;
    snapshot.sink.semantic_accepted_bytes = kExpectedFrames * 4U;
    snapshot.sink.accepted_pending_drain_frames = 0U;
    snapshot.sink.physically_drained_frames = kExpectedFrames;
    snapshot.sink.physically_discarded_accepted_frames = 0U;
    snapshot.sink.running_queue_overflow_count = 0U;
    snapshot.sink.drain_snapshot_epoch = UINT32_MAX - 1U;
    snapshot.sink.drain_completion_epoch = 2U;
    snapshot.sink.quiescent_eof_epoch = 2U;
    snapshot.sink.draining_queue_overflow_count = 4U;
    snapshot.sink.sticky_error = false;
    snapshot.sink.callback_refcount = 0U;
    snapshot.sink.callbacks_active = false;
    snapshot.sink.codec_final_muted = true;
    snapshot.sink.pa_final_low = true;
    snapshot.sink.i2s_enabled = false;
    snapshot.sink.i2s_created = false;
    snapshot.first_error = 0U;
    snapshot.forced_abort = 0U;
    return snapshot;
}

void require(const bool condition, const char *message)
{
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

using Mutation = void (*)(PhysicalSnapshot &);

struct NegativeCase {
    const char *name;
    Mutation mutate;
};

} // namespace

int main()
{
    PhysicalSnapshot healthy = healthy_snapshot();
    require(predicate::physical_s1_snapshot_healthy(
                healthy, kExpectedFrames),
            "healthy schema=2 physical snapshot rejected");

    VirtualObserver natural_physical_observers{};
    natural_physical_observers.full_pcm[0] = 1U;
    const bool virtual_ok = predicate::virtual_sink_observer_healthy(
        natural_physical_observers, 1U, 1U, 1U);
    require(!virtual_ok, "natural physical virtual observers became healthy");
    require(predicate::normal_terminal_healthy(
                true, predicate::physical_s1_snapshot_healthy(
                          healthy, kExpectedFrames)),
            "healthy physical path with virtual defaults did not complete");

    const NegativeCase cases[] = {
        {"snapshot_missing", +[](PhysicalSnapshot &s) { s.captured = false; }},
        {"sink_not_destroyed", +[](PhysicalSnapshot &s) { s.sink_destroyed = 0U; }},
        {"controller_not_finished", +[](PhysicalSnapshot &s) { s.controller_state = NP2_PCM_OUTPUT_STARTED; }},
        {"sink_not_quiescent", +[](PhysicalSnapshot &s) { s.sink.state = P4_NANO_AUDIO86_PHYSICAL_DRAINING; }},
        {"prepare_failed", +[](PhysicalSnapshot &s) { s.sink.prepare_completed = false; }},
        {"stream_not_started", +[](PhysicalSnapshot &s) { s.sink.stream_started = false; }},
        {"finish_failed", +[](PhysicalSnapshot &s) { s.sink.finish_completed = false; }},
        {"controller_frames_wrong", +[](PhysicalSnapshot &s) { --s.controller_accepted_frames; }},
        {"controller_bytes_wrong", +[](PhysicalSnapshot &s) { s.controller_accepted_bytes -= 4U; }},
        {"sink_frames_wrong", +[](PhysicalSnapshot &s) { --s.sink.semantic_accepted_frames; }},
        {"sink_bytes_wrong", +[](PhysicalSnapshot &s) { s.sink.semantic_accepted_bytes -= 4U; }},
        {"pending_nonzero", +[](PhysicalSnapshot &s) { s.sink.accepted_pending_drain_frames = 1U; }},
        {"drained_wrong", +[](PhysicalSnapshot &s) { --s.sink.physically_drained_frames; }},
        {"discarded_nonzero", +[](PhysicalSnapshot &s) { s.sink.physically_discarded_accepted_frames = 1U; }},
        {"running_q_ovf_nonzero", +[](PhysicalSnapshot &s) { s.sink.running_queue_overflow_count = 1U; }},
        {"drain_delta_insufficient", +[](PhysicalSnapshot &s) { s.sink.drain_completion_epoch = 1U; }},
        {"drain_delta_ambiguous", +[](PhysicalSnapshot &s) { s.sink.drain_completion_epoch = UINT32_MAX - 2U; }},
        {"quiescent_delta_ambiguous", +[](PhysicalSnapshot &s) { s.sink.quiescent_eof_epoch = UINT32_MAX - 2U; }},
        {"quiescent_before_drain", +[](PhysicalSnapshot &s) { s.sink.quiescent_eof_epoch = 1U; }},
        {"draining_q_ovf_exceeds_quiescent", +[](PhysicalSnapshot &s) { s.sink.draining_queue_overflow_count = 5U; }},
        {"sticky_error", +[](PhysicalSnapshot &s) { s.sink.sticky_error = true; }},
        {"callback_in_flight", +[](PhysicalSnapshot &s) { s.sink.callback_refcount = 1U; }},
        {"callbacks_active", +[](PhysicalSnapshot &s) { s.sink.callbacks_active = true; }},
        {"codec_not_muted", +[](PhysicalSnapshot &s) { s.sink.codec_final_muted = false; }},
        {"pa_not_low", +[](PhysicalSnapshot &s) { s.sink.pa_final_low = false; }},
        {"i2s_enabled", +[](PhysicalSnapshot &s) { s.sink.i2s_enabled = true; }},
        {"i2s_created", +[](PhysicalSnapshot &s) { s.sink.i2s_created = true; }},
        {"first_error", +[](PhysicalSnapshot &s) { s.first_error = 1U; }},
        {"forced_abort", +[](PhysicalSnapshot &s) { s.forced_abort = 1U; }},
    };
    for (const NegativeCase &test : cases) {
        PhysicalSnapshot snapshot = healthy;
        test.mutate(snapshot);
        require(!predicate::normal_terminal_healthy(
                    true, predicate::physical_s1_snapshot_healthy(
                              snapshot, kExpectedFrames)),
                test.name);
    }

    VirtualObserver virtual_observer{};
    virtual_observer.pcm_consumed_slots = 1U;
    virtual_observer.pcm_partial_slots = 1U;
    virtual_observer.pcm_sink_started = 1U;
    virtual_observer.pcm_sink_finished = 1U;
    virtual_observer.pcm_ack_after_finish = 1U;
    virtual_observer.pcm_first_submit_occupancy = 1U;
    std::memset(virtual_observer.full_pcm, 0x5a, kPcmBytes);
    std::memset(virtual_observer.ring_pcm, 0x5a, kPcmBytes);
    require(predicate::virtual_sink_observer_healthy(
                virtual_observer, 1U, 1U, 1U),
            "healthy virtual observer rejected");

    std::printf("HEALTHY_PHYSICAL_WITH_VIRTUAL_DEFAULTS_TERMINAL_PASS=PASS\n");
    std::printf("SECOND_RUN_COUNTERFACTUAL_TERMINAL_CLASSIFICATION=COMPLETE_UNDER_FIXED_FIRMWARE\n");
    constexpr std::size_t case_count = sizeof(cases) / sizeof(cases[0]);
    std::printf("PHYSICAL_TERMINAL_NEGATIVE_MATRIX=%zu/%zu_PASS\n",
                case_count, case_count);
    std::printf("VIRTUAL_SINK_OBSERVER_PREDICATE=PASS\n");
    return 0;
}
