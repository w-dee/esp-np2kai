#!/usr/bin/env python3
"""Validate the Step 7A.3c graphics BMP scene without freezing a CRC."""

from __future__ import annotations

import argparse
import pathlib
import struct


WIDTH = 640
HEIGHT = 400
ROW_STRIDE = (WIDTH * 3 + 3) & ~3

PALETTE = [
    (0, 0, 0), (0, 0, 115), (115, 0, 0), (115, 0, 115),
    (0, 117, 0), (0, 117, 115), (115, 117, 0), (115, 117, 115),
    (66, 69, 66), (0, 0, 255), (255, 0, 0), (255, 0, 255),
    (0, 255, 0), (0, 255, 255), (255, 255, 0), (255, 255, 255),
]
BLACK = PALETTE[0]
WHITE = PALETTE[15]


def read_bmp(path: pathlib.Path) -> tuple[int, int, list[list[tuple[int, int, int]]]]:
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise AssertionError("BMP header is invalid")
    file_size = struct.unpack_from("<I", data, 2)[0]
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    header_size, width, height, planes, bpp, compression, image_size = struct.unpack_from(
        "<IiiHHII", data, 14
    )
    if (header_size, width, abs(height), planes, bpp, compression) != (
        40, WIDTH, HEIGHT, 1, 24, 0
    ):
        raise AssertionError("BMP dimensions or format are invalid")
    if file_size != len(data) or pixel_offset != 54 or image_size != ROW_STRIDE * HEIGHT:
        raise AssertionError("BMP size fields are invalid")

    pixels: list[list[tuple[int, int, int]]] = []
    for y in range(HEIGHT):
        source_y = HEIGHT - 1 - y if height > 0 else y
        row_start = pixel_offset + source_y * ROW_STRIDE
        row: list[tuple[int, int, int]] = []
        for x in range(WIDTH):
            blue, green, red = data[row_start + x * 3 : row_start + x * 3 + 3]
            row.append((red, green, blue))
        pixels.append(row)
    return width, abs(height), pixels


def check_scene(pixels: list[list[tuple[int, int, int]]]) -> None:
    colors = {pixel for row in pixels for pixel in row}
    expected_colors = set(PALETTE)
    if colors != expected_colors:
        raise AssertionError(
            f"BMP colors differ: expected 16 palette colors, got {len(colors)}: {sorted(colors)}"
        )

    samples = {
        "top-left marker": ((32, 32), 1),
        "top-right marker": ((600, 32), 2),
        "bottom-left marker": ((32, 360), 4),
        "bottom-right marker": ((600, 348), 8),
        "top frame": ((100, 8), 15),
        "bottom frame": ((100, 391), 15),
        "left frame": ((8, 100), 15),
        "right frame": ((631, 100), 15),
        "cross horizontal": ((300, 239), 15),
        "cross vertical": ((319, 300), 15),
    }
    for name, ((x, y), index) in samples.items():
        if pixels[y][x] != PALETTE[index]:
            raise AssertionError(
                f"{name} at ({x},{y}) is {pixels[y][x]}, expected {PALETTE[index]}"
            )

    for row in range(4):
        y = 96 + row * 22
        for column in range(4):
            x = 96 + column * 32
            index = row * 4 + column
            for border_x in range(x, x + 28):
                if pixels[y][border_x] != WHITE or pixels[y + 17][border_x] != WHITE:
                    raise AssertionError(f"swatch {index} horizontal border is incorrect")
            for border_y in range(y, y + 18):
                if pixels[border_y][x] != WHITE or pixels[border_y][x + 27] != WHITE:
                    raise AssertionError(f"swatch {index} vertical border is incorrect")
            for interior_y in range(y + 1, y + 17):
                for interior_x in range(x + 1, x + 27):
                    if pixels[interior_y][interior_x] != PALETTE[index]:
                        raise AssertionError(f"swatch {index} interior is incorrect")

            if column < 3:
                for gap_x in range(x + 28, x + 32):
                    if pixels[y + 5][gap_x] != BLACK:
                        raise AssertionError(f"horizontal gap after swatch {index} is not black")
            if row < 3:
                for gap_y in range(y + 18, y + 22):
                    if pixels[gap_y][x + 5] != BLACK:
                        raise AssertionError(f"vertical gap after swatch {index} is not black")

    for y, expected_x in ((50, {8, 631}), (300, {8, 319, 631})):
        nonblack = {x for x, pixel in enumerate(pixels[y]) if pixel != BLACK}
        if nonblack != expected_x:
            raise AssertionError(
                f"row y={y} has non-black pixels {sorted(nonblack)}, expected {sorted(expected_x)}"
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bmp", type=pathlib.Path)
    args = parser.parse_args()
    width, height, pixels = read_bmp(args.bmp)
    check_scene(pixels)
    print(
        f"np2video-gfx-vram BMP validation passed path={args.bmp} "
        f"size={width}x{height} colors={len(PALETTE)} row50_nonblack=2 row300_nonblack=3"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
