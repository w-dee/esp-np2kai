#!/usr/bin/env python3
"""Curate the audited RetroFM S98 into the strict FM-direct derivative.

This is a provenance/test-data tool, not a runtime parser.  It deliberately
accepts exactly one audited raw S98 identity and fails closed for every other
input.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


RAW_BYTES = 3762
RAW_SHA256 = "87de61e3d155d8ef9b44e78fae2d59204785ab4885539e38462d82fde7666a28"
RAW_WRITE_COUNT = 1050
RAW_FINAL_SYNC = 530082
REMOVALS = (
    (0, 0x22, 0x00),
    (0, 0x27, 0x00),
    (0, 0x07, 0x3F),
)


class CurationError(ValueError):
    """The input is not the exact audited raw S98."""


def _le32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def _decode_varint(data: bytes, cursor: int, limit: int) -> tuple[int, int]:
    value = 0
    shift = 0
    while cursor < limit:
        byte = data[cursor]
        cursor += 1
        value |= (byte & 0x7F) << shift
        if byte & 0x80 == 0:
            return value, cursor
        shift += 7
        if shift > 63:
            raise CurationError("S98 wait varint overflow")
    raise CurationError("truncated S98 wait varint")


def _strict_register(reg: int) -> bool:
    if reg == 0x28:
        return True
    if 0x30 <= reg <= 0x9F:
        return (reg & 0x03) != 0x03
    return reg in (*range(0xA0, 0xA3), *range(0xA4, 0xA7), *range(0xB0, 0xB3))


def _parse_raw(data: bytes) -> tuple[list[tuple[int, int, int, int]], list[tuple[int, int]], int]:
    if len(data) != RAW_BYTES:
        raise CurationError(f"raw byte count mismatch: {len(data)}")
    if hashlib.sha256(data).hexdigest() != RAW_SHA256:
        raise CurationError("raw SHA-256 mismatch")
    if len(data) < 0x30 or data[:4] != b"S983":
        raise CurationError("invalid S98 header")
    num, den, compression, tag, dump, loop, devices = struct.unpack_from("<7I", data, 4)
    if (num, den, compression, tag, dump, loop, devices) != (1, 44100, 0, 3587, 48, 0, 1):
        raise CurationError("raw S98 header contract mismatch")
    if struct.unpack_from("<4I", data, 0x20) != (2, 4000000, 0, 0):
        raise CurationError("raw S98 device contract mismatch")
    cursor = dump
    sync = 0
    writes: list[tuple[int, int, int, int]] = []
    waits: list[tuple[int, int]] = []
    dump_end = None
    while cursor < len(data):
        command_offset = cursor
        command = data[cursor]
        cursor += 1
        if command == 0x00:
            if cursor + 2 > len(data):
                raise CurationError("truncated S98 normal write")
            reg, value = data[cursor], data[cursor + 1]
            cursor += 2
            writes.append((command_offset, sync, reg, value))
        elif command == 0x01:
            if cursor + 2 > len(data):
                raise CurationError("truncated S98 extended write")
            raise CurationError("unexpected S98 extended write")
        elif command == 0xFF:
            sync += 1
            waits.append((command_offset, 1))
        elif command == 0xFE:
            raw, cursor = _decode_varint(data, cursor, len(data))
            wait = raw + 2
            sync += wait
            waits.append((command_offset, wait))
        elif command == 0xFD:
            dump_end = cursor
            break
        else:
            raise CurationError(f"unexpected S98 command 0x{command:02x}")
    if dump_end != tag:
        raise CurationError(f"dump/tag boundary mismatch: {dump_end} != {tag}")
    if data[tag : tag + 5] != b"[S98]":
        raise CurationError("missing S98 tag")
    if len(writes) != RAW_WRITE_COUNT or sync != RAW_FINAL_SYNC:
        raise CurationError("raw write/timing contract mismatch")
    if [write[1:] for write in writes[:3]] != list(REMOVALS):
        raise CurationError("special write order/value/timestamp mismatch")
    if any(reg in (0x07, 0x22, 0x27) for _, _, reg, _ in writes[3:]):
        raise CurationError("extra special register occurrence")
    if any(not _strict_register(reg) for _, _, reg, _ in writes[3:]):
        raise CurationError("unexpected unsupported register")
    return writes, waits, dump_end


def curate_bytes(data: bytes) -> bytes:
    """Return the strict derivative, or raise :class:`CurationError`."""

    writes, _waits, dump_end = _parse_raw(data)
    remove_offsets = {offset for offset, _sync, _reg, _value in writes[:3]}
    new_dump = bytearray()
    cursor = 48
    while cursor < dump_end:
        command = data[cursor]
        if cursor in remove_offsets:
            if command != 0x00:
                raise CurationError("removal offset is not a normal write")
            cursor += 3
            continue
        if command == 0x00:
            new_dump.extend(data[cursor : cursor + 3])
            cursor += 3
        elif command == 0x01:
            new_dump.extend(data[cursor : cursor + 3])
            cursor += 3
        elif command == 0xFE:
            _raw, end = _decode_varint(data, cursor + 1, dump_end)
            new_dump.extend(data[cursor:end])
            cursor = end
        else:
            new_dump.append(command)
            cursor += 1
    if len(new_dump) != (dump_end - 48) - 9:
        raise CurationError("curated dump length mismatch")
    result = bytearray(data[:48])
    result.extend(new_dump)
    result.extend(data[dump_end:])
    struct.pack_into("<I", result, 0x10, 3587 - 9)
    _validate_derivative(bytes(result))
    return bytes(result)


def _validate_derivative(data: bytes) -> None:
    if data[:4] != b"S983" or len(data) != 3753:
        raise CurationError("unexpected derivative identity")
    if tuple(struct.unpack_from("<7I", data, 4)) != (1, 44100, 0, 3578, 48, 0, 1):
        raise CurationError("derivative header repair mismatch")
    if struct.unpack_from("<4I", data, 0x20) != (2, 4000000, 0, 0):
        raise CurationError("derivative device mismatch")
    cursor = 48
    sync = 0
    writes = 0
    while cursor < 3578:
        command = data[cursor]
        cursor += 1
        if command in (0x00, 0x01):
            cursor += 2
            if command == 0x00:
                writes += 1
        elif command == 0xFF:
            sync += 1
        elif command == 0xFE:
            raw, cursor = _decode_varint(data, cursor, 3578)
            sync += raw + 2
        elif command == 0xFD:
            break
        else:
            raise CurationError("invalid derivative command")
    if cursor != 3578 or writes != 1047 or sync != RAW_FINAL_SYNC:
        raise CurationError("derivative structure mismatch")
    if data[3578:3583] != b"[S98]":
        raise CurationError("derivative tag mismatch")


def curate_file(input_path: Path, output_path: Path) -> bytes:
    data = input_path.read_bytes()
    curated = curate_bytes(data)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(curated)
    return curated


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        curated = curate_file(args.input, args.output)
    except (OSError, CurationError) as error:
        print(f"RETROFM_CURATION=FAIL reason={error}")
        return 1
    print(
        "RETROFM_CURATION=PASS "
        f"bytes={len(curated)} sha256={hashlib.sha256(curated).hexdigest()}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
