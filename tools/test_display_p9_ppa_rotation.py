#!/usr/bin/env python3
"""Host/source contract checks for the P9B PPA rotation-only benchmark."""

from __future__ import annotations

import pathlib
import struct
import zlib
import argparse


ROOT = pathlib.Path(__file__).resolve().parents[1]
REFERENCE = ROOT / "firmware/components/p4_nano_live_display/include/p4_nano_live_display/p4_nano_ppa_rotation_reference.hpp"
RUNTIME = ROOT / "firmware/components/p4_nano_live_display/include/p4_nano_live_display/p4_nano_ppa_rotation.hpp"
SOURCE = ROOT / "firmware/components/p4_nano_live_display/p4_nano_ppa_rotation.cpp"
LIVE = ROOT / "firmware/components/p4_nano_live_display/p4_nano_live_display.cpp"
LIVE_CMAKE = ROOT / "firmware/components/p4_nano_live_display/CMakeLists.txt"
MAIN_CMAKE = ROOT / "firmware/main/CMakeLists.txt"
MAIN = ROOT / "firmware/main/main.cpp"
BUILD = ROOT / "tools/emu/build-production.sh"
GOLDEN = ROOT / "tests/guest/np2video-live/golden.json"


def require(text: str, fragment: str, name: str) -> None:
    if fragment not in text:
        raise AssertionError(f"missing {name}: {fragment}")


