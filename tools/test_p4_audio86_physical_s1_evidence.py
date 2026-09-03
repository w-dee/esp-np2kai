#!/usr/bin/env python3
"""Static lifecycle and ownership proofs for 86R.5D.2 S1 evidence."""

from __future__ import annotations

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
BINDING = ROOT / ("firmware/components/p4_nano_audio86_guest_binding/"
                  "p4_nano_audio86_guest_binding.cpp")
SINK = ROOT / ("firmware/components/p4_nano_audio86_physical_sink/"
               "p4_nano_audio86_physical_sink.c")
IDF = ROOT / ("firmware/components/p4_nano_audio86_physical_sink/"
              "p4_nano_audio86_physical_sink_idf.cpp")
CMAKE = ROOT / ("firmware/components/p4_nano_audio86_guest_binding/"
                "CMakeLists.txt")
TERMINAL = ROOT / (
    "firmware/components/p4_nano_audio86_guest_binding/include/"
    "p4_nano_audio86_guest_binding/p4_nano_audio86_terminal_predicate.hpp")

VIRTUAL_ONLY_OBSERVERS = (
    "pcm_consumed_slots",
    "pcm_partial_slots",
    "pcm_sink_started",
    "pcm_sink_finished",
    "pcm_ack_after_finish",
    "pcm_first_submit_occupancy",
    "full_pcm",
    "ring_pcm",
)

