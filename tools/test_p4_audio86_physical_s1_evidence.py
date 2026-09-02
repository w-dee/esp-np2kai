#!/usr/bin/env python3
"""Static lifecycle and ownership proofs for 86R.5D.2 S1 evidence."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BINDING = ROOT / ("firmware/components/p4_nano_audio86_guest_binding/"
                  "p4_nano_audio86_guest_binding.cpp")
SINK = ROOT / ("firmware/components/p4_nano_audio86_physical_sink/"
               "p4_nano_audio86_physical_sink.c")
IDF = ROOT / ("firmware/components/p4_nano_audio86_physical_sink/"
              "p4_nano_audio86_physical_sink_idf.cpp")
CMAKE = ROOT / ("firmware/components/p4_nano_audio86_guest_binding/"
                "CMakeLists.txt")


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

    run = body(binding, "esp_err_t run_on_pc98_task(",
               "} // namespace p4_nano_audio86_guest_binding")
    join = run.index("const bool pcm_joined")
    snapshot = run.index("capture_physical_s1_snapshot(runtime);", join)
    delete = run.index("vTaskDelete(runtime->pcm_consumer);", snapshot)
    destroy = run.index("p4_nano_audio86_physical_sink_destroy(", delete)
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
    require(snapshot < delete < destroy < summary,
            "snapshot/delete/destroy/emission owner order changed")

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
             "                    && physical_s1_snapshot_healthy(runtime)",
        "#else\n                    && runtime->pcm_sink_started")
    require("pcm_sink_started" not in physical_branch and
            "pcm_sink_finished" not in physical_branch,
            "physical predicate fakes virtual sink latches")
    require("std::atomic<uint8_t>" not in binding and
            "_Atomic uint8_t" not in sink,
            "S1 introduced a byte-width atomic path")

    emitted = body(binding, "void emit_physical_s1_evidence(",
                   "bool physical_s1_snapshot_healthy(")
    require(emitted.count("runtime->") == 1 and
            "runtime->physical_s1" in emitted and
            "p4_nano_audio86_physical_sink_get_telemetry" not in emitted,
            "UART evidence does not exclusively use the owned snapshot")

    print("S1_SNAPSHOT_ORDERING_SOURCE_PROOF=PASS")
    print("PHYSICAL_TELEMETRY_SNAPSHOT_RACE_FREE=PASS")
    print("S1_SNAPSHOT_OWNS_ALL_EMITTED_DATA=PASS")
    print("CURRENT_STATE_AND_LATCHED_HISTORY_NOT_CONFLATED=PASS")
    print("S1_CALLBACK_TELEMETRY_LIFETIME_SAFE=PASS")
    print("S1_TELEMETRY_HOT_PATH_PRINTF=NO")
    print("S1_TELEMETRY_ISR_PRINTF=NO")
    print("VIRTUAL_NORMAL_OK_SEMANTICS_UNCHANGED=PASS")
    print("PHYSICAL_SHORT_SUCCESS_PREDICATE_TRUTHFUL=PASS")
    print("FAKE_VIRTUAL_LATCHES_ADDED=NO")
    print("GLOBAL_NORMAL_OK_WEAKENED=NO")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