def retained_bmp_crc(path: pathlib.Path) -> int:
    if not path.exists():
        raise AssertionError(f"fixture BMP does not exist: {path}")
    data = path.read_bytes()
    if data[:2] != b"BM" or len(data) != 768054:
        raise AssertionError("retained reference BMP has unexpected format")
    pixels = bytearray()
    row_bytes = ((640 * 3 + 3) // 4) * 4
    for by in range(399, -1, -1):
        row = data[54 + by * row_bytes:54 + (by + 1) * row_bytes]
        for x in range(640):
            b, g, r = row[x * 3:x * 3 + 3]
            pixels += struct.pack("<H", ((r >> 3) << 11) |
                                  ((g >> 2) << 5) | (b >> 3))
    if (zlib.crc32(pixels) & 0xffffffff) != 0x8DADBF82:
        raise AssertionError("retained BMP does not match tracked source golden CRC")
    output = bytearray(512000)
    for sy in range(400):
        for sx in range(640):
            dx, dy = sy, 639 - sx
            output[(dy * 400 + dx) * 2:(dy * 400 + dx + 1) * 2] = \
                pixels[(sy * 640 + sx) * 2:(sy * 640 + sx + 1) * 2]
    return zlib.crc32(output) & 0xffffffff


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--fixture-bmp",
        type=pathlib.Path,
        help="verify the full live fixture from a runner-produced 640x400 BMP",
    )
    args = parser.parse_args()

    reference = REFERENCE.read_text(encoding="utf-8")
    runtime = RUNTIME.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    live = LIVE.read_text(encoding="utf-8")
    live_cmake = LIVE_CMAKE.read_text(encoding="utf-8")
    main_cmake = MAIN_CMAKE.read_text(encoding="utf-8")
    main = MAIN.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")
    golden = GOLDEN.read_text(encoding="utf-8")

    for fragment, name in (("kSourceWidth = 640U", "source width"),
                           ("kSourceHeight = 400U", "source height"),
                           ("kOutputWidth = 400U", "output width"),
                           ("kOutputHeight = 640U", "output height"),
                           ("kSourceBytes =", "source byte contract"),
                           ("kExpectedOutputCrc = 0x379511d7U", "fixture CRC")):
        require(reference, fragment, name)
    require(reference, "*dx = sy", "CCW x mapping")
    require(reference, "*dy = (kSourceWidth - 1U) - sx", "CCW y mapping")
    require(runtime, "PPA rotation-only", "runtime API documentation")
    require(source, "PPA_DATA_BURST_LENGTH_128", "128-byte burst")
    require(source, "PPA_TRANS_MODE_BLOCKING", "blocking transaction")
    require(source, "PPA_SRM_ROTATION_ANGLE_90", "CCW90 operation")
    require(source, "scale_x = 1.0F", "scale x")
    require(source, "scale_y = 1.0F", "scale y")
    require(source, "heap_caps_aligned_alloc", "aligned output allocation")
    require(source, "kWarmupSamples", "warm-up count")
    require(source, "kMeasuredSamples", "measured count")
    require(source, "reference_matches", "full output reference")
    require(source, "source_crc_before", "source immutability CRC")
    require(source, "ppa_rotation_blocking_wall_us", "blocking timing semantics")
    require(live, "run_ppa_rotation_benchmark_after_start", "isolated lifecycle")
    require(live, "producer_pause_acknowledged", "pause acknowledgement")
    require(live, "native_framebuffer_write=0", "native framebuffer isolation")
    require(live, "P4_NANO_PPA_ROTATION_VSYNC_VALID", "VSYNC validity gate")
    require(live_cmake, "p4_nano_ppa_rotation.cpp", "P9-only source")
    require(live_cmake, "esp_driver_ppa", "P9-only PPA dependency")
    require(main_cmake, "P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE_ACTIVE", "P9 gate")
    require(main, "P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE", "P9 dispatch")
    require(main, '#include "driver/gpio.h"', "P9 GPIO enum include")
    require(main, '#include "driver/uart.h"', "P9 public UART API include")
    require(main, "P4_NANO_EXACT2X_SCALER_BENCHMARK_PROFILE", "P10 dispatch/gate")
    require(main_cmake,
            "list(APPEND NP2_MAIN_PRIV_REQUIRES esp_driver_uart esp_driver_gpio)",
            "P9 UART component dependency")
    route_start = main.index(
        "constexpr uart_port_t kDisplayBenchmarkApplicationConsoleUart")
    route_end = main.index("} // namespace", route_start)
    route = main[route_start:route_end]
    for fragment, name in (
        ("UART_NUM_0", "UART0 route"),
        ("GPIO_NUM_20", "GPIO20 route"),
        ("UART_PIN_NO_CHANGE", "unchanged UART pins"),
        ("return uart_set_pin", "public route call"),
    ):
        require(route, fragment, name)
    if "uart_driver_install" in route or "uart_set_baudrate" in route:
        raise AssertionError("P9 route must not install a driver or change baud")
    if "gpio_reset_pin" in main or "GPIO_NUM_37" in route:
        raise AssertionError("P9 route must not reconfigure GPIO37")
    app_start = main.index('extern "C" void app_main(void)')
    hello = main.index('ESP-NP2KAI HELLO WORLD OK', app_start)
    app_prefix = main[app_start:hello]
    require(app_prefix, "route_display_benchmark_application_console()",
            "route before HELLO")
    require(app_prefix, "benchmark_uart_route_result != ESP_OK",
            "route failure check")
    require(app_prefix, "return;", "route failure stops app")
    require(main, "#if defined(P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE) ||",
            "P9/P10 benchmark-only route gate")
    require(main, "defined(P4_NANO_EXACT2X_SCALER_BENCHMARK_PROFILE)",
            "P9/P10 exact2x route gate")
    measured_start = source.index(
        "if (all_operations_succeeded) {\n        for (std::size_t index = 0U;")
    measured_end = source.index("bool final_operation_succeeded", measured_start)
    measured = source[measured_start:measured_end]
    if "printf" in measured or "uart_set_pin" in measured:
        raise AssertionError("P9 measured PPA loop must not log or reroute UART")
    require(build, "--ppa-rotation-benchmark", "P9 selector")
    require(build, "--exact2x-scaler-benchmark", "P10 selector")
    require(build, "P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE", "P9 build export")
    require(golden, '"crc32": "0x8dadbf82"', "tracked source golden")

    retained_crc = None
    if args.fixture_bmp is not None:
        retained_crc = retained_bmp_crc(args.fixture_bmp)
    else:
        for candidate in (
            ROOT / "host/build/runner/np2video-live-reference.bmp",
            ROOT / "host/build/phase2/runner/np2video-live-reference.bmp",
        ):
            if candidate.exists():
                retained_crc = retained_bmp_crc(candidate)
                break
    if retained_crc is not None and retained_crc != 0x379511D7:
        raise AssertionError(f"rotated fixture CRC mismatch: 0x{retained_crc:08x}")

    print("Display Performance P9C-R3B UART0 GPIO20 route contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