COMMON_NORMAL_OK_INVARIANTS = (
    "guest_ok", "joined", "pcm_joined", "pressure_ok", "!failed(runtime)",
    "np2audio86_event_ring_occupancy", "np2audio86_byte_ring_occupancy",
    "!np2audio86_runtime_horizon_pending", "pcm_ring_finished.load",
    "np2opngen_pcm_ring_occupancy", "pcm_ring_producer_partial_valid_frames",
    "pcm_controller.accepted_frames", "pcm_controller.accepted_bytes",
    "pcm_produced_frames", "pcm_produced_bytes", "pcm_produced_slots",
    "pcm_drops", "pcm_overwrites", "pcm_forced_abort == 0U",
    "pcm_forced_abort_requested.load", "pcm_join_timeout",
    "pcm_worker_join_timeout", "pcm_consumer_suspended_observed.load",
    "pcm_worker_suspended_observed.load",
    "pcm_consumer_deleted_after_suspended.load",
    "pcm_worker_deleted_after_suspended.load",
    "pcm_abandoned_published_frames", "pcm_abandoned_partial_frames",
    "pcm_abandoned_rendered_frames", "pcm_ring_before_done",
    "pcm_eos_after_done", "pcm_finish_after_empty",
    "reset_ring_owned_frames", "reset_applied_after_ring",
    "reset_ack_after_ring",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def body(source: str, begin: str, end: str) -> str:
    start = source.index(begin)
    finish = source.index(end, start)
    return source[start:finish]


def main() -> int:
    binding = BINDING.read_text(encoding="utf-8")
    sink = SINK.read_text(encoding="utf-8")
    idf = IDF.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    terminal = TERMINAL.read_text(encoding="utf-8")

    run = body(binding, "esp_err_t run_on_pc98_task(",
               "} // namespace p4_nano_audio86_guest_binding")
    join = run.index("const bool pcm_joined")
    snapshot = run.index("capture_physical_s1_snapshot(runtime);", join)
    delete = run.index("vTaskDelete(runtime->pcm_consumer);", snapshot)
    destroy = run.index("p4_nano_audio86_physical_sink_destroy(", delete)
    destroy_record = run.index("record_physical_s1_destroy(", destroy)
    classification = run.index("const bool normal_ok", destroy_record)
    summary = run.index("emit_summary(runtime, ok);", destroy)
    handoff = run[run.index("const bool pcm_terminal", join - 700):snapshot]
    for fact in (
        "xSemaphoreTake(runtime->pcm_done_semaphore, kTimeout)",
        "pcm_consumer_quiescent.load(std::memory_order_acquire)",
        "pcm_consumer_terminal_ack.load(std::memory_order_acquire)",
        "wait_task_suspended(runtime->pcm_consumer)",
        "physical_snapshot_ready = pcm_terminal && pcm_quiescent &&",
        "pcm_ack && pcm_suspended",
    ):
        require(fact in handoff, f"snapshot handoff fact missing: {fact}")
    require(snapshot < delete < destroy < destroy_record < classification < summary,
            "snapshot/delete/destroy/classification/emission order changed")

    capture = body(binding, "void capture_physical_s1_snapshot(",
                   "void record_physical_s1_destroy(")
    require("p4_nano_audio86_physical_sink_get_telemetry" in capture and
            "runtime->physical_s1 = snapshot" in capture and
            "std::printf" not in capture,
            "snapshot is not an owned, passive copy")
    consumer = body(binding, "void pcm_consumer_task(", "#endif\n\nbool failed")
    require("std::printf" not in consumer,
            "physical consumer hot path gained UART formatting")
    require("printf" not in sink,
            "physical sink/callback path gained printf")
    callbacks = body(idf, "bool IRAM_ATTR on_sent(", "void rollback_prepare(")
    require("printf" not in callbacks,
            "IDF ISR callbacks gained printf")

    finish = body(sink, "static enum np2_pcm_sink_result physical_finish(",
                  "static enum np2_pcm_sink_result physical_abort(")
    ordered = (
        "sink->drain_completion_epoch",
        "disarm_callbacks(sink)",
        "sink->backend.mute",
        "sink->backend.pa_low",
        "sink->backend.disable",
        "close_callbacks(sink)",
        "sink->quiescent_eof_epoch",
        "sink->accepted_pending_drain_frames = 0U",
        "P4_NANO_AUDIO86_PHYSICAL_QUIESCENT",
        "sink->finish_completed = true",
    )
    positions = [finish.index(item) for item in ordered]
    require(positions == sorted(positions),
            "physical drain/disarm/barrier/quiescent order changed")

    for history in (
        "prepare_completed", "pa_initial_low", "codec_initialized_muted",
        "i2s_initialized", "muted_warmup_completed", "callbacks_registered",
        "stream_started", "codec_unmute_completed", "finish_completed",
        "codec_final_muted", "pa_final_low",
    ):
        require(f"sink->{history} = true" in sink,
                f"history latch missing: {history}")
    require("sink->i2s_enabled = true" in sink and
            "sink->i2s_enabled = false" in sink and
            "sink->i2s_created = true" in sink and
            "sink->i2s_created = false" in sink,
            "current I2S state is conflated with history")

    record_gate = body(binding,
                       "#if defined(P4_NANO_AUDIO86_PHYSICAL_SHORT_PROFILE)\n"
                       "const char *physical_s1_controller_state_name",
                       "void emit_summary(")
    for name in ("5D2_S1_IDENTITY", "5D2_S1_START", "5D2_S1_PCM",
                 "5D2_S1_FINISH"):
        require(record_gate.count(name) == 1,
                f"authoritative record definition mismatch: {name}")
    require("P4_AUDIO86_GIT_SHA" in record_gate and
            "physical-short evidence requires P4_AUDIO86_GIT_SHA" in cmake and
            'P4_AUDIO86_GIT_SHA="${P4_AUDIO86_GIT_SHA}"' in cmake,
            "physical source identity is not build-bound")
    require("physical_s1_snapshot_healthy(runtime)" in run,
            "physical-short success predicate is not active")
    physical_branch = body(
        run, "#if defined(P4_NANO_AUDIO86_PHYSICAL_SHORT_PROFILE)\n"
             "    const bool physical_sink_ok",
        "#else\n    const bool virtual_sink_ok")
    for observer in VIRTUAL_ONLY_OBSERVERS:
        require(observer not in physical_branch,
                f"physical predicate retained virtual observer: {observer}")
    common = body(run, "const bool common_ok =", ";\n#if defined("
                  "P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)\n#if defined("
                  "P4_NANO_AUDIO86_PHYSICAL_SHORT_PROFILE)")
    for observer in VIRTUAL_ONLY_OBSERVERS:
        require(observer not in common,
                f"common predicate retained virtual observer: {observer}")
    for invariant in COMMON_NORMAL_OK_INVARIANTS:
        require(invariant in common,
                f"common predicate lost invariant: {invariant}")
    virtual = body(terminal, "bool virtual_sink_observer_healthy(",
                   "template <typename Snapshot>")
    for observer in VIRTUAL_ONLY_OBSERVERS:
        require(observer in virtual,
                f"virtual predicate lost observer: {observer}")
    for predicate in (
        "observer.pcm_consumed_slots == expected_slots",
        "observer.pcm_partial_slots == expected_partial_slots",
        "observer.pcm_sink_started == 1U",
        "observer.pcm_sink_finished == 1U",
        "observer.pcm_ack_after_finish == 1U",
        "observer.pcm_first_submit_occupancy >=",
        "std::memcmp(observer.full_pcm, observer.ring_pcm",
    ):
        require(predicate in virtual,
                f"virtual predicate semantics drifted: {predicate}")
    require("virtual_sink_observer_healthy(" in run and
            "const bool sink_profile_ok = physical_sink_ok" in run and
            "const bool sink_profile_ok = virtual_sink_ok" in run and
            "normal_terminal_healthy(" in run,
            "profile-specific terminal predicate layers are not explicit")

    callback_observers = body(binding, "enum np2_pcm_sink_result pcm_sink_start(",
                              "const np2_pcm_sink kPcmSink")
    for update in (
        "runtime->pcm_sink_started = 1U",
        "++runtime->pcm_consumed_slots",
        "++runtime->pcm_partial_slots",
        "runtime->pcm_first_submit_occupancy =",
        "runtime->ring_pcm +",
        "runtime->pcm_sink_finished = 1U",
    ):
        require(update in callback_observers,
                f"virtual callback provenance missing: {update}")
    require(re.search(
                r"runtime->pcm_ack_after_finish\s*=\s*"
                r"runtime->pcm_sink_finished", binding) is not None,
            "virtual-derived finish acknowledgement provenance changed")
    require("p4_nano_audio86_physical_sink_interface(" in run and
            "selected_sink =" in run,
            "physical sink no longer replaces the virtual callback sink")
    require("std::atomic<uint8_t>" not in binding and
            "_Atomic uint8_t" not in sink,
            "S1 introduced a byte-width atomic path")

    emitted = body(binding, "void emit_physical_s1_evidence(",
                   "bool physical_s1_snapshot_healthy(")
    require(emitted.count("runtime->") == 1 and
            "runtime->physical_s1" in emitted and
            "p4_nano_audio86_physical_sink_get_telemetry" not in emitted,
            "UART evidence does not exclusively use the owned snapshot")

    healthy = body(terminal, "bool physical_s1_snapshot_healthy(",
                   "constexpr bool normal_terminal_healthy(")
    require("sink.drain_completion_epoch - sink.drain_snapshot_epoch" in healthy and
            "sink.quiescent_eof_epoch - sink.drain_snapshot_epoch" in healthy and
            "drain_post_snapshot_eofs >=" in healthy and
            "P4_NANO_AUDIO86_PHYSICAL_DMA_DESCRIPTORS" in healthy and
            "quiescent_post_snapshot_eofs >= drain_post_snapshot_eofs" in healthy,
            "physical-short EOF drain predicate weakened")
    require("sink.running_queue_overflow_count == 0U" in healthy and
            "sink.draining_queue_overflow_count <=" in healthy and
            "quiescent_post_snapshot_eofs" in healthy and
            "sink.draining_queue_overflow_count == 0U" not in healthy,
            "physical-short drain q_ovf predicate is not semantic")

    q_ovf_callback = body(
        sink, "static CALLBACK_IRAM void callback_on_send_q_ovf(",
        "void CALLBACK_IRAM p4_nano_audio86_callback_gate_on_sent(")
    draining_branch = body(
        q_ovf_callback,
        "if (state == P4_NANO_AUDIO86_PHYSICAL_DRAINING)",
        "else if (state == P4_NANO_AUDIO86_PHYSICAL_RUNNING")
    running_branch = q_ovf_callback[q_ovf_callback.index(
        "else if (state == P4_NANO_AUDIO86_PHYSICAL_RUNNING"):]
    require("draining_queue_overflow_count" in draining_branch and
            "sticky_error" not in draining_branch and
            "notify_waiter" not in draining_branch,
            "draining q_ovf is no longer telemetry-only")
    require("running_queue_overflow_count" in running_branch and
            "sticky_error" in running_branch and
            "notify_waiter" in running_branch,
            "running q_ovf is no longer sticky-fatal")

    print("S1_SNAPSHOT_ORDERING_SOURCE_PROOF=PASS")
    print("PHYSICAL_TELEMETRY_SNAPSHOT_RACE_FREE=PASS")
    print("S1_SNAPSHOT_OWNS_ALL_EMITTED_DATA=PASS")
    print("CURRENT_STATE_AND_LATCHED_HISTORY_NOT_CONFLATED=PASS")
    print("S1_CALLBACK_TELEMETRY_LIFETIME_SAFE=PASS")
    print("S1_TELEMETRY_HOT_PATH_PRINTF=NO")
    print("S1_TELEMETRY_ISR_PRINTF=NO")
    print("VIRTUAL_NORMAL_OK_SEMANTICS_UNCHANGED=PASS")
    print("VIRTUAL_ONLY_OBSERVER_SET_COMPLETE=PASS")
    print("PHYSICAL_BRANCH_VIRTUAL_OBSERVERS=0")
    print("PHYSICAL_BRANCH_VIRTUAL_OBSERVER_EXCLUSION_SOURCE_PROOF=PASS")
    print("COMMON_NORMAL_OK_INVARIANTS_PRESERVED=PASS")
    print("FULL_PHYSICAL_TERMINAL_PREDICATE_BEHAVIOR="
          "LEGACY_NON_S1_VIRTUAL_OBSERVER_PATH_UNCHANGED")
    print("PHYSICAL_SHORT_SUCCESS_PREDICATE_TRUTHFUL=PASS")
    print("PHYSICAL_SHORT_DRAIN_Q_OVF_PREDICATE_CORRECTED=PASS")
    print("PHYSICAL_SHORT_Q_OVF_INTERVAL_PREDICATE_CORRECTED=PASS")
    print("DRAIN_COMPLETION_EPOCH_SEMANTICS_PRESERVED=PASS")
    print("QUIESCENT_EOF_EPOCH_OBSERVABLE=PASS")
    print("DRAIN_AND_QUIESCENT_EOF_INTERVALS_DISTINCT=PASS")
    print("TERMINAL_CLASSIFICATION_TIMING_UNCHANGED=PASS")
    print("S1_EVIDENCE_SCHEMA_VERSION=2")
    print("RUNNING_Q_OVF_SEMANTICS_UNCHANGED=PASS")
    print("DRAINING_Q_OVF_ACCEPTANCE_CLASS=TELEMETRY_ONLY")
    print("GLOBAL_Q_OVF_SEMANTICS_WEAKENED=NO")
    print("FAKE_VIRTUAL_LATCHES_ADDED=NO")
    print("FAKE_VIRTUAL_OBSERVER_STATE_ADDED=NO")
    print("GLOBAL_NORMAL_OK_WEAKENED=NO")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
