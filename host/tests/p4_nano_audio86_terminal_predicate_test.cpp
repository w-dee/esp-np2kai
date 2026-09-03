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
constexpr std::uint64_t kS2ExpectedFrames = 2400U;
constexpr std::uint64_t kS2ExpectedUnits = 10U;
constexpr std::uint32_t kS2ExpectedPreloadedUnits = 4U;

struct PhysicalSnapshot {
    p4_nano_audio86_physical_telemetry sink{};
    std::uint64_t semantic_frames = 0U;
    std::uint64_t semantic_bytes = 0U;
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
    /* Predicate projection of the retained second schema=2 physical run. */
    PhysicalSnapshot snapshot{};
    snapshot.captured = true;
    snapshot.sink_destroyed = 1U;
    snapshot.controller_state = NP2_PCM_OUTPUT_FINISHED;
    snapshot.controller_accepted_frames = kExpectedFrames;
    snapshot.controller_accepted_bytes = kExpectedFrames * 4U;
    snapshot.semantic_frames = kExpectedFrames;
    snapshot.semantic_bytes = kExpectedFrames * 4U;
    snapshot.sink.state = P4_NANO_AUDIO86_PHYSICAL_QUIESCENT;
    snapshot.sink.prepare_completed = true;
    snapshot.sink.pa_initial_low = true;
    snapshot.sink.codec_initialized_muted = true;
    snapshot.sink.i2s_initialized = true;
    snapshot.sink.muted_warmup_completed = true;
    snapshot.sink.callbacks_registered = true;
    snapshot.sink.stream_started = true;
    snapshot.sink.codec_unmute_completed = true;
    snapshot.sink.finish_completed = true;
    snapshot.sink.semantic_accepted_frames = kExpectedFrames;
    snapshot.sink.semantic_accepted_bytes = kExpectedFrames * 4U;
    snapshot.sink.accepted_pending_drain_frames = 0U;
    snapshot.sink.physically_drained_frames = kExpectedFrames;
    snapshot.sink.physically_discarded_accepted_frames = 0U;
    snapshot.sink.running_queue_overflow_count = 0U;
    snapshot.sink.drain_snapshot_epoch = 0U;
    snapshot.sink.drain_completion_epoch = 4U;
    snapshot.sink.quiescent_eof_epoch = 4U;
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

PhysicalSnapshot healthy_s2_snapshot()
{
    PhysicalSnapshot snapshot = healthy_snapshot();
    snapshot.semantic_frames = kS2ExpectedFrames;
    snapshot.semantic_bytes = kS2ExpectedFrames * 4U;
    snapshot.controller_accepted_frames = kS2ExpectedFrames;
    snapshot.controller_accepted_bytes = kS2ExpectedFrames * 4U;
    snapshot.sink.semantic_accepted_frames = kS2ExpectedFrames;
    snapshot.sink.semantic_accepted_bytes = kS2ExpectedFrames * 4U;
    snapshot.sink.physical_units_copied = kS2ExpectedUnits;
    snapshot.sink.physical_bytes_copied = kS2ExpectedUnits * 960U;
    snapshot.sink.full_units = kS2ExpectedUnits;
    snapshot.sink.final_partial_units = 0U;
    snapshot.sink.final_valid_frames = 0U;
    snapshot.sink.physical_padding_frames = 0U;
    snapshot.sink.preloaded_units = kS2ExpectedPreloadedUnits;
    snapshot.sink.submit_attempts = kS2ExpectedUnits;
    snapshot.sink.retry_count = 0U;
    snapshot.sink.drain_duration_ms = 4U;
    snapshot.sink.physically_drained_frames = kS2ExpectedFrames;
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
    require(predicate::physical_snapshot_healthy(healthy),
            "healthy workload-agnostic physical snapshot rejected");
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

    PhysicalSnapshot healthy_s2 = healthy_s2_snapshot();
    require(predicate::physical_s2_snapshot_healthy(
                healthy_s2, kS2ExpectedFrames, kS2ExpectedUnits,
                kS2ExpectedPreloadedUnits),
            "healthy S2 physical snapshot rejected");
    require(predicate::normal_terminal_healthy(
                true, predicate::physical_s2_snapshot_healthy(
                          healthy_s2, kS2ExpectedFrames, kS2ExpectedUnits,
                          kS2ExpectedPreloadedUnits)),
            "healthy full physical path with virtual defaults did not complete");

    const NegativeCase cases[] = {
        {"snapshot_missing", +[](PhysicalSnapshot &s) { s.captured = false; }},
        {"sink_not_destroyed", +[](PhysicalSnapshot &s) { s.sink_destroyed = 0U; }},
        {"controller_not_finished", +[](PhysicalSnapshot &s) { s.controller_state = NP2_PCM_OUTPUT_STARTED; }},
        {"sink_not_quiescent", +[](PhysicalSnapshot &s) { s.sink.state = P4_NANO_AUDIO86_PHYSICAL_DRAINING; }},
        {"prepare_failed", +[](PhysicalSnapshot &s) { s.sink.prepare_completed = false; }},
        {"pa_initial_not_low", +[](PhysicalSnapshot &s) { s.sink.pa_initial_low = false; }},
        {"codec_initial_not_muted", +[](PhysicalSnapshot &s) { s.sink.codec_initialized_muted = false; }},
        {"i2s_not_initialized", +[](PhysicalSnapshot &s) { s.sink.i2s_initialized = false; }},
        {"warmup_incomplete", +[](PhysicalSnapshot &s) { s.sink.muted_warmup_completed = false; }},
        {"callbacks_not_registered", +[](PhysicalSnapshot &s) { s.sink.callbacks_registered = false; }},
        {"stream_not_started", +[](PhysicalSnapshot &s) { s.sink.stream_started = false; }},
        {"codec_not_unmuted", +[](PhysicalSnapshot &s) { s.sink.codec_unmute_completed = false; }},
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
        require(!predicate::physical_snapshot_healthy(snapshot), test.name);
        const bool s1_legacy_ignored_startup_field =
            std::strcmp(test.name, "pa_initial_not_low") == 0 ||
            std::strcmp(test.name, "codec_initial_not_muted") == 0 ||
            std::strcmp(test.name, "i2s_not_initialized") == 0 ||
            std::strcmp(test.name, "warmup_incomplete") == 0 ||
            std::strcmp(test.name, "callbacks_not_registered") == 0 ||
            std::strcmp(test.name, "codec_not_unmuted") == 0;
        require(predicate::physical_s1_snapshot_healthy(
                    snapshot, kExpectedFrames) ==
                    s1_legacy_ignored_startup_field,
                "S1 legacy predicate truth table changed");
    }

    const NegativeCase s2_cases[] = {
        {"semantic_frames", +[](PhysicalSnapshot &s) { --s.semantic_frames; }},
        {"semantic_bytes", +[](PhysicalSnapshot &s) { s.semantic_bytes -= 4U; }},
        {"physical_units", +[](PhysicalSnapshot &s) { --s.sink.physical_units_copied; }},
        {"physical_bytes", +[](PhysicalSnapshot &s) { s.sink.physical_bytes_copied -= 960U; }},
        {"full_units", +[](PhysicalSnapshot &s) { --s.sink.full_units; }},
        {"final_partial", +[](PhysicalSnapshot &s) { s.sink.final_partial_units = 1U; }},
        {"final_valid", +[](PhysicalSnapshot &s) { s.sink.final_valid_frames = 1U; }},
        {"padding", +[](PhysicalSnapshot &s) { s.sink.physical_padding_frames = 1U; }},
        {"preloaded_units", +[](PhysicalSnapshot &s) { --s.sink.preloaded_units; }},
        {"submit_attempts", +[](PhysicalSnapshot &s) { ++s.sink.submit_attempts; }},
        {"retry_relation", +[](PhysicalSnapshot &s) {
             s.sink.retry_count = 1U;
         }},
        {"drain_timeout", +[](PhysicalSnapshot &s) {
             s.sink.drain_duration_ms =
                 P4_NANO_AUDIO86_PHYSICAL_DRAIN_TIMEOUT_MS;
         }},
    };
    for (const NegativeCase &test : s2_cases) {
        PhysicalSnapshot snapshot = healthy_s2;
        test.mutate(snapshot);
        require(!predicate::physical_s2_snapshot_healthy(
                    snapshot, kS2ExpectedFrames, kS2ExpectedUnits,
                    kS2ExpectedPreloadedUnits),
                test.name);
    }
    PhysicalSnapshot healthy_s2_retry = healthy_s2;
    healthy_s2_retry.sink.submit_attempts = kS2ExpectedUnits + 1U;
    healthy_s2_retry.sink.retry_count = 1U;
    require(predicate::physical_s2_snapshot_healthy(
                healthy_s2_retry, kS2ExpectedFrames, kS2ExpectedUnits,
                kS2ExpectedPreloadedUnits),
            "valid natural S2 retry rejected");

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
    std::printf("GENERIC_PHYSICAL_HEALTH_WORKLOAD_AGNOSTIC=PASS\n");
    std::printf("S1_PHYSICAL_HEALTH_NONREGRESSION=PASS\n");
    std::printf("S1_PHYSICAL_HEALTH_LEGACY_TRUTH_TABLE=%zu/%zu_PASS\n",
                case_count, case_count);
    constexpr std::size_t s2_case_count =
        sizeof(s2_cases) / sizeof(s2_cases[0]);
    std::printf("S2_WORKLOAD_PREDICATE_NEGATIVE_MATRIX=%zu/%zu_PASS\n",
                s2_case_count, s2_case_count);
    std::printf("S2_FULL_PHYSICAL_WITH_VIRTUAL_DEFAULTS_TERMINAL=PASS\n");
    return 0;
}
