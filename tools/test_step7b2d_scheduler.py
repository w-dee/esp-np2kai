#!/usr/bin/env python3
"""Contract checks for the Step 7B.2d scheduler-cooperation slice."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
TASKMNG_HEADER = ROOT / "host/compat/taskmng.h"
ESP_TASKMNG = ROOT / "firmware/components/np2host/taskmng_esp.c"
HEADLESS_TASKMNG = ROOT / "host/backend/headless/taskmng.c"
VIDEO_RUNNER = ROOT / "firmware/components/np2video_runner/np2video_runner.c"
LIVE_DISPLAY = ROOT / "firmware/components/p4_nano_live_display/p4_nano_live_display.cpp"


def read(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


def require(text: str, fragment: str, description: str) -> None:
    if fragment not in text:
        raise AssertionError(f"missing {description}: {fragment}")


def main() -> int:
    header = read(TASKMNG_HEADER)
    esp = read(ESP_TASKMNG)
    headless = read(HEADLESS_TASKMNG)
    runner = read(VIDEO_RUNNER)
    live = read(LIVE_DISPLAY)

    require(header, "void np2_host_taskmng_cooperate(void);", "API declaration")
    require(esp, "void np2_host_taskmng_cooperate(void)", "ESP API")
    require(esp, "vTaskDelay(1);", "one-tick ESP cooperation")
    require(headless, "void np2_host_taskmng_cooperate(void)", "headless API")
    require(headless, "Native/headless hosts already have an operating-system scheduler.",
            "headless no-op documentation")
    if "vTaskDelay" in headless or "sleep(" in headless:
        raise AssertionError("headless cooperation must remain a no-op")

    if runner.count("np2_host_taskmng_cooperate();") != 1:
        raise AssertionError("producer cooperation must have exactly one benchmark call")
    pccore_index = runner.index("pccore_exec(TRUE);")
    producer_cooperate_index = runner.index("np2_host_taskmng_cooperate();")
    if producer_cooperate_index <= pccore_index:
        raise AssertionError("producer cooperation must follow pccore_exec(TRUE)")
    if "#if defined(NP2VIDEO_BENCHMARK_PROFILE)" not in runner:
        raise AssertionError("benchmark profile guard is missing")

    consume_start = live.index("int benchmark_consume_one(BenchmarkState *state)")
    consume_end = live.index("void benchmark_hold_visible", consume_start)
    consume = live[consume_start:consume_end]
    require(consume, "benchmark_release(state, &token);", "consumer release")
    require(consume, "const std::uint64_t service_us", "consumer timing stop")
    require(consume, "state->transform_samples[measured_index]", "measured sample storage")
    if consume.count("std::printf(") != 4:
        raise AssertionError("consumer path must retain only four one-time markers")
    consumer_cooperate_index = consume.index("np2_host_taskmng_cooperate();")
    if consumer_cooperate_index < consume.index("const std::uint64_t service_us"):
        raise AssertionError("consumer cooperation entered service timing")
    if consumer_cooperate_index < consume.index("state->transform_samples[measured_index]"):
        raise AssertionError("consumer cooperation entered measured sample bookkeeping")
    release_index = consume.rfind("benchmark_release(state, &token);")
    if release_index < 0 or consumer_cooperate_index <= release_index:
        raise AssertionError("consumer cooperation must follow frame release")
    if consume.count("np2_host_taskmng_cooperate();") != 1:
        raise AssertionError("consumer cooperation must have exactly one call")

    for marker in (
        "P4_NANO_BENCHMARK_FIRST_ACQUIRE=1",
        "P4_NANO_BENCHMARK_FIRST_TRANSFORM_COMPLETE=1",
        "P4_NANO_BENCHMARK_FIRST_CACHE_SYNC_COMPLETE=1",
        "P4_NANO_BENCHMARK_FIRST_VISIBLE=1",
    ):
        if live.count(marker) != 1:
            raise AssertionError(f"expected one-time liveness marker: {marker}")
    require(live, "producer_cooperate_calls=", "producer counter output")
    require(live, "consumer_cooperate_calls=", "consumer counter output")
    require(live, "timing_clock=esp_timer_wall_elapsed", "timing clock metadata")
    require(live, "preemption_may_be_included=1", "preemption metadata")
    require(live, "cooperation_delay_outside_isolated_metrics=1",
            "cooperation interval metadata")

    require(runner, "#define NP2VIDEO_RUNNER_PRIORITY (tskIDLE_PRIORITY + 3)",
            "producer priority")
    require(runner, "xTaskCreate(np2video_task", "unpinned producer creation")
    print("Step 7B.2d scheduler-cooperation source contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
