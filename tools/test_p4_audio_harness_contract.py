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
