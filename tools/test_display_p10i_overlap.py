#!/usr/bin/env python3
"""Host/static contract for the P10I-B PPA/PIE overlap benchmark."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "firmware/components/p4_nano_live_display/p4_nano_ppa_pie_overlap.cpp"
HEADER = ROOT / "firmware/components/p4_nano_live_display/include/p4_nano_live_display/p4_nano_ppa_pie_overlap.hpp"
SHARED = ROOT / "firmware/components/p4_nano_live_display/p4_nano_exact2x_internal_source.cpp"
SHARED_HEADER = ROOT / "firmware/components/p4_nano_live_display/include/p4_nano_live_display/p4_nano_exact2x_internal_source.hpp"
LIVE = ROOT / "firmware/components/p4_nano_live_display/p4_nano_live_display.cpp"
MAIN = ROOT / "firmware/main/main.cpp"
MAIN_CMAKE = ROOT / "firmware/main/CMakeLists.txt"
LIVE_CMAKE = ROOT / "firmware/components/p4_nano_live_display/CMakeLists.txt"
DISPLAY_CMAKE = ROOT / "firmware/components/p4_nano_display/CMakeLists.txt"
TIMING = ROOT / "firmware/components/p4_nano_display/include/p4_nano_display/p4_nano_display_timing_profiles.hpp"
PIE_ASM = ROOT / "firmware/components/p4_nano_display/p4_nano_display_exact2x_pie.S"
BUILD = ROOT / "tools/emu/build-production.sh"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    shared = SHARED.read_text(encoding="utf-8")
    shared_header = SHARED_HEADER.read_text(encoding="utf-8")
    live = LIVE.read_text(encoding="utf-8")
    main_cpp = MAIN.read_text(encoding="utf-8")
    main_cmake = MAIN_CMAKE.read_text(encoding="utf-8")
    live_cmake = LIVE_CMAKE.read_text(encoding="utf-8")
    display_cmake = DISPLAY_CMAKE.read_text(encoding="utf-8")
    timing = TIMING.read_text(encoding="utf-8")
    pie_asm = PIE_ASM.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")

    for fragment in (
        "--ppa-pie-overlap-benchmark",
        "P4_NANO_PPA_PIE_OVERLAP_BENCHMARK_PROFILE",
        "p4_nano_ppa_pie_overlap.cpp",
        "PPA_TRANS_MODE_NON_BLOCKING",
        "max_pending_trans_num = 1U",
        "P4_NANO_PPA_PIE_OVERLAP_VSYNC_VALID",
        "P4_NANO_PPA_PIE_OVERLAP_LIFECYCLE_RESULT",
    ):
        require(fragment in source + live + main_cpp + main_cmake + live_cmake +
                display_cmake + build, f"missing P10I integration: {fragment}")

    require("PPA_TRANS_MODE_BLOCKING" in source,
            "sequential control must use blocking PPA")
    require(source.count("PPA_TRANS_MODE_NON_BLOCKING") >= 2,
            "candidate submit sites must use non-blocking mode")
    require("run_sequential_frame" in source and "run_overlap_frame" in source,
            "same-binary control/candidate phases missing")
    require("control_snapshot" in source and
            "destination_mode=benchmark_psram_destination" in source,
            "benchmark-owned destination contract missing")
    require("xSemaphoreCreateBinaryStatic" in source and
            "xSemaphoreGiveFromISR" in source and
            "xSemaphoreTake(context->done, kPpaWaitTicks)" in source,
            "bounded ISR completion wait contract missing")
    require("xTaskCreate" not in source and "vTaskCoreAffinitySet" not in source and
            "vTaskPrioritySet" not in source and "taskYIELD" not in source,
            "P10I introduced forbidden scheduling changes")
    require("ESP_CACHE_MSYNC_FLAG_DIR_M2C" in source and
            "ESP_CACHE_MSYNC_FLAG_DIR_C2M |" in source,
            "cache direction contract missing")
    require("exact2x_pie_tile128_aligned" in source and
            "exact2x::make_tile_operation" in source and
            "exact2x::expected_frame_matches" in source,
            "shared P10F geometry/PIE/correctness helpers not reused")
    for fragment in (
        "kTileBytes == 102400U",
        "kTileDestinationBytes == 409600U",
        "kTileCount == 5U",
        "kDestinationBytes == 2048000U",
        "kRequiredAlignmentBytes == 64U",
        "source_x =",
        "tile_index + 1U) * 128U",
    ):
        require(fragment in source or fragment in shared,
                f"P10I geometry contract missing: {fragment}")
    require(source.count("MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT") == 2,
            "P10I must allocate exactly two internal tile buffers")
    require("kDisplayTimingLower2" in timing and "240.0F / 7.0F" in timing and
            "29.426767F" in timing and "880U" in timing and "1324U" in timing,
            "LOWER2 timing profile changed or not bound")
    require("q0" in pie_asm and "q1" in pie_asm and "q3" not in pie_asm,
            "P10I must reuse the established q0/q1 PIE assembly")
    for crc in ("0x8dadbf82U", "0x379511d7U", "0xc8a10b55U"):
        require(crc in shared_header, f"accepted CRC changed: {crc}")
    require("make_tile_operation" in shared and
            "PPA_TRANS_MODE_NON_BLOCKING" not in shared,
            "P10F blocking source contract was changed")
    require("kWarmupSamples + kMeasuredSamples" in source and
            "final_validation" in source,
            "P10I sample/final-validation envelope missing")
    require("benchmark_display_refresh_profile=lower2" in build and
            "P4_NANO_BENCHMARK_DISPLAY_REFRESH_PROFILE" in build,
            "P10I must bind LOWER2 through the selector")
    require("P4_NANO_PPA_PIE_OVERLAP_BENCHMARK_PROFILE=1" in live_cmake,
            "P10I compile definition missing")
    require("P4_NANO_PPA_PIE_OVERLAP_BENCHMARK_PROFILE_ACTIVE" in main_cmake,
            "P10I CMake profile gate missing")
    require("P4_NANO_PPA_PIE_OVERLAP_BENCHMARK_PROFILE" in display_cmake,
            "P10I display timing component gate missing")
    require("P4_NANO_PPA_PIE_OVERLAP_BENCHMARK_PROFILE" in main_cpp,
            "P10I main dispatch guard missing")

    metric_print = source[source.index("void print_metric"):source.index(
        "struct CompletionContext")]
    require("std::sort(stats.samples.begin(), stats.samples.begin() + stats.stored)"
            in metric_print,
            "metric samples must be sorted before reporting")
    require(metric_print.index("std::sort") < metric_print.index("std::printf"),
            "metric sort must precede printf evaluation")
    percentile_body = source[source.index("percentile_from_sorted"):source.index(
        "void print_metric")]
    require("const MetricStats &stats" in percentile_body and
            "std::sort" not in percentile_body,
            "percentile extraction must not mutate samples")
    for fragment in ("const std::uint64_t minimum", "const std::uint64_t p50",
                     "const std::uint64_t p95", "const std::uint64_t p99",
                     "const std::uint64_t maximum", "const bool ordered",
                     "PPA_COMPLETION_OBSERVED_LATENCY", "PPA_API_CALL_WALL"):
        require(fragment in source, f"deterministic metric contract missing: {fragment}")
    require("PPA_SUBMIT_OVERHEAD" not in source and
            "PPA_LATENCY_AGGREGATE" not in source,
            "blocking API wall time must not be labeled as async submit/latency")
    for fragment in ("in_flight", "fetch_add(1U", "fetch_sub(1U",
                     "drain_outstanding_ppa", "kCleanupWaitAttempts",
                     "P4_NANO_PPA_PIE_OVERLAP_CLEANUP=", "FAIL_IN_FLIGHT",
                     "FAIL_UNREGISTER", "transaction_lifetime_must_be_retained",
                     "deliberately retain the client"):
        require(fragment in source + live,
                f"async cleanup contract missing: {fragment}")
    require("if (!cleanup_pass)" in source and
            "else {\n        heap_caps_free(destination);" in source,
            "PPA buffers must only be freed after cleanup passes")
    require("while (" not in source,
            "P10I failure recovery must not use a spin loop")
    require("PPA_TRANS_MODE_BLOCKING" in source and
            "callback_count" in source and "PPA_API_CALL_WALL" in source,
            "sequential blocking callback contract changed")
    require("P4_NANO_PPA_PIE_OVERLAP_CPU_HEADROOM" in source and
            "wall_margin_is_not_free_cpu=1" in source,
            "CPU headroom semantics marker missing")
    require("TOTAL_FRAME_SERVICE" in source and
            "kWarmupSamples + kMeasuredSamples" in source and
            "kFinalValidationSamples" in source,
            "primary timing envelope changed")

    print("Display Performance P10I-B1 PPA/PIE overlap hardening host/static contract passed")
    print("P10I_HARDWARE_ACCESS=NOT_PERFORMED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
