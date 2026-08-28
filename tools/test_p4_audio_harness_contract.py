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
