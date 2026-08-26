#!/usr/bin/env python3
"""Host/static contract for the P10F-B same-binary source A/B benchmark."""

from __future__ import annotations

import pathlib
import struct
import zlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
ASM = ROOT / "firmware/components/p4_nano_display/p4_nano_display_exact2x_pie.S"
HEADER = ROOT / "firmware/components/p4_nano_display/include/p4_nano_display/p4_nano_display_exact2x.hpp"
LIVE = ROOT / "firmware/components/p4_nano_live_display/p4_nano_live_display.cpp"
SOURCE = ROOT / "firmware/components/p4_nano_live_display/p4_nano_exact2x_internal_source.cpp"
INTERNAL_HEADER = ROOT / "firmware/components/p4_nano_live_display/include/p4_nano_live_display/p4_nano_exact2x_internal_source.hpp"
LIVE_CMAKE = ROOT / "firmware/components/p4_nano_live_display/CMakeLists.txt"
DISPLAY_CMAKE = ROOT / "firmware/components/p4_nano_display/CMakeLists.txt"
MAIN = ROOT / "firmware/main/main.cpp"
MAIN_CMAKE = ROOT / "firmware/main/CMakeLists.txt"
BUILD = ROOT / "tools/emu/build-production.sh"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def rgb565_from_bmp(path: pathlib.Path) -> bytes:
    data = path.read_bytes()
    require(data[:2] == b"BM" and len(data) == 768054,
            f"unexpected retained BMP: {path}")
    row_bytes = ((640 * 3 + 3) // 4) * 4
    pixels = bytearray()
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
    asm = ASM.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    live = LIVE.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    internal_header = INTERNAL_HEADER.read_text(encoding="utf-8")
    live_cmake = LIVE_CMAKE.read_text(encoding="utf-8")
    display_cmake = DISPLAY_CMAKE.read_text(encoding="utf-8")
    main = MAIN.read_text(encoding="utf-8")
    main_cmake = MAIN_CMAKE.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")

    p10f_start = live.index(
        "esp_err_t run_exact2x_internal_source_benchmark_after_start")
    p10f_end = live.index(
        "#elif defined(P4_NANO_PSRAM_BANDWIDTH_BENCHMARK_PROFILE)",
        p10f_start)
    p10f_lifecycle = live[p10f_start:p10f_end]
    vsync_guard = live[live.index(
        "#if defined(P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE) || \\\n    defined(P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_PROFILE)"):live.index(
        "bool benchmark_vsync_valid(")]

    for fragment in (
            "P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_PROFILE",
            "--exact2x-internal-source-benchmark",
            "p4_nano_exact2x_internal_source.cpp",
            "esp_driver_ppa",
            "P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_PROFILE=1"):
        require(fragment in (source + live + live_cmake + display_cmake +
                             main_cmake + build),
                f"missing P10F profile integration: {fragment}")

    require("P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_PROFILE" in main,
            "P10F app dispatch/profile guard missing")
    require("P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_PROFILE" in
            vsync_guard,
            "P10F profile missing from benchmark_vsync_valid guard")
    for fragment, name in (
            ("benchmark_vsync_valid(state->display)",
             "P10F VSYNC validity call"),
            ("!vsync_valid", "P10F VSYNC failure gate"),
            ("benchmark_print_vsync_stats(state->display)",
             "P10F generic VSYNC report"),
            ("P4_NANO_EXACT2X_INTERNAL_SOURCE_VSYNC_VALID",
             "P10F VSYNC marker"),
            ("P4_NANO_EXACT2X_INTERNAL_SOURCE_LIFECYCLE_RESULT",
             "P10F lifecycle marker"),
            ("const bool scheduling_contract", "P10F scheduling contract"),
            ("esp_err_t internal_source_result", "P10F module result"),
            ("state->publish_failed.load(", "P10F publish failure"),
            ("const bool pause_stable", "P10F pause stability"),
            ("state->backlight_off_failed =",
             "P10F backlight result"),
            ("state->producer_pause_acknowledged.load(",
             "P10F cleared pause acknowledgement"),
            ("!state->isolated_resumed", "P10F resumed-state check"),
            ("state->releases != state->acquisitions",
             "P10F acquisition/release balance"),
            ("state->producer_result.status != ESP_OK",
             "P10F producer result"),
            ("benchmark_hold_visible(state)", "P10F visible hold")):
        require(fragment in p10f_lifecycle, f"missing {name}")
    require("P4_NANO_PPA_ROTATION_VSYNC_VALID" in live and
            "P4_NANO_PPA_ROTATION_LIFECYCLE_RESULT" in live and
            "P4_NANO_PPA_INTERNAL_TILE_VSYNC_VALID" in live and
            "P4_NANO_PPA_INTERNAL_TILE_LIFECYCLE_RESULT" in live,
            "existing P9/P10E lifecycle markers changed or disappeared")
    for forbidden in ("GPIO_NUM_20", "vsync_front_porch", "refresh_rate"):
        require(forbidden not in p10f_lifecycle,
                f"P10F lifecycle changed forbidden display setting: {forbidden}")
    route_guard_start = main.index(
        "#if defined(P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE)")
    route_guard_end = main.index("#endif", route_guard_start)
    require("P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_PROFILE" not in
            main[route_guard_start:route_guard_end],
            "P10F must retain the normal UART0 route")

    require("exact2x_pie_aligned" in asm and "li      a5, 640" in asm,
            "accepted full-frame helper changed or disappeared")
    require("exact2x_pie_tile128_aligned" in asm and "li      a5, 128" in asm,
            "T128 PIE helper missing")
    tile_start = asm.index("exact2x_pie_tile128_aligned:")
    tile_end = asm.index(".size exact2x_pie_tile128_aligned", tile_start)
    tile = asm[tile_start:tile_end]
    for instruction in (
            "esp.vld.128.ip  q0, a0, 16",
            "esp.orq         q1, q0, q0",
            "esp.vzip.16     q0, q1",
            "esp.vst.128.ip  q0, a2, 16",
            "esp.vst.128.ip  q1, a2, 16",
            "esp.vst.128.ip  q0, a3, 16",
            "esp.vst.128.ip  q1, a3, 16",
            "mv      a1, a3"):
        require(instruction in tile, f"T128 instruction missing: {instruction}")
    require("li      a4, 50" in tile, "T128 group count changed")
    require("q2" not in tile and "q3" not in tile and "q4" not in tile,
            "T128 helper uses an unexpected vector register")
    require("\n    sp" not in tile and " sp," not in tile,
            "T128 helper must not use the stack")
    require("exact2x_pie_tile128_aligned" in header,
            "T128 public ABI missing")

    offsets = [i * 409600 for i in range(5)]
    require(offsets == [0, 409600, 819200, 1228800, 1638400],
            "destination tile offsets changed")
    ranges = [(offset, offset + 409600 - 1) for offset in offsets]
    require(ranges[0] == (0, 409599) and ranges[-1] == (1638400, 2047999),
            "destination range endpoints changed")
    require(all(offset % 16 == 0 and offset % 64 == 0 for offset in offsets),
            "destination tile alignment contract changed")
    require(all(ranges[i][1] + 1 == ranges[i + 1][0]
                for i in range(4)), "destination tiles have a gap or overlap")
    require(source.count("exact2x_pie_tile128_aligned") >= 1,
            "same T128 helper is not used by both phase plumbing paths")
    require("ppa_do_scale_rotate_mirror" in source and
            "PPA preparation" not in source[source.index("run_pie_frame"):],
            "candidate PIE path lost its explicit PPA operation")
    require("ESP_CACHE_MSYNC_FLAG_DIR_M2C" in source and
            "ESP_CACHE_MSYNC_FLAG_DIR_C2M |" in source,
            "common cache contract missing")
    require("P4_NANO_EXACT2X_INTERNAL_TIMER_CONTROL" in source,
            "timer-bracket diagnostic missing")
    require("kWarmupSamples = 8U" in source + internal_header and
            "kMeasuredSamples = 128U" in source + internal_header and
            "kFinalValidationSamples = 1U" in source + internal_header,
            "P10F sample contract missing")
    for forbidden in ("xTaskCreate", "PPA_TRANS_MODE_NON_BLOCKING",
                      "P4_NANO_OVERLAP", "esp_task_wdt_",
                      "GPIO_NUM_20"):
        require(forbidden not in source,
                f"P10F contains forbidden production/overlap feature: {forbidden}")

    fixture = ROOT / "host/build/phase2/runner/np2video-live-reference.bmp"
    require(fixture.exists(), "retained source BMP is required")
    original = rgb565_from_bmp(fixture)
    rotated = rotate_ccw(original)
    expected = exact2x(rotated)
    require(zlib.crc32(original) & 0xffffffff == 0x8dadbf82,
            "original source CRC mismatch")
    require(zlib.crc32(rotated) & 0xffffffff == 0x379511d7,
            "rotated source CRC mismatch")
    require(zlib.crc32(expected) & 0xffffffff == 0xc8a10b55,
            "destination CRC mismatch")

    print("Display Performance P10F-B same-binary internal-source host/static contract passed")
    print("P10F_DESTINATION_TILE_RANGES=0..409599,409600..819199,"
          "819200..1228799,1228800..1638399,1638400..2047999")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
