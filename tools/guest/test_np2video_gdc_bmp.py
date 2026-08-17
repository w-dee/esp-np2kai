#!/usr/bin/env python3
"""Independently validate the Step 7A.3d rendered BMP scene."""

from __future__ import annotations

import argparse
import json
import pathlib

from test_np2video_gfx_vram_bmp import BLACK, PALETTE, read_bmp


ROOT = pathlib.Path(__file__).resolve().parents[2]
LAYOUT = ROOT / "tests/guest/np2video-gdc/layout.json"
WIDTH = 640
HEIGHT = 400


def expected_pixels(layout: dict) -> list[list[int]]:
    expected = [[0 for _ in range(WIDTH)] for _ in range(HEIGHT)]
    for line in layout["scene"]["lines"]:
        x, y = line["from"]
        dc = line["DC"]
        d1 = line["D1"]
        direction = int(line["ope"], 0) & 7
        color = line["color"]
        for index in range(dc + 1):
            if dc == 0:
                step = 0
            else:
                step = (((d1 * index) // dc) + 1) >> 1
            if direction == 0:
                pixel_x, pixel_y = x + step, y + index
            elif direction == 1:
                pixel_x, pixel_y = x + index, y + step
            elif direction == 2:
                pixel_x, pixel_y = x + index, y - step
            elif direction == 3:
                pixel_x, pixel_y = x + step, y - index
            elif direction == 4:
                pixel_x, pixel_y = x - step, y - index
            elif direction == 5:
                pixel_x, pixel_y = x - index, y - step
            elif direction == 6:
                pixel_x, pixel_y = x - index, y + step
            else:
                pixel_x, pixel_y = x - step, y + index

            if not (0 <= pixel_x < WIDTH and 0 <= pixel_y < HEIGHT):
                raise AssertionError(f"scene line leaves the visible surface at {(pixel_x, pixel_y)}")
            previous = expected[pixel_y][pixel_x]
            if previous not in (0, color):
                raise AssertionError(
                    f"unexpected overlap of colors {previous} and {color} at {(pixel_x, pixel_y)}"
                )
            expected[pixel_y][pixel_x] = color
    return expected


def check_scene(pixels: list[list[tuple[int, int, int]]], layout: dict) -> None:
    expected = expected_pixels(layout)
    nonblack = 0
    for y in range(HEIGHT):
        for x in range(WIDTH):
            color_index = expected[y][x]
            expected_rgb = BLACK if color_index == 0 else PALETTE[color_index]
            if pixels[y][x] != expected_rgb:
                raise AssertionError(
                    f"pixel mismatch at ({x},{y}): got {pixels[y][x]}, expected {expected_rgb}"
                )
            if color_index != 0:
                nonblack += 1

    expected_color_indices = {index for row in expected for index in row}
    actual_colors = {pixel for row in pixels for pixel in row}
    expected_colors = {BLACK} | {PALETTE[index] for index in expected_color_indices if index != 0}
    if actual_colors != expected_colors:
        raise AssertionError("BMP contains colors outside the exact GDC scene")
    if nonblack <= 0:
        raise AssertionError("GDC BMP contains no visible geometry")

    # Mechanical checks for the distinctive endpoints and empty regions make
    # accidental mirroring, inversion, duplication, or text visible to CI.
    for line in layout["scene"]["lines"]:
        for x, y in (line["from"], line["to"]):
            if pixels[y][x] != PALETTE[line["color"]]:
                raise AssertionError(f"line endpoint is incorrect at {(x, y)}")
    for x, y in ((0, 0), (320, 0), (0, 399), (639, 399), (320, 50), (320, 350)):
        if expected[y][x] == 0 and pixels[y][x] != BLACK:
            raise AssertionError(f"empty point is not black at {(x, y)}")

    # The scene has no text or cursor: several large interior probes must be
    # black unless occupied by the explicitly described yellow rectangle.
    for x, y in ((20, 40), (320, 40), (620, 40), (20, 200), (320, 220), (620, 200), (320, 380)):
        if expected[y][x] == 0 and pixels[y][x] != BLACK:
            raise AssertionError(f"unexpected text/cursor-like pixel at {(x, y)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bmp", type=pathlib.Path)
    args = parser.parse_args()
    layout = json.loads(LAYOUT.read_text(encoding="utf-8"))
    width, height, pixels = read_bmp(args.bmp)
    if (width, height) != (WIDTH, HEIGHT):
        raise AssertionError("GDC BMP dimensions are incorrect")
    check_scene(pixels, layout)
    print(
        f"np2video-gdc BMP validation passed path={args.bmp} "
        f"size={width}x{height} full_scene=1 crc=REFERENCE_ONLY"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
