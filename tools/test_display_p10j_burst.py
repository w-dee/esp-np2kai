#!/usr/bin/env python3
"""Host/static contract for the P10J-B PPA burst headroom sweep."""

from __future__ import annotations

import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "firmware/components/p4_nano_live_display/p4_nano_ppa_pie_overlap.cpp"
HEADER = ROOT / "firmware/components/p4_nano_live_display/include/p4_nano_live_display/p4_nano_ppa_pie_overlap.hpp"
LIVE = ROOT / "firmware/components/p4_nano_live_display/p4_nano_live_display.cpp"
MAIN = ROOT / "firmware/main/main.cpp"
LIVE_CMAKE = ROOT / "firmware/components/p4_nano_live_display/CMakeLists.txt"
BUILD = ROOT / "tools/emu/build-production.sh"
TIMING = ROOT / "firmware/components/p4_nano_display/include/p4_nano_display/p4_nano_display_timing_profiles.hpp"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    live = LIVE.read_text(encoding="utf-8")
    main_cpp = MAIN.read_text(encoding="utf-8")
    live_cmake = LIVE_CMAKE.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")
    timing = TIMING.read_text(encoding="utf-8")

    for fragment in (
        "--ppa-pie-burst-benchmark",
        "P4_NANO_PPA_PIE_BURST_BENCHMARK_PROFILE",
        "run_burst_sweep",
        "PPA_DATA_BURST_LENGTH_128",
        "PPA_DATA_BURST_LENGTH_64",
        "PPA_DATA_BURST_LENGTH_32",
        "P4_NANO_PPA_BURST_CONFIG",
        "P4_NANO_PPA_PIE_BURST_PHASE_RESULT",
    ):
        require(fragment in source + header + live + main_cpp + live_cmake + build,
                f"missing P10J integration: {fragment}")

    require("P4_NANO_PPA_PIE_OVERLAP_BENCHMARK_PROFILE" in build,
            "P10I selector must remain available")
    require(source.count("PPA_DATA_BURST_LENGTH_128") >= 2,
            "burst128 must remain the formal baseline and candidate")
    require(source.count("PPA_DATA_BURST_LENGTH_64") == 2 and
            source.count("PPA_DATA_BURST_LENGTH_32") == 2,
            "burst64/32 must be represented only by the candidate and static check")
    require("PPA_DATA_BURST_LENGTH_16" not in source and
            "PPA_DATA_BURST_LENGTH_8" not in source and
            "burst16" not in source.lower() and "burst8" not in source.lower(),
            "initial P10J sweep must not include burst16 or burst8")
    require("std::array<BurstCandidate, 3U>" in source and
            "kBurstCandidates.size() == 3U" in source,
            "exact three public burst candidates are required")

    p10j_function = source[source.index("esp_err_t run_burst_sweep") :]
    burst_phase = source[source.index("bool run_burst_phase") : source.index(
        "const char *burst_pie_class")]
    require("run_sequential_frame" not in p10j_function,
            "P10J must measure overlap only, without a sequential phase")
    require(p10j_function.count("ppa_register_client") == 1 and
            p10j_function.count("ppa_unregister_client") >= 1,
            "each burst phase needs one fresh PPA client lifecycle")
    require("max_pending_trans_num = 1U" in p10j_function and
            "PPA_OPERATION_SRM" in p10j_function,
            "P10J queue depth and SRM operation changed")
    require("context->callback_count.store(0U" in p10j_function and
            "context->callback_failures.store(0U" in p10j_function and
            "xSemaphoreTake(context->done, 0U) == pdTRUE" in p10j_function,
            "phase reset must reject stale completion state")
    require("drain_outstanding_ppa(context)" in p10j_function and
            "if (!cleanup_pass)" in p10j_function and
            "lifetime_must_be_retained.store(true" in p10j_function,
            "unsafe cleanup must retain PPA-visible ownership")

    require("P4_NANO_PPA_PIE_BURST_PHASE burst=%s" in source and
            "P4_NANO_PPA_PIE_BURST_METRIC" in source and
            "PPA_WAIT_FIRST" in source and
            "PPA_WAIT_LATER_AGGREGATE" in source and
            "ppa_wait_exposed_us != metrics.ppa_wait_first_us +" in source,
            "burst phase and first/later wait diagnostics missing")
    for metric in (
        "TOTAL_FRAME_SERVICE", "FIRST_PPA_LATENCY",
        "PPA_COMPLETION_OBSERVED_LATENCY", "PIE_ACTIVE", "CACHE_SYNC",
        "PPA_WAIT_EXPOSED", "PPA_API_CALL_WALL", "PPA_WAIT_FIRST",
        "PPA_WAIT_LATER_AGGREGATE",
    ):
        require(metric in source, f"missing P10J metric: {metric}")
    require("const std::uint64_t start" in burst_phase and
            "sync_destination(destination, &metrics.cache_sync_us)" in burst_phase and
            "metrics.total_us =" in burst_phase,
            "total service timer boundary must include cache sync")

    require("kWarmupSamples + kMeasuredSamples" in burst_phase and
            "expected_callbacks_per_burst=685" in source and
            "total_expected_callbacks=2055" in source,
            "8+128+1 and callback accounting contract missing")
    require("timed_rotated_crc=0" in source and
            "timed_output_crc=0" in source and
            "timed_pixel_validation=0" in source and
            "final_validation_crc=1" in source and
            "final_pixel_validation=1" in source,
            "CRC/pixel validation must be final-only")
    require("&final_metrics.rotated_crc" in burst_phase and
            "validate_frame(source, destination, final_metrics" in burst_phase and
            "expected_frame_matches" in source,
            "final validation must retain CRC and direct pixel equivalence")
    require("P4_NANO_PPA_BURST_EQUIVALENCE" in p10j_function and
            "std::memcmp" in p10j_function,
            "burst outputs must be byte-equivalent to burst128")

    require("display_profile=lower2" in source and
            "tile_width=128" in source and "tile_count=5" in source and
            "buffers=2" in source and "queue_depth=1" in source and
            "bursts=128,64,32" in source and
            "destination_mode=benchmark_psram_destination" in source,
            "LOWER2 geometry/buffer/destination contract missing")
    require("kDisplayTimingLower2" in timing and "240.0F / 7.0F" in timing and
            "29.426767F" in timing and "880U" in timing and "1324U" in timing,
            "LOWER2 timing profile changed or not selected")
    require("P4_NANO_PPA_BURST_COMPARISON" in source and
            "pie_class=" in source and "total_p99_class=" in source and
            "preferred_burst=" in source,
            "comparison and preferred-burst markers missing")
    require("P4_NANO_PPA_PIE_BURST_CPU_HEADROOM" in source and
            "wall_margin_is_not_free_cpu=1" in source and
            "semaphore_blocked_wait_may_be_used_by_runnable_task=1" in source,
            "CPU-headroom interpretation marker missing")
    require("burst_p99_rank" in source and
            "reduction_is_similar" in source and
            "candidate_wait < preferred_wait" in source,
            "preferred-candidate p99/wait tie-break contract missing")

    require("P4_NANO_PPA_PIE_OVERLAP_BENCHMARK_PROFILE=1" in live_cmake and
            "P4_NANO_PPA_PIE_BURST_BENCHMARK_PROFILE=1" in live_cmake,
            "P10I/P10J compile definitions must be independent")
    require("P4_NANO_PPA_PIE_BURST_BENCHMARK_PROFILE" in main_cpp and
            "P4_NANO_PPA_PIE_BURST_BENCHMARK_PROFILE" in live,
            "P10J main/live dispatch guard missing")
    require("P4_NANO_PPA_PIE_BURST_BENCHMARK_PROFILE" in build and
            "P4_NANO_PPA_PIE_OVERLAP_BENCHMARK_PROFILE" in build,
            "P10J build routing must not alter P10I")
    require("GPIO_NUM_20" not in p10j_function and
            "GPIO_NUM_37" not in p10j_function and "GPIO_NUM_38" not in p10j_function,
            "P10J must not add GPIO20 or alter normal UART pins")
    require("xTaskCreate" not in p10j_function and
            "vTaskCoreAffinitySet" not in p10j_function and
            "vTaskPrioritySet" not in p10j_function and "taskYIELD" not in p10j_function,
            "P10J introduced forbidden scheduling changes")
    require("P4_NANO_PPA_PIE_OVERLAP_MEASUREMENT_CONTRACT" in source,
            "existing P10I contract marker disappeared")

    print("Display Performance P10J-B PPA burst headroom sweep host/static contract passed")
    print("P10J_HARDWARE_ACCESS=NOT_PERFORMED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
