#!/usr/bin/env python3
"""Static contracts for the A2.4a final-window and evidence hardening slice."""

from __future__ import annotations

import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[1]
BENCHMARK = (ROOT / "firmware/components/p4_nano_audio_benchmark/"
             "p4_nano_audio_benchmark.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "firmware/components/p4_nano_audio_benchmark/CMakeLists.txt"
         ).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    consumer = re.search(
        r"static void pcm_consumer_task\(.*?\nstatic void producer_fail",
        BENCHMARK, re.DOTALL)
    require(consumer is not None, "consumer task is missing")
    final_tick = re.search(
        r"if \(ctx->consumer\.processed_ticks == ctx->expected_ticks\) \{"
        r"(?P<body>.*?)\n\s*break;\n\s*\}",
        consumer.group(0), re.DOTALL)
    require(final_tick is not None, "final-tick predicate is missing")
    final_body = final_tick.group("body")
    require("ctx->consumer.frames == ctx->workload->expected.end_frame" in final_body,
            "final-tick frame predicate is missing")
    require("np2opngen_pcm_ring_occupancy(ctx->pcm_ring) == 0U" in final_body,
            "final-tick occupancy predicate is missing")
    completion_if = re.search(r"if \(!final_frames \|\| !final_empty\) \{",
                              final_body)
    require(completion_if is not None and "|| !ctx->pcm_producer_done" not in
            completion_if.group(0),
            "consumer final-tick predicate must not require producer_done")

    identity = re.search(
        r"const bool identity_match =(?P<body>.*?);\n\n    const char",
        BENCHMARK, re.DOTALL)
    require(identity is not None, "global identity predicate is missing")
    require("ctx->pcm_producer_done.load(std::memory_order_acquire)" in
            identity.group("body"),
            "global identity must still require producer_done")
    require("ctx->consumer_done_flag.load(std::memory_order_acquire)" in
            identity.group("body"),
            "global identity must still require consumer completion")

    seam = re.search(
        r"#if P4_NANO_AUDIO_FINAL_WINDOW_TEST_CASE == 1(?P<body>.*?)"
        r"#endif\n\s*sink->owner->pcm_producer_done\.store",
        BENCHMARK, re.DOTALL)
    require(seam is not None, "final-publication test seam is missing")
    require("final_publication_window_active.store" in seam.group("body") and
            "vTaskDelay(pdMS_TO_TICKS(25))" in seam.group("body"),
            "final-publication seam must create a scheduler-visible delay")
    require("final_publication_window_ack" in seam.group("body") and
            "xSemaphoreTake" in seam.group("body"),
            "final-publication seam must wait for consumer observation")
    require("ctx.final_publication_window_ack = xSemaphoreCreateBinary()" in BENCHMARK,
            "final-publication acknowledgement semaphore is not created")
    require("xSemaphoreGive(ctx->final_publication_window_ack)" in BENCHMARK,
            "final-publication acknowledgement is not published")
    require("P4_NANO_AUDIO_FINAL_WINDOW_TEST_CASE" in CMAKE,
            "final-window case must be a CMake test-only definition")

    require("consumer_pacing=ESP_TIMER_5MS" in BENCHMARK,
            "physical pacing label must identify esp_timer")
    for record in (
            "P4_AUDIO_PACING", "P4_AUDIO_PACING_CALLBACK",
            "P4_AUDIO_PACING_WAKE", "P4_AUDIO_PACING_CONSUMER",
            "P4_AUDIO_COMPUTE_SERVICE", "P4_AUDIO_PCM_FINISH_WAIT",
            "P4_AUDIO_TIMER_LIFECYCLE"):
        require(record in BENCHMARK, f"missing record {record}")
    require("workload=%s mode=%s" in BENCHMARK,
            "new records must identify workload and mode")
    require("timing_valid=%s" in BENCHMARK,
            "timing validity field is missing")
    require("compute_underflow_count" in BENCHMARK,
            "compute underflow field is missing")
    require("first_compute_underflow_raw_us" in BENCHMARK and
            "first_compute_underflow_wait_us" in BENCHMARK,
            "underflow diagnostics are incomplete")
    require("record_failure(ctx, FailureStage::PacingTimingInvariant)" in BENCHMARK,
            "timing underflow must map to PacingTimingInvariant")
    require("Do not expose a wrapping subtraction as timing data." in BENCHMARK,
            "underflow path must reject wrapping subtraction")

    require("constexpr int kConsumerCore = 0;" in BENCHMARK and
            "constexpr UBaseType_t kConsumerPriority = tskIDLE_PRIORITY + 5;" in
            BENCHMARK,
            "consumer topology constants changed")
    require("ESP_TIMER_TASK" in BENCHMARK,
            "pacing timer must remain ESP_TIMER_TASK dispatched")
    print("P4_AUDIO_A24A_STATIC_CONTRACT=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
