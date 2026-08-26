#!/usr/bin/env python3
"""Host geometry and source/profile contracts for P10E-B."""

from __future__ import annotations

import pathlib
import struct
import zlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "tools/emu/build-production.sh"
MAIN = ROOT / "firmware/main/main.cpp"
MAIN_CMAKE = ROOT / "firmware/main/CMakeLists.txt"
LIVE = ROOT / "firmware/components/p4_nano_live_display/p4_nano_live_display.cpp"
LIVE_CMAKE = ROOT / "firmware/components/p4_nano_live_display/CMakeLists.txt"
HEADER = ROOT / "firmware/components/p4_nano_live_display/include/p4_nano_live_display/p4_nano_ppa_internal_tile.hpp"
SOURCE = ROOT / "firmware/components/p4_nano_live_display/p4_nano_ppa_internal_tile.cpp"
GOLDEN = ROOT / "tests/guest/np2video-live/golden.json"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def direct_ccw(source: bytes) -> bytes:
    output = bytearray(400 * 640 * 2)
    for sy in range(400):
        for sx in range(640):
            dx, dy = sy, 639 - sx
            output[(dy * 400 + dx) * 2:(dy * 400 + dx + 1) * 2] = \
                source[(sy * 640 + sx) * 2:(sy * 640 + sx + 1) * 2]
    return bytes(output)


def tiled_ccw(source: bytes, tile_width: int) -> tuple[bytes, list[int]]:
    output = bytearray(400 * 640 * 2)
    source_xes: list[int] = []
    tile_count = 640 // tile_width
    for tile_index in range(tile_count):
        source_x = 640 - (tile_index + 1) * tile_width
        source_xes.append(source_x)
        tile = bytearray(400 * tile_width * 2)
        for sy in range(400):
            for local_sx in range(tile_width):
                local_dx = sy
                local_dy = tile_width - 1 - local_sx
                source_offset = (sy * 640 + source_x + local_sx) * 2
                tile[(local_dy * 400 + local_dx) * 2:
                     (local_dy * 400 + local_dx + 1) * 2] = \
                    source[source_offset:source_offset + 2]
        destination_offset = tile_index * tile_width * 400 * 2
        output[destination_offset:destination_offset + len(tile)] = tile
    return bytes(output), source_xes


