#!/usr/bin/env python3
"""Static scheduling contracts for the FreeRTOS audio benchmark harness."""

from __future__ import annotations

import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[1]
BENCHMARK = (ROOT / "firmware/components/p4_nano_audio_benchmark/"
             "p4_nano_audio_benchmark.cpp").read_text(encoding="utf-8")
STREAM = (ROOT / "firmware/components/np2opngen_fixture/"
          "np2opngen_e1b_stream.c").read_text(encoding="utf-8")
HEADER = (ROOT / "firmware/components/np2opngen_fixture/include/"
          "np2opngen_e1b_stream.h").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    require("std::atomic<bool> failed" in BENCHMARK,
            "failure flag must be atomic")
    require("std::atomic<FailureStage> failure_stage" in BENCHMARK,
            "failure stage must be atomic")
    for stage in (
            "Preflight", "TimingAlloc", "AtomicGate", "WorkerInit",
            "DoneCreate", "WorkerCreate", "ProducerCreate", "DoneTimeout",
            "ProducerFail", "WorkerFailed", "ObserverInvariant",
            "FinishIdentity", "PrintTiming"):
        require(f"FailureStage::{stage}" in BENCHMARK,
                f"failure stage {stage} is missing")
    require(BENCHMARK.count("P4_AUDIO_FAILURE") == 1,
            "failure output must have one structured prefix")
    require(BENCHMARK.count("P4_AUDIO_IDENTITY_DIAG") == 1,
            "identity diagnostic must have one structured prefix")
    finish = re.search(
        r"static bool finish_identity\(.*?\nstatic void print_hex",
        BENCHMARK, re.DOTALL)
    require(finish is not None, "finish_identity helper is missing")
    finish_body = finish.group(0)
    require(finish_body.count("np2opngen_e1b_worker_event_trace_finish") == 1 and
            finish_body.count("np2opngen_synth_event_trace_finish") == 1,
            "each event trace must be finalized exactly once")
    require(finish_body.count("np2_sha256_final(&ctx->sink.sha") == 1 and
            "if (ctx->correctness) np2_sha256_final(&ctx->sink.sha" in
            finish_body,
            "PCM SHA must finalize once on correctness runs")
    require("if (!identity_match && ctx->correctness)" in finish_body,
            "identity diagnostic must be failure-only and correctness-only")
    for name in (
            "worker_trace_finish", "producer_count", "consumer_count",
            "consumer_crc", "consumer_sha", "producer_trace_finish",
            "trace_count_equal", "trace_crc_equal", "trace_sha_equal",
            "sequence", "pcm_frames", "pcm_bytes", "pcm_crc", "pcm_sha",
            "producer_loop", "none"):
        require(f'"{name}"' in finish_body,
                f"first_failure name {name} is missing")
    for field in (
            "consumer_count_match", "consumer_crc_match",
            "producer_count_match", "trace_count_equal", "trace_crc_equal",
            "consumer_sha_match", "trace_sha_equal", "sequence_match",
            "pcm_frames_match", "pcm_bytes_match", "pcm_crc_match",
            "pcm_sha_match", "producer_loop_valid", "identity_match=0",
            "consumer_event_sha256=", "producer_event_sha256=",
            "pcm_sha256="):
        require(field in finish_body,
                f"identity diagnostic field {field} is missing")
    diagnostic_section = re.search(
        r"if \(!identity_match && ctx->correctness\) \{(?P<body>.*?)\n    \}",
        finish_body, re.DOTALL)
    require(diagnostic_section is not None,
            "identity diagnostic gate body is missing")
    require("np2_sha256_final" not in diagnostic_section.group("body"),
            "diagnostic printing must not finalize SHA state")
    require("if (!identity_match) return false;" in finish_body,
            "identity failure must preserve failure result")
    diagnostic = re.search(
        r"static void print_failure_record\(.*?\n\}", BENCHMARK, re.DOTALL)
    require(diagnostic is not None, "failure diagnostic helper is missing")
    diagnostic_body = diagnostic.group(0)
    require("np2opngen_synth_event_trace_finish" not in diagnostic_body and
            "np2_sha256_final" not in diagnostic_body,
            "failure diagnostic must not finalize trace or SHA state")
    require("if (failed) print_failure_record" in BENCHMARK,
            "successful runs must not print failure diagnostics")
    require("const bool quiescent = worker_done_observed && producer_done" in
            diagnostic_body,
            "quiescent snapshot must require both completion edges")
    require("atomic_load_explicit(\n        &ctx->control.producer_done, std::memory_order_acquire)" in
            diagnostic_body,
            "producer completion must be acquire-observed")
    quiescent = re.search(
        r"if \(quiescent\) \{(?P<safe>.*?)\n    \} else \{(?P<unsafe>.*?)\n    \}",
        diagnostic_body, re.DOTALL)
    require(quiescent is not None, "quiescent diagnostic split is missing")
    require("ctx->worker" in quiescent.group("safe"),
            "quiescent diagnostic must expose worker state")
    require("ctx->worker" not in quiescent.group("unsafe") and
            "ctx->sink" not in quiescent.group("unsafe") and
            "ctx->timing" not in quiescent.group("unsafe"),
            "unquiesced diagnostic must not inspect mutable runtime state")
    require("taskYIELD" not in BENCHMARK,
            "queue-full producer path must not yield-spin")
    require("producer_waiting.store(true, std::memory_order_release)" in BENCHMARK,
            "producer wait state publication is missing")
    require("ulTaskNotifyTake(pdTRUE, portMAX_DELAY)" in BENCHMARK,
            "queue-full path must block on a counting notification")
    require("np2opngen_spsc_occupancy(&ctx->queue)" in BENCHMARK,
            "wait path must recheck occupancy for lost-wakeup safety")
    require("ctx->producer_waiting.load(std::memory_order_acquire)" in BENCHMARK
            and "xTaskNotifyGive(ctx->producer_task)" in BENCHMARK,
            "successful dequeue must wake a waiting producer")
    require("observer_quantum_complete" in BENCHMARK
            and "kHousekeepingQuantumInterval = 64U" in BENCHMARK
            and "vTaskDelay(kHousekeepingDelayTicks)" in BENCHMARK,
            "fixed housekeeping cadence is missing")
    require("quantum_complete" in HEADER,
            "observer quantum-complete boundary is missing")
    require("worker->cursor % NP2_OPNGEN_E1B_RENDER_QUANTUM == 0U" in STREAM,
            "housekeeping callback must be limited to complete logical quanta")
    render_end = re.search(
        r"observer->render_end\(worker->observer->context,\s*"
        r"frame_offset,\s*\(uint32_t\)frame_count, 0\);(?P<body>.*?)"
        r"observer->quantum_complete\(", STREAM, re.DOTALL)
    require(render_end is not None,
            "housekeeping callback must follow successful render_end")
    require("worker->observer->render_end" not in render_end.group("body"),
            "unexpected timing callback between render_end and housekeeping")
    print("P4_AUDIO_HARNESS_CONTRACT=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
