#!/usr/bin/env python3
"""Contract checks for the isolated Step 7B.2d transform benchmark."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN_CMAKE = ROOT / "firmware/main/CMakeLists.txt"
LIVE_CMAKE = ROOT / "firmware/components/p4_nano_live_display/CMakeLists.txt"
RUNNER_CMAKE = ROOT / "firmware/components/np2video_runner/CMakeLists.txt"
RUNNER_HEADER = ROOT / "firmware/components/np2video_runner/include/np2video_runner/np2video_runner.h"
RUNNER = ROOT / "firmware/components/np2video_runner/np2video_runner.c"
LIVE = ROOT / "firmware/components/p4_nano_live_display/p4_nano_live_display.cpp"
BUILD = ROOT / "tools/emu/build-production.sh"


def read(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


def require(text: str, fragment: str, description: str) -> None:
    if fragment not in text:
        raise AssertionError(f"missing {description}: {fragment}")


def main() -> int:
    main_cmake = read(MAIN_CMAKE)
    live_cmake = read(LIVE_CMAKE)
    runner_cmake = read(RUNNER_CMAKE)
    runner_header = read(RUNNER_HEADER)
    runner = read(RUNNER)
    live = read(LIVE)
    build = read(BUILD)

    require(main_cmake, "P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE_ACTIVE",
            "isolated profile selector")
    require(main_cmake, "live display and isolated transform benchmark profiles are mutually exclusive",
            "mutual exclusion")
    require(main_cmake, "isolated transform benchmark requires NP2_I286C_INLINE_MEM_FASTPATH=0",
            "FASTPATH=0 guard")
    require(live_cmake, "P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE=1",
            "isolated live component definition")
    require(runner_cmake, "NP2VIDEO_BENCHMARK_PROFILE=1",
            "isolated benchmark runner loop definition")
    require(runner_cmake, "P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE=1",
            "isolated runner component definition")
    require(build, "--live-display-transform-isolated-benchmark",
            "isolated build flag")
    require(build, "requires --i286-inline-mem-fastpath 0",
            "isolated FASTPATH command guard")

    require(runner_header, "np2video_runner_cooperate_fn",
            "producer cooperation callback")
    require(runner_header, "np2video_runner_pause_at_cooperate_fn",
            "producer pause callback")
    require(runner, "state->cooperate(cooperate_calls",
            "cooperation observation")
    require(runner, "state->pause_at_cooperate(cooperate_calls",
            "safe pause callback")
    pccore = runner.index("pccore_exec(TRUE);")
    cooperate = runner.index("np2_host_taskmng_cooperate();", pccore)
    pause = runner.index("state->pause_at_cooperate(cooperate_calls", cooperate)
    if not pccore < cooperate < pause:
        raise AssertionError("pause must follow pccore and scheduler cooperation")
    if "while (state->pause_requested" in runner:
        raise AssertionError("producer pause must not busy-spin")

    require(live, "P4_NANO_TRANSFORM_ISOLATED_CONFIG",
            "isolated configuration output")
    require(live, "producer_pause_policy=post_pccore_block",
            "post-pccore pause metadata")
    for marker in (
        "PRODUCER_PAUSE_REQUESTED",
        "PRODUCER_PAUSE_ACK",
        "ISOLATED_MEASUREMENT_BEGIN",
        "ISOLATED_MEASUREMENT_END",
        "PRODUCER_RESUMED",
    ):
        require(live, marker, f"isolated lifecycle marker {marker}")
    require(live, "isolated_source_token",
            "retained immutable source token")
    require(live, "state->isolated_source_crc_after = source_crc",
            "final source CRC capture")
    require(live, "benchmark_release(state, &state->isolated_source_token)",
            "source release after measurement")
    require(live, "isolated_transform_samples",
            "separate isolated transform samples")
    require(live, "isolated_cache_samples",
            "separate isolated cache samples")
    require(live, "isolated_service_samples",
            "separate isolated service samples")
    require(live, "static_cast<unsigned>(kBenchmarkWarmupTransforms)",
            "isolated warm-up output")
    require(live, "static_cast<unsigned>(kBenchmarkFinalValidationTransforms)",
            "isolated final validation output")
    require(live, "np2_host_taskmng_cooperate();",
            "isolated scheduler cooperation")
    require(live, "xSemaphoreTake(state->isolated_pause_resume, portMAX_DELAY)",
            "blocking producer pause")
    require(live, "fastpath=0",
            "isolated FASTPATH metadata")
    require(live, "P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE",
            "isolated compile guard")
    require(live, "constexpr std::uint32_t kBenchmarkWarmupTransforms = 8U",
            "exact warm-up count")
    require(live, "constexpr std::uint32_t kBenchmarkMeasuredTransforms = 128U",
            "exact measured count")
    require(live, "constexpr std::uint32_t kBenchmarkFinalValidationTransforms = 1U",
            "exact final-validation count")
    isolated_run = live.index("esp_err_t run_isolated_benchmark_after_start")
    hold = live.index("benchmark_hold_isolated_source(state)", isolated_run)
    pause_request = live.index("benchmark_request_isolated_pause(state)", hold)
    samples = live.index("benchmark_run_isolated_samples(state)", pause_request)
    release = live.index(
        "benchmark_release(state, &state->isolated_source_token)", samples
    )
    if not hold < pause_request < samples < release:
        raise AssertionError("source lease must span pause and all isolated samples")
    resumed = live.index("PRODUCER_RESUMED", samples)
    cleanup = live.index("display_session_cleanup(&state->display)", release)
    if not samples < resumed < release < cleanup:
        raise AssertionError("producer must resume before source release and cleanup")
    if resumed > cleanup:
        raise AssertionError("producer must resume before display cleanup")
    transform_start = live.index("const std::uint64_t transform_start", samples)
    transform_stop = live.index("const bool transformed", transform_start)
    transform_interval = live[transform_start:transform_stop]
    if any(marker in transform_interval for marker in ("crc32", "printf", "sort")):
        raise AssertionError("transform interval contains non-timing work")
    cache_start = live.index("const std::uint64_t cache_start", transform_stop)
    cache_stop = live.index("const esp_err_t sync_result", cache_start)
    cache_interval = live[cache_start:cache_stop]
    if any(marker in cache_interval for marker in ("crc32", "printf", "sort")):
        raise AssertionError("cache-sync interval contains non-timing work")
    if ".cooperate = nullptr" not in live or ".pause_at_cooperate = nullptr" not in live:
        raise AssertionError("existing live runner path must keep isolated callbacks disabled")
    if "pdMS_TO_TICKS(1)" in live[live.index("run_isolated_benchmark_after_start"):]:
        raise AssertionError("isolated benchmark must not use zero-tick 1 ms delay")

    print("Step 7B.2d isolated-transform source contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
