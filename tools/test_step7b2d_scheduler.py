#!/usr/bin/env python3
"""Contract checks for the Step 7B.2d scheduler-cooperation slice."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
TASKMNG_HEADER = ROOT / "host/compat/taskmng.h"
ESP_TASKMNG = ROOT / "firmware/components/np2host/taskmng_esp.c"
HEADLESS_TASKMNG = ROOT / "host/backend/headless/taskmng.c"
VIDEO_RUNNER = ROOT / "firmware/components/np2video_runner/np2video_runner.c"
VIDEO_RUNNER_HEADER = ROOT / "firmware/components/np2video_runner/include/np2video_runner/np2video_runner.h"
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
    runner_header = read(VIDEO_RUNNER_HEADER)
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
    pccore_start_index = runner.index("pccore_exec_start_us")
    pccore_wall_index = runner.index("pccore_exec_wall_us", pccore_index)
    producer_cooperate_index = runner.index("np2_host_taskmng_cooperate();")
    if not (pccore_start_index < pccore_index < pccore_wall_index <
            producer_cooperate_index):
        raise AssertionError(
            "pccore_exec wall timing must bracket only pccore_exec(TRUE)"
        )
    if producer_cooperate_index <= pccore_index:
        raise AssertionError("producer cooperation must follow pccore_exec(TRUE)")
    if "#if defined(NP2VIDEO_BENCHMARK_PROFILE)" not in runner:
        raise AssertionError("benchmark profile guard is missing")
    require(runner_header, "bool task_scheduling_override;", "scheduling override gate")
    require(runner_header, "int task_core_id;", "scheduling override core field")
    require(runner_header, "uint32_t task_priority;", "scheduling override priority field")
    require(runner, "if (config->task_scheduling_override)",
            "scheduling override branch")
    require(runner, "config->task_core_id < 0", "core validation")
    require(runner, "config->task_priority >= (uint32_t)configMAX_PRIORITIES",
            "priority validation")
    require(runner, "xTaskCreatePinnedToCore(", "pinned producer creation")
    require(runner, ".task_scheduling_override = false",
            "default scheduling override disabled")
    require(runner, "pccore_exec_count", "pccore timing result fields")

    consume_start = live.index("int benchmark_consume_one(BenchmarkState *state)")
    consume_end = live.index("void benchmark_hold_visible", consume_start)
    consume = live[consume_start:consume_end]
    require(consume, "benchmark_release(state, &token);", "consumer release")
    require(consume, "const std::uint64_t service_us", "consumer timing stop")
    require(consume, "state->transform_samples[measured_index]", "measured sample storage")
    require(live, "constexpr TickType_t kConsumerPollDelayTicks = 1;",
            "explicit one-tick consumer poll delay")
    require(live, "static_assert(kConsumerPollDelayTicks > 0);",
            "non-zero consumer poll-delay guard")
    require(live, "static_assert(pdMS_TO_TICKS(1) == 0",
            "100 Hz one-millisecond conversion guard")
    require(live, "CONFIG_FREERTOS_HZ may be 100",
            "poll-delay tick-rate comment")
    require(live, "pdMS_TO_TICKS(1) is zero",
            "poll-delay conversion warning")
    require(live, "provisional benchmark liveness policy",
            "poll-delay policy comment")
    if "kConsumerPollDelay = pdMS_TO_TICKS(1)" in live:
        raise AssertionError("consumer poll delay must not use pdMS_TO_TICKS(1)")
    if "vTaskDelay(kConsumerPollDelay);" in live:
        raise AssertionError("consumer polling must use the explicit tick delay")
    benchmark_start = live.index("esp_err_t run_benchmark()")
    benchmark_loop_start = live.index("bool failed = false", benchmark_start)
    benchmark_loop_end = live.index(
        "while (benchmark_consume_one(&state) > 0)", benchmark_loop_start)
    benchmark_loop = live[benchmark_loop_start:benchmark_loop_end]
    require(benchmark_loop, "vTaskDelay(kConsumerPollDelayTicks);",
            "non-frame consumer tick cooperation")
    require(benchmark_loop, "else if (consumed == 0)",
            "no-frame consumer polling branch")
    if "pdMS_TO_TICKS(1)" in benchmark_loop:
        raise AssertionError("benchmark polling must not convert one millisecond")
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
    require(live, "task_scheduling_override = true",
            "benchmark scheduling override enabled")
    require(live, "kBenchmarkProducerCore = 1", "benchmark CPU1 policy")
    require(live, "kBenchmarkProducerPriority =",
            "benchmark priority-zero policy")
    require(live, "P4_NANO_BENCHMARK_EXEC_SLICE",
            "pccore wall-time summary")
    require(live, "metric=pccore_exec_wall_us",
            "unambiguous pccore wall-time metric name")
    require(live, "producer_priority_policy=provisional_wdt_safe",
            "provisional scheduler policy metadata")
    require(live, "const std::uint32_t consumer_priority",
            "runtime consumer priority capture")
    require(live, "consumer_priority != 1U", "consumer priority validation")
    normal_start = live.index("esp_err_t run()")
    benchmark_start = live.index("esp_err_t run_benchmark()\n{")
    normal_profile = live[normal_start:benchmark_start]
    require(normal_profile, ".task_scheduling_override = false",
            "normal live profile scheduling default")
    if ".task_scheduling_override = true" in normal_profile:
        raise AssertionError("normal live profile must not enable benchmark override")
    benchmark_profile = live[benchmark_start:]
    require(benchmark_profile, ".task_scheduling_override = true",
            "benchmark scheduling override enabled")
    if "third_party/np2kai/src/pccore.c" in live:
        raise AssertionError("benchmark scheduler slice must not edit third_party")
    require(live, "timing_clock=esp_timer_wall_elapsed", "timing clock metadata")
    require(live, "preemption_may_be_included=1", "preemption metadata")
    require(live, "cooperation_delay_outside_isolated_metrics=1",
            "cooperation interval metadata")

    require(runner, "#define NP2VIDEO_RUNNER_PRIORITY (tskIDLE_PRIORITY + 3)",
            "default producer priority")
    require(runner, "xTaskCreate(np2video_task", "default unpinned producer creation")
    print("Step 7B.2d scheduler-cooperation source contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
