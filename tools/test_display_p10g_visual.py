#!/usr/bin/env python3
"""Host/static contract for the P10G-B lower-refresh visual profiles."""

from __future__ import annotations

import pathlib
import struct
import subprocess
import zlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "tools/emu/build-production.sh"
MAIN = ROOT / "firmware/main/main.cpp"
MAIN_CMAKE = ROOT / "firmware/main/CMakeLists.txt"
DISPLAY = ROOT / "firmware/components/p4_nano_display/p4_nano_display.cpp"
DISPLAY_CMAKE = ROOT / "firmware/components/p4_nano_display/CMakeLists.txt"
VISUAL = ROOT / "firmware/components/p4_nano_display/p4_nano_refresh_visual.cpp"
HEADER = ROOT / "firmware/components/p4_nano_display/include/p4_nano_display/p4_nano_refresh_visual.hpp"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def visual_crc() -> int:
    """Independent RGB565 model of the deterministic visual image."""
    width, height = 800, 1280
    black, red, green, blue, white = 0x0000, 0xF800, 0x07E0, 0x001F, 0xFFFF
    yellow, cyan, magenta = 0xFFE0, 0x07FF, 0xF81F
    gray25, gray50, gray75 = 0x4208, 0x8410, 0xBDF7
    pixels = [black] * (width * height)

    for y in range(height):
        for x in range(width):
            value = black
            if y < 8:
                value = red
            elif x >= width - 8:
                value = green
            elif y >= height - 8:
                value = blue
            elif x < 8:
                value = white
            if x < 64 and y < 64:
                value = yellow
            elif x >= width - 64 and y < 64:
                value = magenta
            elif x < 64 and y >= height - 64:
                value = cyan
            elif x >= width - 64 and y >= height - 64:
                value = 0xFD20
            pixels[y * width + x] = value

    def rect(x: int, y: int, w: int, h: int, value: int) -> None:
        for row in range(y, y + h):
            pixels[row * width + x:row * width + x + w] = [value] * w

    rect(96, 0, 608, 8, red)
    rect(96, height - 8, 608, 8, blue)
    rect(0, 96, 8, 1088, white)
    rect(width - 8, 96, 8, 1088, green)
    rect(96, 128, 160, 128, red)
    rect(272, 128, 160, 128, green)
    rect(448, 128, 160, 128, blue)
    rect(624, 128, 128, 128, white)
    rect(96, 272, 160, 80, black)
    rect(272, 272, 160, 80, gray25)
    rect(448, 272, 160, 80, gray50)
    rect(624, 272, 128, 80, gray75)
    for y in range(400, 528, 8):
        for x in range(96, 352, 8):
            rect(x, y, 8, 8, white if (((x - 96) // 8 + (y - 400) // 8) & 1) else black)
    for x in range(96, 752):
        pixels[376 * width + x] = white if x & 1 else black
        pixels[378 * width + x] = black if x & 1 else white
    for y in range(400, 528):
        pixels[y * width + 376] = white if y & 1 else black
        pixels[y * width + 378] = black if y & 1 else white
    rect(96, 560, 128, 64, magenta)
    rect(240, 552, 192, 96, cyan)
    rect(448, 544, 256, 112, yellow)
    rect(112, 680, 592, 8, white)
    rect(112, 688, 8, 128, white)
    rect(120, 808, 48, 8, white)
    rect(688, 96, 8, 64, white)
    rect(688, 96, 64, 8, white)
    rect(744, 152, 8, 8, red)
    rect(760, 88, 16, 16, black)
    packed = b"".join(struct.pack("<H", value) for value in pixels)
    return zlib.crc32(packed) & 0xFFFFFFFF


def expect_cli_failure(*args: str) -> None:
    result = subprocess.run(
        ["bash", str(BUILD), *args], cwd=ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
    require(result.returncode == 2,
            f"expected CLI rejection for {' '.join(args)}: {result.stdout}")


def main() -> int:
    build = BUILD.read_text(encoding="utf-8")
    main = MAIN.read_text(encoding="utf-8")
    main_cmake = MAIN_CMAKE.read_text(encoding="utf-8")
    display = DISPLAY.read_text(encoding="utf-8")
    display_cmake = DISPLAY_CMAKE.read_text(encoding="utf-8")
    visual = VISUAL.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")

    for fragment in (
        "--display-refresh-visual",
        "baseline|lower1|lower2",
        "refresh_visual_selected",
        "build-${board}-${variant}-refresh-visual-${refresh_visual_profile}",
        "P4_NANO_REFRESH_VISUAL_PROFILE=${refresh_visual_profile}",
    ):
        require(fragment in build, f"missing visual CLI/build contract: {fragment}")
    require("P4_NANO_REFRESH_VISUAL_PROFILE_ACTIVE" in main_cmake,
            "missing CMake visual selector")
    require("mutually exclusive with other profiles" in main_cmake,
            "missing visual profile mutual exclusion")
    require("P4_NANO_REFRESH_VISUAL_PROFILE=1" in main_cmake,
            "missing main visual compile definition")
    require("p4_nano_refresh_visual.cpp" in display_cmake,
            "visual source is not isolated to the visual profile")

    for fragment in (
        '"baseline", "PLL_F240M"', '80.0F', '1500.0F', '68.662455F',
        '"lower1", "PLL_F240M"', '48.0F', '700.0F', '41.197473F',
        '"lower2", "PLL_F240M"', '240.0F / 7.0F', '500.0F', '29.426767F',
        'predicted_divider == 5U', 'predicted_divider == 7U',
        'MIPI_DSI_DPI_CLK_SRC_PLL_F240M', 'htotal == 880U', 'vtotal == 1324U',
    ):
        require(fragment in header, f"missing timing constant/static check: {fragment}")
    require("MIPI_DSI_DPI_CLK_SRC_DEFAULT" in display and
            "kRefreshVisualConfig.dpi_clock_source" in display,
            "production default and visual source isolation changed")
    for fragment in (
        "constexpr std::uint8_t kDsiLaneCount = 2;",
        "constexpr std::size_t kFramebufferCount = 1;",
        "dpi_config.num_fbs = kFramebufferCount;",
        "dpi_config.video_timing.hsync_back_porch = 20;",
        "dpi_config.video_timing.hsync_pulse_width = 20;",
        "dpi_config.video_timing.hsync_front_porch = 40;",
        "dpi_config.video_timing.vsync_back_porch = 10;",
        "dpi_config.video_timing.vsync_pulse_width = 4;",
        "dpi_config.video_timing.vsync_front_porch = 30;",
        "dpi_config.flags.use_dma2d = false;",
    ):
        require(fragment in display,
                f"production display geometry contract changed: {fragment}")
    for forbidden in ("MIPI_DSI_DPI_CLK_SRC_APLL", "refresh_rate",
                      "vsync_front_porch", "gpio_set_level", "GPIO_NUM_20"):
        require(forbidden not in visual,
                f"visual profile contains forbidden retuning/routing: {forbidden}")
    require("P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE" in main and
            "GPIO_NUM_20" in main,
            "benchmark-only GPIO20 route disappeared from its guarded profile")
    visual_dispatch = main[main.index("P4_NANO_REFRESH_VISUAL_PROFILE"):]
    require("run_refresh_visual()" in visual_dispatch,
            "visual profile does not dispatch its isolated application")

    for fragment in (
        "P4_NANO_REFRESH_VISUAL_CONFIG mode=%s",
        "requested_dpi_mhz=%.9f",
        "predicted_refresh_hz=%.9f",
        "format=RGB565 num_fbs=1",
        "fill_static_pattern",
        "draw_border", "draw_primary_regions", "draw_checkerboard",
        "draw_one_pixel_lines", "draw_rectangles_and_orientation",
        "validate_refresh_visual_pattern",
        "P4_NANO_REFRESH_VISUAL_PATTERN crc=0x%08",
        "P4_NANO_REFRESH_VISUAL_BACKLIGHT_ON=%s",
        "display_session_reset_vsync(&resources)",
        "phase=static duration_s=45",
        "phase=dynamic_marker duration_s=15 nominal_toggle_hz=1",
        "kDynamicMarkerUpdates = 15U",
        "update_refresh_visual_marker(resources.framebuffer",
        "P4_NANO_REFRESH_VISUAL_VSYNC",
        "period_count + 1U ==",
        "P4_NANO_REFRESH_VISUAL_SOFTWARE_RESULT=%s",
        "P4_NANO_REFRESH_VISUAL_HUMAN_VERDICT_REQUIRED=1",
        "P4_NANO_REFRESH_VISUAL_BACKLIGHT_OFF=%s",
    ):
        require(fragment in visual, f"missing visual lifecycle contract: {fragment}")
    static_phase = visual[visual.index("phase=static duration_s=45"):visual.index(
        "phase=dynamic_marker duration_s=15")]
    require("update_refresh_visual_marker" not in static_phase,
            "static phase must not write the framebuffer")
    require("vTaskDelay(kStaticInspectionTicks)" in static_phase,
            "static phase must use a blocking delay")
    require("HUMAN_VISUAL_VERDICT=PASS" not in visual,
            "firmware must not infer a human visual verdict")
    require(visual_crc() == 0x67727B8D,
            f"deterministic visual CRC changed: 0x{visual_crc():08x}")

    expect_cli_failure("--variant", "p4-v1x", "--board", "generic",
                       "--display-refresh-visual", "lower1")
    expect_cli_failure("--variant", "p4-v1x", "--board", "p4-nano",
                       "--display-refresh-visual", "lower1",
                       "--display-foundation")
    expect_cli_failure("--variant", "p4-v1x", "--board", "p4-nano",
                       "--display-refresh-visual", "unsupported")

    print("Display Performance P10G-B lower-refresh visual host/static contract passed")
    print("P10G_VISUAL_PATTERN_CRC=0x67727b8d")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
