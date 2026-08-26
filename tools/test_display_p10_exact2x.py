#!/usr/bin/env python3
"""Host contract and independent golden test for the P10 exact-2x scaler."""

from __future__ import annotations

import argparse
import pathlib
import struct
import zlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "firmware/components/p4_nano_display/include/p4_nano_display/p4_nano_display_exact2x.hpp"
SOURCE = ROOT / "firmware/components/p4_nano_display/p4_nano_display_exact2x.cpp"
LIVE = ROOT / "firmware/components/p4_nano_live_display/p4_nano_live_display.cpp"
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
    live = LIVE.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")
    golden = GOLDEN.read_text(encoding="utf-8")
    for fragment in ("kExact2xSourceWidth = 400U",
                     "kExact2xSourceHeight = 640U",
                     "kExact2xDestinationWidth = 800U",
                     "kExact2xDestinationHeight = 1280U",
                     "kExact2xSourceBytes",
                     "kExact2xDestinationBytes",
                     "kExact2xExpectedDestinationCrc = 0xc8a10b55U"):
        require(fragment in header, f"missing geometry/CRC contract: {fragment}")
    for fragment in ("const std::uint32_t packed",
                     "destination_pairs0[x] = packed",
                     "destination_pairs1[x] = packed"):
        require(fragment in source, f"missing scalar packed store: {fragment}")
    require("P4_NANO_EXACT2X_SCALER_BENCHMARK_PROFILE" in live,
            "missing isolated P10 selector")
    require("P4_NANO_EXACT2X_PIE_TIMING PIE_AVAILABLE=0" in live,
            "PIE availability must be explicit until zip semantics are proven")
    require("P4_NANO_EXACT2X_PIE_CORRECTNESS PIE_AVAILABLE=0 status=BLOCKED" in live,
            "missing blocked PIE correctness status")
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
    print("Display Performance P10B exact2x host contract passed")
    print(f"P10_EXACT2X_SOURCE_CRC=0x{zlib.crc32(rotated) & 0xffffffff:08x}")
    print(f"P10_EXACT2X_EXPECTED_CRC=0x{expected_crc:08x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