def main() -> int:
    build = BUILD.read_text(encoding="utf-8")
    main = MAIN.read_text(encoding="utf-8")
    main_cmake = MAIN_CMAKE.read_text(encoding="utf-8")
    live = LIVE.read_text(encoding="utf-8")
    live_cmake = LIVE_CMAKE.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    golden = GOLDEN.read_text(encoding="utf-8")

    require("--ppa-internal-tile-benchmark" in build,
            "missing P10E-B CLI selector")
    require("P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_PROFILE" in build,
            "missing P10E-B build export")
    require("P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_PROFILE" in main_cmake,
            "missing P10E-B CMake profile")
    require("P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_PROFILE=1" in main_cmake,
            "missing P10E-B compile define")
    require("p4_nano_ppa_internal_tile.cpp" in live_cmake,
            "missing P10E-B source linkage")
    require("esp_driver_ppa" in live_cmake,
            "missing PPA dependency")
    require("P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_PROFILE" in live,
            "missing P10E-B lifecycle dispatch")

    route_start = main.index(
        "constexpr uart_port_t kDisplayBenchmarkApplicationConsoleUart")
    route_guard_start = main.rfind(
        "#if defined(P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE)", 0, route_start)
    route_guard_end = main.index("#endif", route_guard_start)
    route_guard = main[route_guard_start:route_guard_end]
    require("P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_PROFILE" not in route_guard,
            "P10E-B must not trigger the retained GPIO20 route")
    app_start = main.index('extern "C" void app_main(void)')
    hello = main.index("ESP-NP2KAI HELLO WORLD OK", app_start)
    require("uart_set_pin" not in main[app_start:hello],
            "P10E-B must use the normal UART0 application path")

    for fragment in (
        "kSourceWidth = 640U", "kSourceHeight = 400U",
        "kOutputWidth = 400U", "kOutputHeight = 640U",
        "kLargestTileWidth = 128U", "kLargestTileBytes",
        "kWarmupSamples = 8U", "kMeasuredSamples = 128U",
        "kFinalValidationSamples = 1U",
        "kExpectedSourceCrc = 0x8dadbf82U",
        "kExpectedOutputCrc = 0x379511d7U",
    ):
        require(fragment in header, f"missing geometry contract: {fragment}")
    for tile_width, tile_count, tile_bytes in ((32, 20, 25600),
                                                (64, 10, 51200),
                                                (128, 5, 102400)):
        require(f"tile_count({tile_width}U) == {tile_count}U" in source,
                f"missing T{tile_width} tile-count static contract")
        require(f"tile_bytes({tile_width}U) == {tile_bytes}U" in source,
                f"missing T{tile_width} tile-byte static contract")

    for fragment in (
        "MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT",
        "heap_caps_aligned_alloc", "PPA_SRM_ROTATION_ANGLE_90",
        "PPA_TRANS_MODE_BLOCKING", "block_w = tile_width",
        "block_h = kSourceHeight", "block_offset_x = source_x",
        "buffer_size = static_cast<std::uint32_t>(kLargestTileBytes)",
        "source_crc_before", "source_crc_after", "reference_matches",
        "P4_NANO_PPA_TILE_CONFIG", "P4_NANO_PPA_TILE_SERVICE",
        "P4_NANO_PPA_TILE_SAMPLE_COUNTS", "warmup=%u measured=%u",
        "P4_NANO_PPA_TILE_CORRECTNESS", "P4_NANO_PPA_TILE_RESULT",
        "P4_NANO_PPA_INTERNAL_TILE_RESULT", "vTaskDelay(1)",
    ):
        require(fragment in source, f"missing P10E-B implementation contract: {fragment}")
    require("std::array<PhaseStats, 3U> s_phase_stats{}" in source,
            "phase statistics must use static storage")
    require("PhaseStats stats{}" not in source,
            "per-phase statistics must not be automatic storage")
    for twdt_api in ("esp_task_wdt_init", "esp_task_wdt_reconfigure",
                     "esp_task_wdt_add", "esp_task_wdt_delete",
                     "esp_task_wdt_reset"):
        require(twdt_api not in source, f"P10E-B must not call {twdt_api}")
    require("exact2x" not in source.lower() and "pie" not in source.lower(),
            "P10E-B must not include PIE/exact2x implementation")
    require("p4_nano_ppa_internal_tile::run" in live,
            "P10E-B must dispatch the tile service")

    # Pure-host semantic model using a deterministic RGB565-like byte fixture.
    fixture = bytearray(640 * 400 * 2)
    for index in range(0, len(fixture), 2):
        pixel = (index // 2 * 37 + 11) & 0xffff
        fixture[index:index + 2] = struct.pack("<H", pixel)
    direct = direct_ccw(bytes(fixture))
    for tile_width, expected_count in ((32, 20), (64, 10), (128, 5)):
        tiled, source_xes = tiled_ccw(bytes(fixture), tile_width)
        require(len(source_xes) == expected_count,
                f"T{tile_width} tile count changed")
        require(source_xes[0] == 640 - tile_width,
                f"T{tile_width} first strip is not rightmost")
        require(source_xes[-1] == 0,
                f"T{tile_width} final strip does not reach source x=0")
        require(tiled == direct,
                f"T{tile_width} tiled model differs from direct CCW")
        require(tile_width * expected_count == 640,
                f"T{tile_width} has a gap/overlap in destination rows")
        require((expected_count - 1) * tile_width == 640 - tile_width,
                f"T{tile_width} final destination row mapping changed")

    require('"crc32": "0x8dadbf82"' in golden,
            "retained source CRC contract changed")
    require("P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_PROFILE" not in
            main[route_guard_start:route_guard_end],
            "GPIO20 routing guard accidentally includes P10E-B")
    print("Display Performance P10E-B internal PPA tile host/static contract passed")
    print("P10E_TILE_HOST_MODEL T32=20 T64=10 T128=5 byte_exact=PASS")
    print(f"P10E_TILE_SYNTHETIC_DIRECT_CRC=0x{zlib.crc32(direct) & 0xffffffff:08x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
