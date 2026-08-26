#!/usr/bin/env python3
"""Host contract, lane model, and golden test for the P10 exact-2x A/B path."""

from __future__ import annotations

import argparse
import pathlib
import re
import struct
import zlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "firmware/components/p4_nano_display/include/p4_nano_display/p4_nano_display_exact2x.hpp"
SOURCE = ROOT / "firmware/components/p4_nano_display/p4_nano_display_exact2x.cpp"
PIE_SOURCE = ROOT / "firmware/components/p4_nano_display/p4_nano_display_exact2x_pie.S"
LIVE = ROOT / "firmware/components/p4_nano_live_display/p4_nano_live_display.cpp"
DISPLAY = ROOT / "firmware/components/p4_nano_display/p4_nano_display.cpp"
DISPLAY_CMAKE = ROOT / "firmware/components/p4_nano_display/CMakeLists.txt"
MAIN = ROOT / "firmware/main/main.cpp"
BUILD = ROOT / "tools/emu/build-production.sh"
GOLDEN = ROOT / "tests/guest/np2video-live/golden.json"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def rgb565_from_bmp(path: pathlib.Path) -> bytes:
    data = path.read_bytes()
    require(data[:2] == b"BM" and len(data) == 768054,
            f"unexpected retained BMP: {path}")
    pixels = bytearray()
    row_bytes = ((640 * 3 + 3) // 4) * 4
    for by in range(399, -1, -1):
        row = data[54 + by * row_bytes:54 + (by + 1) * row_bytes]
        for x in range(640):
            b, g, r = row[x * 3:x * 3 + 3]
            pixels += struct.pack("<H", ((r >> 3) << 11) |
                                  ((g >> 2) << 5) | (b >> 3))
    return bytes(pixels)


def rotate_ccw(source: bytes) -> bytes:
    output = bytearray(400 * 640 * 2)
    for sy in range(400):
        for sx in range(640):
            dx, dy = sy, 639 - sx
            source_offset = (sy * 640 + sx) * 2
            destination_offset = (dy * 400 + dx) * 2
            output[destination_offset:destination_offset + 2] = \
                source[source_offset:source_offset + 2]
    return bytes(output)


def exact2x(source: bytes) -> bytes:
    output = bytearray(800 * 1280 * 2)
    for y in range(640):
        for x in range(400):
            pixel = source[(y * 400 + x) * 2:(y * 400 + x + 1) * 2]
            for oy in (0, 1):
                for ox in (0, 1):
                    offset = ((2 * y + oy) * 800 + 2 * x + ox) * 2
                    output[offset:offset + 2] = pixel
    return bytes(output)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture-bmp", type=pathlib.Path)
    args = parser.parse_args()
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    pie = PIE_SOURCE.read_text(encoding="utf-8")
    legacy_start = pie.index("exact2x_pie_aligned:")
    legacy_end = pie.index(".size exact2x_pie_aligned", legacy_start)
    legacy_pie = pie[legacy_start:legacy_end]
    live = LIVE.read_text(encoding="utf-8")
    display = DISPLAY.read_text(encoding="utf-8")
    display_cmake = DISPLAY_CMAKE.read_text(encoding="utf-8")
    main = MAIN.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")
    golden = GOLDEN.read_text(encoding="utf-8")
    for fragment in ("kExact2xSourceWidth = 400U",
                     "kExact2xSourceHeight = 640U",
                     "kExact2xDestinationWidth = 800U",
                     "kExact2xDestinationHeight = 1280U",
                     "kExact2xGroupsPerSourceRow",
                     "kExact2xSourceBytes",
                     "kExact2xDestinationBytes",
                     "kExact2xM2CAlignmentBytes = 64U",
                     "kExact2xExpectedDestinationCrc = 0xc8a10b55U"):
        require(fragment in header, f"missing geometry/CRC contract: {fragment}")
    for fragment in ("const std::uint32_t packed",
                     "destination_pairs0[x] = packed",
                     "destination_pairs1[x] = packed"):
        require(fragment in source, f"missing scalar packed store: {fragment}")
    require("P4_NANO_EXACT2X_SCALER_BENCHMARK_PROFILE" in live,
            "missing isolated P10 selector")
    require("exact2x_pie_available() noexcept { return true; }" in header,
            "PIE availability must reflect the linked P10 A/B helper")
    require("exact2x_pie_aligned" in header,
            "missing PIE helper declaration")
    require("p4_nano_display_exact2x_pie.S" in display_cmake,
            "P10 profile must link the PIE helper")
    for fragment in ("kExact2xWarmupSamples = 8U",
                     "kExact2xMeasuredSamples = 128U",
                     "kExact2xFinalValidationSamples = 1U"):
        require(fragment in header, f"P10 sample-count contract changed: {fragment}")
    metric_start = live.index("void exact2x_print_metric")
    metric_end = live.index("bool exact2x_full_match", metric_start)
    metric = live[metric_start:metric_end]
    require("std::sort(samples.begin(), samples.begin() + count)" in metric,
            "metric sorting must use static sample storage")
    if "std::array<std::uint64_t, N> sorted" in metric or \
            "std::array<std::uint64_t, 128>" in metric:
        raise AssertionError("P10 metric reporting must not copy 128 samples on the stack")
    phase_start = live.index("bool exact2x_run_phase")
    phase_end = live.index("bool exact2x_run_samples", phase_start)
    phase_function = live[phase_start:phase_end]
    samples_start = live.index("bool exact2x_run_samples")
    samples_end = live.index("esp_err_t run_exact2x_benchmark_after_start", samples_start)
    samples_function = live[samples_start:samples_end]
    require("constexpr std::size_t kExact2xCooperateInterval = 64U;" in live,
            "P10 cooperation interval must be a fixed 64-iteration constant")
    require("constexpr TickType_t kExact2xCooperateDelayTicks = 1;" in live,
            "P10 cooperation delay must be exactly one tick")
    require("P4_NANO_EXACT2X_COOPERATE interval=%zu delay_ticks=%u" in
            samples_function,
            "P10 cooperation configuration must be reported once")
    loop_start = phase_function.index("for (std::size_t index = 0U;")
    loop_end = phase_function.index(
        "    // Keep this required one-call validation outside the measured window.",
        loop_start)
    loop = phase_function[loop_start:loop_end]
    delay_index = loop.index("vTaskDelay(kExact2xCooperateDelayTicks);")
    require("completed_iterations % kExact2xCooperateInterval == 0U" in loop,
            "P10 cooperation must occur every 64 completed iterations")
    require("exact2x_add_sample" in loop[:delay_index],
            "P10 delay must follow measured sample storage")
    for fragment in ("kernel_start", "kernel_us", "cache_start", "cache_us"):
        require(fragment in loop[:delay_index],
                f"P10 delay must follow {fragment} timing")
    require("vTaskDelay(0)" not in loop and "pdMS_TO_TICKS(1)" not in loop,
            "P10 cooperation must use one explicit tick, not zero/ms conversion")
    require(loop.find("vTaskDelay") == delay_index,
            "P10 loop must have exactly one cooperative delay point")
    for twdt_api in ("esp_task_wdt_init", "esp_task_wdt_reconfigure",
                     "esp_task_wdt_add", "esp_task_wdt_delete",
                     "esp_task_wdt_reset"):
        require(twdt_api not in samples_function,
                f"P10 must not change TWDT policy: {twdt_api}")
    normalize_start = live.index("bool exact2x_normalize")
    normalize_end = live.index("bool exact2x_scalar_kernel", normalize_start)
    normalize = live[normalize_start:normalize_end]
    require(normalize.count("ESP_CACHE_MSYNC_FLAG_DIR_M2C") == 2,
            "P10 must normalize both source and destination with M2C")
    require("ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED" not in normalize,
            "P10 M2C normalization must not use UNALIGNED")
    require("kExact2xM2CAlignmentBytes" in samples_function,
            "P10 layout must enforce the M2C alignment contract")
    require("source_m2c_alignment_ok" in samples_function and
            "destination_m2c_alignment_ok" in samples_function,
            "P10 must validate source and destination M2C alignment")
    for fragment in (
            "reinterpret_cast<std::uintptr_t>(source) %\n"
            "                p4_nano_display::kExact2xM2CAlignmentBytes == 0U",
            "reinterpret_cast<std::uintptr_t>(destination) %\n"
            "                p4_nano_display::kExact2xM2CAlignmentBytes == 0U",
            "p4_nano_display::kExact2xSourceBytes %\n"
            "                p4_nano_display::kExact2xM2CAlignmentBytes == 0U",
            "p4_nano_display::kExact2xDestinationBytes %\n"
            "                p4_nano_display::kExact2xM2CAlignmentBytes == 0U"):
        require(fragment in samples_function,
                f"P10 M2C alignment contract missing: {fragment}")
    require("m2c_alignment=%s" in samples_function,
            "P10 layout reporting must expose M2C alignment")
    require("ESP_CACHE_MSYNC_FLAG_DIR_C2M" in phase_function and
            "ESP_CACHE_MSYNC_FLAG_UNALIGNED" in phase_function,
            "P10 initial source C2M path must remain unchanged")
    require("display_session_sync_framebuffer(&state->display)" in phase_function,
            "P10 framebuffer visibility sync must remain in the benchmark")
    require("ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED" in
            display,
            "P10 framebuffer post-scalar C2M path must remain unchanged")
    require("stats->kernel.fill(0U)" in live,
            "P10 stats reset must use static storage")
    if "Exact2xStats{}" in live:
        raise AssertionError("P10 stats reset must not materialize an aggregate stack temporary")
    for phase in ("SCALAR", "PIE"):
        require(f'"{phase}"' in samples_function,
                f"missing same-binary {phase} phase")
    for marker in ("P4_NANO_EXACT2X_%s_%s",
                   "P4_NANO_EXACT2X_%s_CORRECTNESS",
                   "P4_NANO_EXACT2X_%s_RESULT"):
        require(marker in live, f"missing phase-specific marker: {marker}")
    require("scalar_result && pie_result" in samples_function,
            "overall P10 result must require both phases")
    require("P4_NANO_EXACT2X_PIE_LAYOUT" in samples_function,
            "missing PIE layout report")
    require("pie_layout_ok" in samples_function,
            "PIE invocation must be gated by its layout contract")
    require('exact2x_report_invalid_phase(&exact2x_pie_stats, "PIE")' in
            samples_function,
            "PIE layout failure must report an invalid phase without invocation")
    require("pie_available=1" in live,
            "P10 mode report must describe the implemented PIE helper")
    for twdt_api in ("esp_task_wdt_init", "esp_task_wdt_reconfigure",
                     "esp_task_wdt_add", "esp_task_wdt_delete",
                     "esp_task_wdt_reset"):
        require(twdt_api not in phase_function,
                f"P10 must not change TWDT policy: {twdt_api}")

    require("li      a5, 640" in legacy_pie, "PIE helper must iterate 640 source rows")
    require("li      a4, 50" in legacy_pie, "PIE helper must use 50 groups per row")
    for instruction in ("esp.vld.128.ip  q0, a0, 16",
                        "esp.orq         q1, q0, q0",
                        "esp.vzip.16     q0, q1",
                        "esp.vst.128.ip  q0, a2, 16",
                        "esp.vst.128.ip  q1, a2, 16",
                        "esp.vst.128.ip  q0, a3, 16",
                        "esp.vst.128.ip  q1, a3, 16"):
        require(instruction in legacy_pie, f"PIE inner-loop instruction missing: {instruction}")
    require("esp.vzip.16     q0, q0" not in legacy_pie,
            "PIE helper must use distinct q0/q1 VZIP operands")
    require(not re.search(r"\\bq[2-7]\\b", legacy_pie),
            "PIE helper must use q0/q1 only")
    require(re.search(r"(?m)^\s*mv\s+a1,\s*a3\s*$", legacy_pie),
            "PIE row progression must use the post-inner-loop a3 pair base")
    require(not re.search(r"(?m)^\s*addi\s+a1,\s*a3,\s*1600\s*$", legacy_pie),
            "PIE row progression must not add an extra destination-row stride")
    require(".size exact2x_pie_aligned" in pie and "ret" in legacy_pie,
            "PIE helper must have normal function metadata and return")
    lanes = list("ABCDEFGH")
    zipped_low = [lanes[0], lanes[0], lanes[1], lanes[1],
                  lanes[2], lanes[2], lanes[3], lanes[3]]
    zipped_high = [lanes[4], lanes[4], lanes[5], lanes[5],
                   lanes[6], lanes[6], lanes[7], lanes[7]]
    require(zipped_low + zipped_high ==
            list("AABBCCDDEEFFGGHH"), "PIE host lane model mismatch")
    # Model the destination row progression explicitly across the complete
    # 640-row source.  The inner loop advances each destination pointer by
    # one 16-byte store per VST and a3 therefore already names the next pair
    # base at the row boundary.
    source_rows = 640
    destination_rows = 1280
    destination_row_stride = 1600
    destination_bytes = 2048000
    row_pairs = []
    max_written_offset = -1
    for source_row in range(source_rows):
        first_row = 2 * source_row
        second_row = first_row + 1
        require(second_row < destination_rows,
                f"row progression exceeds destination at source row {source_row}")
        row_pairs.append((first_row, second_row))
        max_written_offset = max(
            max_written_offset,
            second_row * destination_row_stride + destination_row_stride - 1)
    require(row_pairs[0] == (0, 1), "row 0 must write destination rows 0,1")
    require(row_pairs[1] == (2, 3), "row 1 must write destination rows 2,3")
    require(row_pairs[639] == (1278, 1279),
            "row 639 must write destination rows 1278,1279")
    require(max_written_offset == destination_bytes - 1,
            "row model must end at the final framebuffer byte")
    require(max_written_offset < destination_bytes,
            "row model must stay strictly inside the framebuffer")
    route_start = main.index(
        "constexpr uart_port_t kDisplayBenchmarkApplicationConsoleUart")
    route_guard_start = main.rfind(
        "#if defined(P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE)", 0, route_start)
    route_guard_end = main.index("#endif", route_guard_start)
    route_guard = main[route_guard_start:route_guard_end]
    require("P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE" in route_guard,
            "P9 route guard missing")
    if "P4_NANO_EXACT2X_SCALER_BENCHMARK_PROFILE" in route_guard:
        raise AssertionError("P10 must not trigger the GPIO20 route")
    app_start = main.index('extern "C" void app_main(void)')
    hello = main.index("ESP-NP2KAI HELLO WORLD OK", app_start)
    if "uart_set_pin" in main[app_start:hello]:
        raise AssertionError("P10 app_main must not reroute UART before HELLO")
    require("--exact2x-scaler-benchmark" in build,
            "missing P10 build selector")
    require('"crc32": "0x8dadbf82"' in golden,
            "P9 source golden changed")

    fixture = args.fixture_bmp
    if fixture is None:
        for candidate in (
            ROOT / "host/build/runner/np2video-live-reference.bmp",
            ROOT / "host/build/phase2/runner/np2video-live-reference.bmp",
        ):
            if candidate.exists():
                fixture = candidate
                break
    require(fixture is not None, "retained source BMP is required for P10 host test")
    original = rgb565_from_bmp(fixture)
    require(zlib.crc32(original) & 0xffffffff == 0x8dadbf82,
            "retained source CRC mismatch")
    rotated = rotate_ccw(original)
    require(zlib.crc32(rotated) & 0xffffffff == 0x379511d7,
            "independent P9 CCW90 derivation mismatch")
    expected = exact2x(rotated)
    expected_crc = zlib.crc32(expected) & 0xffffffff
    require(expected_crc == 0xc8a10b55,
            f"derived P10 expected CRC changed: 0x{expected_crc:08x}")
    # Verify the packed scalar contract independently, including both rows
    # and both pixels represented by each 32-bit pair.
    model = bytearray(len(expected))
    for y in range(640):
        for x in range(400):
            pixel = rotated[(y * 400 + x) * 2:(y * 400 + x + 1) * 2]
            pair = pixel + pixel
            for row in (2 * y, 2 * y + 1):
                offset = (row * 800 + 2 * x) * 2
                model[offset:offset + 4] = pair
    require(bytes(model) == expected, "scalar packed model mismatch")
    print("Display Performance P10D-B exact2x host contract and PIE lane model passed")
    print(f"P10_EXACT2X_ROW_ADDRESS_MODEL max_offset={max_written_offset} "
          f"framebuffer_bytes={destination_bytes}")
    print(f"P10_EXACT2X_SOURCE_CRC=0x{zlib.crc32(rotated) & 0xffffffff:08x}")
    print(f"P10_EXACT2X_EXPECTED_CRC=0x{expected_crc:08x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
