#!/usr/bin/env python3
"""Perform mechanical checks on a retained reference BMP."""

from __future__ import annotations

import argparse
import pathlib
import struct


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("bmp", type=pathlib.Path)
    args = parser.parse_args()
    data = args.bmp.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise SystemExit("BMP header is invalid")
    file_size, pixel_offset = struct.unpack_from("<II", data, 2)[0], struct.unpack_from("<I", data, 10)[0]
    header_size, width, height, planes, bpp, compression, image_size = struct.unpack_from("<IiiHHII", data, 14)
    if (header_size, width, height, planes, bpp, compression) != (40, 640, 400, 1, 24, 0):
        raise SystemExit("BMP dimensions or format are invalid")
    row_stride = (640 * 3 + 3) & ~3
    if file_size != len(data) or image_size != row_stride * 400 or pixel_offset != 54:
        raise SystemExit("BMP size fields are invalid")
    pixels = data[pixel_offset:]
    if not pixels or not any(pixels):
        raise SystemExit("BMP is empty or all black")
    # The text scene must contain bright pixels in both upper and lower halves.
    if not any(pixels[: len(pixels) // 2]) or not any(pixels[len(pixels) // 2:]):
        raise SystemExit("BMP does not contain broad scene activity")
    print(f"video BMP mechanical validation passed path={args.bmp} bytes={len(data)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
