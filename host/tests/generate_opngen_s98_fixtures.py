#!/usr/bin/env python3
"""Generate the small, rights-clear S98V3 parser corpus used by S2 tests."""

import argparse
import struct
from pathlib import Path


CLOCK = 3_993_600
HEADER_BYTES = 0x30


def header(timer_numerator=1, timer_denominator=1000):
    result = bytearray(HEADER_BYTES)
    result[:4] = b"S983"
    struct.pack_into("<I", result, 0x04, timer_numerator)
    struct.pack_into("<I", result, 0x08, timer_denominator)
    struct.pack_into("<I", result, 0x14, HEADER_BYTES)
    struct.pack_into("<I", result, 0x1C, 1)
    struct.pack_into("<I", result, 0x20, 2)
    struct.pack_into("<I", result, 0x24, CLOCK)
    return result


def write(register, value):
    return bytes((0x00, register, value))


def wait_syncs(syncs):
    if syncs == 1:
        return b"\xff"
    if syncs < 2:
        raise ValueError("sync count must be positive")
    raw = syncs - 2
    encoded = bytearray((0xfe,))
    while True:
        byte = raw & 0x7f
        raw >>= 7
        encoded.append(byte | (0x80 if raw else 0))
        if not raw:
            return bytes(encoded)


def finish(name, payload, numerator=1, denominator=1000):
    return name, bytes(header(numerator, denominator) + payload + b"\xfd")


def fixtures():
    yield finish(
        "fm_single_tone",
        b"".join((
            write(0x30, 0x01), write(0x40, 0x10), write(0x50, 0x1f),
            write(0x60, 0x08), write(0x70, 0x04), write(0x80, 0x26),
            write(0x90, 0x00), write(0xb0, 0x07), write(0xa4, 0x24),
            write(0xa0, 0x20), write(0x28, 0xf0), wait_syncs(40),
            write(0x28, 0x00), wait_syncs(10),
        )),
    )
    yield finish(
        "fm_frequency_change",
        b"".join((
            write(0xb0, 0x07), write(0xa4, 0x24), write(0xa0, 0x20),
            write(0x28, 0xf0), wait_syncs(147), write(0xa4, 0x25),
            write(0xa0, 0x40), wait_syncs(147), write(0x28, 0x00),
        )),
        1, 44100,
    )
    three_channel = bytearray()
    for channel in range(3):
        three_channel += write(0x30 + channel, 0x01 + channel)
        three_channel += write(0xb0 + channel, 0x07)
        three_channel += write(0xa4 + channel, 0x24 + channel)
        three_channel += write(0xa0 + channel, 0x20 + channel)
        three_channel += write(0x28, 0xf0 | channel)
    three_channel += wait_syncs(25)
    for channel in range(3):
        three_channel += write(0x28, channel)
    yield finish("fm_three_channel", bytes(three_channel))
    yield finish(
        "fm_same_timestamp_burst",
        b"".join((
            write(0x30, 0x01), write(0x40, 0x10), write(0xb0, 0x07),
            write(0xa4, 0x24), write(0xa0, 0x20), write(0x28, 0xf0),
            wait_syncs(1), write(0x28, 0x00),
        )),
    )
    yield finish(
        "fm_envelope",
        b"".join((
            write(0x30, 0x01), write(0x40, 0x10), write(0x50, 0x1f),
            write(0x60, 0x08), write(0x70, 0x04), write(0x80, 0x26),
            write(0x90, 0x00), write(0xb0, 0x07), write(0xa4, 0x24),
            write(0xa0, 0x20), write(0x28, 0xf0), wait_syncs(8),
            write(0x28, 0x00),
        )),
    )
    yield finish(
        "fm_algorithm_feedback",
        b"".join((
            write(0x30, 0x01), write(0x40, 0x10), write(0xb0, 0x00),
            write(0xa4, 0x24), write(0xa0, 0x20), write(0x28, 0xf0),
            wait_syncs(4), write(0xb0, 0x38), wait_syncs(4),
            write(0xb0, 0x07), write(0x28, 0x00),
        )),
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for name, payload in fixtures():
        (args.output_dir / f"{name}.s98").write_bytes(payload)
        print(f"S98_SYNTHETIC_FIXTURE name={name} bytes={len(payload)}")


if __name__ == "__main__":
    main()
