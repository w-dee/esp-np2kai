#!/usr/bin/env python3
"""Host/static contract for the P10K-D0 isolated grouped-store A/B."""

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "firmware/components/p4_nano_live_display/p4_nano_exact2x_grouped_store.cpp"
HEADER = ROOT / "firmware/components/p4_nano_live_display/include/p4_nano_live_display/p4_nano_exact2x_grouped_store.hpp"
SHARED_HEADER = ROOT / "firmware/components/p4_nano_live_display/include/p4_nano_live_display/p4_nano_exact2x_internal_source.hpp"
SHARED = ROOT / "firmware/components/p4_nano_live_display/p4_nano_exact2x_internal_source.cpp"
LIVE = ROOT / "firmware/components/p4_nano_live_display/p4_nano_live_display.cpp"
MAIN = ROOT / "firmware/main/main.cpp"
MAIN_CMAKE = ROOT / "firmware/main/CMakeLists.txt"
LIVE_CMAKE = ROOT / "firmware/components/p4_nano_live_display/CMakeLists.txt"
DISPLAY_CMAKE = ROOT / "firmware/components/p4_nano_display/CMakeLists.txt"
BUILD = ROOT / "tools/emu/build-production.sh"
ASM = ROOT / "firmware/components/p4_nano_display/p4_nano_display_exact2x_pie.S"
PIE_HEADER = ROOT / "firmware/components/p4_nano_display/include/p4_nano_display/p4_nano_display_exact2x.hpp"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", type=pathlib.Path,
                        help="optional D0 ELF for helper-symbol validation")
    args = parser.parse_args()

    source = SOURCE.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    shared_header = SHARED_HEADER.read_text(encoding="utf-8")
    shared = SHARED.read_text(encoding="utf-8")
    live = LIVE.read_text(encoding="utf-8")
    main_cpp = MAIN.read_text(encoding="utf-8")
    main_cmake = MAIN_CMAKE.read_text(encoding="utf-8")
    live_cmake = LIVE_CMAKE.read_text(encoding="utf-8")
    display_cmake = DISPLAY_CMAKE.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")
    assembly = ASM.read_text(encoding="utf-8")
    pie_header = PIE_HEADER.read_text(encoding="utf-8")

    profile = "P4_NANO_EXACT2X_GROUPED_STORE_BENCHMARK_PROFILE"
    integration = source + header + live + main_cpp + main_cmake + live_cmake + display_cmake + build
    for fragment in (
        profile,
        "--exact2x-grouped-store-benchmark",
        "p4_nano_exact2x_grouped_store.cpp",
        "P4_NANO_EXACT2X_GROUPED_STORE_BENCHMARK_VARIANT",
        "P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE",
    ):
        require(fragment in integration, f"missing D0 routing: {fragment}")
    require("benchmark_display_refresh_profile=lower2" in build,
            "D0 selector must force LOWER2 timing")
    require("P4_NANO_EXACT2X_GROUPED_STORE_BENCHMARK_PROFILE_ACTIVE" in main_cmake,
            "D0 CMake active selector missing")
    require("P4_NANO_EXACT2X_GROUPED_STORE_BENCHMARK_PROFILE=1" in live_cmake and
            "P4_NANO_EXACT2X_GROUPED_STORE_BENCHMARK_PROFILE=1" in main_cmake,
            "D0 compile definitions missing")
    require("P4_NANO_EXACT2X_GROUPED_STORE_BENCHMARK_PROFILE" in main_cpp,
            "D0 main dispatch guard missing")

    for fragment in (
        "kTileBytes == 102400U", "kTileDestinationBytes == 409600U",
        "kTileCount == 5U", "kDestinationBytes == 2048000U",
        "kRequiredAlignmentBytes == 64U", "kWarmupSamples == 8U",
        "kMeasuredSamples == 128U", "kFinalValidationSamples == 1U",
        "PPA_DATA_BURST_LENGTH_128", "max_pending_trans_num = 1U",
        "PPA_TRANS_MODE_BLOCKING", "destination_mode=separate_psram",
        "tile_memory=internal", "internal_buffers=2",
        "control_helper=current", "candidate_helper=grouped64",
        "source=internal", "scanout=active", "service=kernel_plus_cache_sync",
        "P10K-D0 CONTROLLED VARIABLE = PIE STORE GROUPING ONLY",
    ):
        require(fragment in source, f"D0 contract missing: {fragment}")
    require(source.count("MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT") == 2,
            "D0 must allocate exactly two internal tile buffers")
    retained = "std::array<PhaseStats, 2U> s_phase_stats{};"
    require(retained in source and
            source.index(retained) < source.index(
                "namespace p4_nano_exact2x_grouped_store"),
            "D1B must retain exactly two benchmark-private PhaseStats slots")
    require(not re.search(r"\bPhaseStats\s+(?:current|grouped)\s*\{\}", source),
            "PhaseStats current/grouped must not be automatic objects")
    run_start = source.index("esp_err_t run(const Input &input)")
    run_end = source.index("} // namespace p4_nano_exact2x_grouped_store", run_start)
    run_body = source[run_start:run_end]
    require("PhaseStats &current = s_phase_stats[0];" in run_body and
            "PhaseStats &grouped = s_phase_stats[1];" in run_body,
            "current/grouped references must map to separate retained slots")
    require(run_body.count("s_phase_stats[") == 2,
            "D1B must retain exactly two PhaseStats slots")
    require("reset_stats(&current);" in run_body and
            "reset_stats(&grouped);" in run_body,
            "both retained PhaseStats slots must be explicitly reset")
    allocate_start = source.index("bool allocate(")
    allocate_end = source.index("const char *kernel_class", allocate_start)
    require("PhaseStats" not in source[allocate_start:allocate_end],
            "PhaseStats must not use heap allocation")
    require(source.count("exact2x_pie_tile128_aligned") >= 1 and
            source.count("exact2x_pie_tile128_grouped64_aligned") >= 1,
            "both current and grouped helpers must be reachable")
    grouped_start = assembly.index("exact2x_pie_tile128_grouped64_aligned:")
    grouped_end = assembly.index(".size exact2x_pie_tile128_grouped64_aligned",
                                 grouped_start)
    require("exact2x_pie_tile128_grouped64_aligned" in pie_header and
            "q3" not in assembly[grouped_start:grouped_end],
            "grouped helper ABI/q3 contract changed")

    frame_start = source.index("bool run_pie_frame")
    frame_end = source.index("bool run_phase", frame_start)
    frame = source[frame_start:frame_end]
    require(frame.index("prepare_internal_tile") < frame.index("const std::uint64_t start"),
            "PPA preparation must remain outside PIE timer")
    require(frame.count("esp_timer_get_time()") == 2,
            "each tile helper must have one start/end timer pair")
    require("if (grouped)" in frame and
            "exact2x_pie_tile128_aligned" in frame and
            "exact2x_pie_tile128_grouped64_aligned" in frame,
            "current/grouped phase helper selection missing")

    loop_start = source.index("for (std::size_t index = 0U;",
                              source.index("bool run_phase"))
    loop_end = source.index("std::uint32_t final_rotated_crc", loop_start)
    timed_loop = source[loop_start:loop_end]
    require("nullptr))" in timed_loop and "kernel_us + cache_us" in timed_loop,
            "timed path must collect metrics without validation work")
    require("crc32" not in timed_loop and "memcmp" not in timed_loop,
            "CRC/memcmp must remain final-only")
    require("vTaskDelay(kSchedulerHealthDelayTicks)" in timed_loop and
            "(index + 1U) % 64U == 0U" in timed_loop,
            "P10F scheduler-health cadence was not retained")
    final_start = source.index("std::uint32_t final_rotated_crc")
    final_body = source[final_start:source.index("void print_timer_control", final_start)]
    require("crc32" in final_body and "expected_frame_matches" in final_body,
            "final-only CRC/pixel validation missing")

    for marker in (
        "timed_rotated_crc=0", "timed_output_crc=0", "timed_pixel_validation=0",
        "timed_memcmp=0", "final_validation_crc=1", "final_pixel_validation=1",
        "final_control_candidate_memcmp=1", "calls_per_frame=10",
        "P4_NANO_EXACT2X_GROUPED_EQUIVALENCE", "byte_exact=%d",
        "P4_NANO_EXACT2X_GROUPED_COMPARISON", "p95_delta_us=",
        "p99_delta_us=", "kernel_reduction_percent=", "service_guard=",
        "integrated_gate=", "direction=",
    ):
        require(marker in source, f"D0 output marker missing: {marker}")
    require(source.index("helper=current") < source.index("helper=grouped64"),
            "current phase must precede grouped phase")
    require(source.index("std::memcpy(allocations.control_snapshot") <
            source.index("std::memcmp(allocations.control_snapshot"),
            "control snapshot/equivalence ordering changed")

    for forbidden in (
        "xTaskCreate", "xTaskCreatePinnedToCore", "taskYIELD",
        "PPA_TRANS_MODE_NON_BLOCKING", "PPA_DATA_BURST_LENGTH_64",
        "PPA_DATA_BURST_LENGTH_32", "exact2x_pie_preemption_clobber",
        "GPIO_NUM_20", "esp_timer_start_once",
    ):
        require(forbidden not in source, f"D0 contains forbidden experiment change: {forbidden}")
    require("p4_nano_exact2x_internal_source.cpp" in live_cmake and
            "p4_nano_exact2x_grouped_store.cpp" in live_cmake,
            "D0 shared source linkage missing")
    require("P4_NANO_EXACT2X_GROUPED_STORE_BENCHMARK_PROFILE" not in
            (SHARED.read_text(encoding="utf-8") +
             (ROOT / "firmware/components/p4_nano_live_display/p4_nano_ppa_pie_overlap.cpp").read_text(encoding="utf-8")),
            "existing P10F/P10I implementation was contaminated")
    require("P4_NANO_EXACT2X_GROUPED_STORE_BENCHMARK_PROFILE" in build and
            "P4_NANO_EXACT2X_GROUPED_STORE_BENCHMARK_PROFILE" in display_cmake,
            "D0 build/display isolation missing")
    for crc in ("0x8dadbf82U", "0x379511d7U", "0xc8a10b55U"):
        require(crc in shared_header, f"accepted correctness CRC changed: {crc}")

    if args.elf is not None:
        require(args.elf.is_file(), f"ELF does not exist: {args.elf}")
        objdump = shutil.which("riscv32-esp-elf-objdump")
        if objdump is None:
            candidates = sorted((pathlib.Path.home() / ".espressif" / "tools").glob(
                "riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-objdump"))
            objdump = str(candidates[-1]) if candidates else None
        require(objdump is not None, "riscv32-esp-elf-objdump is required for --elf")
        result = subprocess.run([objdump, "-t", str(args.elf)], check=True,
                                capture_output=True, text=True)
        symbols = result.stdout
        for symbol in ("exact2x_pie_tile128_aligned",
                       "exact2x_pie_tile128_grouped64_aligned"):
            require(symbol in symbols, f"D0 ELF is missing {symbol}")
        disassembly = subprocess.run([objdump, "-d", "-C", str(args.elf)],
                                     check=True, capture_output=True,
                                     text=True).stdout
        lines = disassembly.splitlines()
        run_symbol = "<p4_nano_exact2x_grouped_store::run("
        try:
            run_index = next(index for index, line in enumerate(lines)
                             if run_symbol in line and line.rstrip().endswith(">:"))
        except StopIteration as exc:
            raise AssertionError("D1B ELF is missing grouped-store run symbol") from exc
        prologue = lines[run_index + 1:run_index + 36]
        frame_size = sum(int(match) for line in prologue
                         for match in re.findall(r"addi\s+sp,sp,-(\d+)", line))
        require(frame_size <= 1024,
                f"D1B run static frame exceeds 1024 bytes: {frame_size}")
        print(f"P10K_D1B_RUN_STATIC_FRAME_BYTES={frame_size}")

    print("Display Performance P10K-D0 grouped-store host/static contract passed")
    print("P10K_D0_HARDWARE_ACCESS=NOT_PERFORMED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
