#!/usr/bin/env python3
"""RAM-backed File Transfer Base regression over esp-emu UART-TCP."""

from __future__ import annotations

import argparse
import binascii
import hashlib
import os
from pathlib import Path
import sys
import traceback

import uart_binary_data_plane_test as wire


FILE_BYTES = 128 * 1024 + 37


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--uart-log", required=True, type=Path)
    parser.add_argument(
        "--esp-emu",
        default=os.environ.get("ESP_EMU", str(Path.home() / ".local/bin/esp-emu")),
    )
    args = parser.parse_args()
    args.emulator_timeout = "180s"
    args.process_timeout = 190.0
    return args


def payload(size: int, salt: int = 0) -> bytes:
    value = bytearray(((index * 73 + (index >> 8) * 19 + salt) & 0xFF) for index in range(size))
    marker = b"\x00\x00@ESP-NP2 {\"protocol-looking\":true}\n\r\xff\x00"
    for offset in (7, 1021, 8193, max(0, size - len(marker))):
        if offset + len(marker) <= size:
            value[offset : offset + len(marker)] = marker
    return bytes(value)


def require_error(response: dict, code: str) -> None:
    if response.get("ok") is not False or response.get("error", {}).get("code") != code:
        raise AssertionError(f"expected {code}, received: {response}")


def send_request(emu: wire.Emulator, request_id: int, command: str,
                 params: dict | None = None) -> dict:
    emu.send_json(request_id, command, params)
    return emu.wait_response(request_id)


def upload(emu: wire.Emulator, request_id: int, path: str, data: bytes,
           replace: bool = False, replay_final: bool = False) -> int | None:
    result = wire.require_response(
        send_request(emu, request_id, "file.write.begin", {
            "path": path, "size_bytes": len(data), "replace": replace,
        }), request_id)
    if not data:
        if result.get("transfer_id") is not None or result.get("state") != "completed":
            raise AssertionError(f"invalid zero-length write result: {result}")
        return None
    transfer_id = int(result["transfer_id"])
    final_frame = b""
    final_ack: dict | None = None
    for sequence, offset in enumerate(range(0, len(data), wire.MAX_PAYLOAD)):
        chunk = data[offset : offset + wire.MAX_PAYLOAD]
        frame = wire.build_frame(wire.DATA, transfer_id, sequence, offset, payload=chunk)
        emu.send(frame)
        try:
            ack = emu.wait_frame()
        except AssertionError as exc:
            binary_status = send_request(emu, 900, "binary.transfer.status", {
                "transfer_id": transfer_id,
            })
            file_status = send_request(emu, 901, "file.transfer.status", {
                "transfer_id": transfer_id,
            })
            raise AssertionError(
                f"upload ACK timeout at sequence={sequence} offset={offset} length={len(chunk)} "
                f"binary={binary_status} file={file_status}"
            ) from exc
        wire.require_ack(ack, transfer_id, sequence + 1, offset + len(chunk))
        final_frame, final_ack = frame, ack
    if replay_final:
        emu.send(final_frame)
        replay = emu.wait_frame()
        wire.require_ack(replay, transfer_id, final_ack["sequence"], final_ack["offset"])
    return transfer_id


def download(emu: wire.Emulator, request_id: int, path: str,
             offset: int = 0, length: int | None = None) -> tuple[int | None, bytes]:
    params: dict[str, object] = {"path": path, "offset_bytes": offset}
    if length is not None:
        params["length_bytes"] = length
    result = wire.require_response(send_request(emu, request_id, "file.read.begin", params), request_id)
    size = int(result["size_bytes"])
    if size == 0:
        if result.get("transfer_id") is not None or result.get("state") != "completed":
            raise AssertionError(f"invalid zero-length read result: {result}")
        return None, b""
    transfer_id = int(result["transfer_id"])
    output = bytearray()
    sequence = 0
    while len(output) < size:
        frame = emu.wait_frame()
        expected_length = min(wire.MAX_PAYLOAD, size - len(output))
        if (frame["type"] != wire.DATA or frame["transfer_id"] != transfer_id or
                frame["sequence"] != sequence or frame["offset"] != len(output) or
                len(frame["payload"]) != expected_length or not frame["crc_valid"]):
            raise AssertionError(f"unexpected download frame: {frame}")
        output.extend(frame["payload"])
        sequence += 1
        emu.send(wire.build_frame(wire.ACK, transfer_id, sequence, len(output)))
    return transfer_id, bytes(output)


def require_completed(emu: wire.Emulator, request_id: int, transfer_id: int,
                      size: int, crc: int | None = None) -> None:
    binary = wire.require_response(send_request(
        emu, request_id, "binary.transfer.status", {"transfer_id": transfer_id}), request_id)
    file_status = wire.require_response(send_request(
        emu, request_id + 1, "file.transfer.status", {"transfer_id": transfer_id}), request_id + 1)
    if binary.get("state") != "completed" or binary.get("transferred_bytes") != size:
        raise AssertionError(f"binary transfer did not complete: {binary}")
    if crc is not None and binary.get("crc32") != crc:
        raise AssertionError(f"whole-transfer CRC mismatch: {binary}")
    if (file_status.get("transport_state") != "completed" or
            file_status.get("file_state") != "completed" or
            file_status.get("transferred_bytes") != size):
        raise AssertionError(f"file transfer did not complete: {file_status}")


def run(emu: wire.Emulator) -> None:
    hello = wire.require_response(send_request(emu, 1, "protocol.hello"), 1)
    if "file-transfer.v1" not in hello.get("capabilities", []):
        raise AssertionError(f"file transfer capability missing: {hello}")

    root = wire.require_response(send_request(emu, 2, "file.stat", {"path": "/"}), 2)
    if root.get("type") != "directory":
        raise AssertionError(f"root stat failed: {root}")

    names: list[str] = []
    cursor: str | None = None
    request_id = 10
    while True:
        params: dict[str, object] = {"path": "/seed", "limit": 3}
        if cursor is not None:
            params["cursor"] = cursor
        page = wire.require_response(send_request(emu, request_id, "file.list", params), request_id)
        names.extend(entry["name"] for entry in page["entries"])
        if page["done"]:
            break
        cursor = page["next_cursor"]
        request_id += 1
    expected_names = ["existing.bin"] + [f"page-{index:02d}.bin" for index in range(12)]
    if names != expected_names:
        raise AssertionError(f"pagination omitted/reordered entries: {names}")

    long_expected = [
        f"{index:02d}-abcdefghijklmnopqrstuvwxyz-ABCDEFGHIJKLMNOPQRSTUVWXYZ.bin"
        for index in range(6)
    ]
    long_names: list[str] = []
    cursor = None
    long_pages = 0
    while True:
        params = {"path": "/long", "limit": 16}
        if cursor is not None:
            params["cursor"] = cursor
        page = wire.require_response(send_request(emu, 20 + long_pages, "file.list", params),
                                     20 + long_pages)
        long_pages += 1
        long_names.extend(entry["name"] for entry in page["entries"])
        if page["done"]:
            if page["next_cursor"] is not None:
                raise AssertionError(f"final list cursor was not null: {page}")
            break
        cursor = page["next_cursor"]
    if long_pages < 2 or long_names != long_expected:
        raise AssertionError(f"byte-budget pagination failed: pages={long_pages}, names={long_names}")
    require_error(send_request(emu, 27, "file.list", {"path": "/seed/existing.bin"}),
                  "NOT_A_DIRECTORY")
    require_error(send_request(emu, 28, "file.list", {
        "path": "/seed", "cursor": "page-03.bin/extra", "limit": 3,
    }), "INVALID_PATH")

    original = payload(FILE_BYTES)
    upload_id = upload(emu, 30, "/upload/roundtrip.bin", original, replay_final=True)
    assert upload_id is not None
    require_completed(emu, 31, upload_id, len(original), binascii.crc32(original) & 0xFFFFFFFF)
    stat = wire.require_response(send_request(
        emu, 33, "file.stat", {"path": "/upload/roundtrip.bin"}), 33)
    if stat.get("size_bytes") != len(original):
        raise AssertionError(f"uploaded stat size mismatch: {stat}")
    hashed = wire.require_response(send_request(
        emu, 35, "file.sha256", {"path": "/upload/roundtrip.bin"}), 35)
    if (hashed.get("size_bytes") != len(original) or
            hashed.get("sha256") != hashlib.sha256(original).hexdigest()):
        raise AssertionError(f"uploaded file.sha256 mismatch: {hashed}")
    require_error(send_request(emu, 34, "file.write.begin", {
        "path": "/upload/roundtrip.bin", "size_bytes": 1,
    }), "ALREADY_EXISTS")

    download_id, received = download(emu, 40, "/upload/roundtrip.bin")
    assert download_id is not None
    require_completed(emu, 41, download_id, len(original), binascii.crc32(original) & 0xFFFFFFFF)
    if received != original:
        raise AssertionError("large upload/download readback differs")
    _, ranged = download(emu, 43, "/upload/roundtrip.bin", 777, 1500)
    if ranged != original[777:2277]:
        raise AssertionError("range read differs")
    require_error(send_request(emu, 44, "file.read.begin", {
        "path": "/upload/roundtrip.bin", "offset_bytes": len(original) + 1,
    }), "OUT_OF_RANGE")

    _, seed_before = download(emu, 50, "/seed/existing.bin")
    abort_result = wire.require_response(send_request(emu, 51, "file.write.begin", {
        "path": "/seed/existing.bin", "size_bytes": 2050, "replace": True,
    }), 51)
    abort_id = int(abort_result["transfer_id"])
    first = payload(1024, 7)
    emu.send(wire.build_frame(wire.DATA, abort_id, 0, 0, payload=first))
    wire.require_ack(emu.wait_frame(), abort_id, 1, 1024)
    require_error(send_request(emu, 519, "file.sha256", {"path": "/seed/existing.bin"}), "BUSY")
    require_error(send_request(emu, 520, "file.list", {"path": "/seed"}), "BUSY")
    require_error(send_request(emu, 521, "file.read.begin", {
        "path": "/seed/existing.bin", "length_bytes": 0,
    }), "BUSY")
    wire.require_response(send_request(emu, 52, "binary.transfer.abort", {
        "transfer_id": abort_id,
    }), 52)
    aborted = wire.require_response(send_request(emu, 53, "file.transfer.status", {
        "transfer_id": abort_id,
    }), 53)
    if (aborted.get("file_state") != "aborted" or
            aborted.get("error", {}).get("code") != "TRANSFER_ABORTED"):
        raise AssertionError(f"aborted write state mismatch: {aborted}")
    _, seed_after = download(emu, 54, "/seed/existing.bin")
    if seed_after != seed_before:
        raise AssertionError("aborted replacement changed committed bytes")

    replacement = payload(211, 23)
    replace_id = upload(emu, 55, "/seed/existing.bin", replacement, replace=True)
    assert replace_id is not None
    require_completed(emu, 56, replace_id, len(replacement), binascii.crc32(replacement) & 0xFFFFFFFF)
    _, replaced = download(emu, 58, "/seed/existing.bin")
    if replaced != replacement:
        raise AssertionError("successful replacement readback differs")

    upload(emu, 60, "/upload/empty.bin", b"")
    empty_stat = wire.require_response(send_request(
        emu, 601, "file.stat", {"path": "/upload/empty.bin"}), 601)
    if empty_stat.get("size_bytes") != 0:
        raise AssertionError(f"zero-length stat mismatch: {empty_stat}")
    empty_hash = wire.require_response(send_request(
        emu, 602, "file.sha256", {"path": "/upload/empty.bin"}), 602)
    if (empty_hash.get("size_bytes") != 0 or
            empty_hash.get("sha256") != hashlib.sha256(b"").hexdigest()):
        raise AssertionError(f"zero-length file.sha256 mismatch: {empty_hash}")
    _, empty = download(emu, 61, "/upload/empty.bin")
    if empty:
        raise AssertionError("zero-length file produced bytes")
    _, eof = download(emu, 62, "/seed/existing.bin", len(replacement), 0)
    if eof:
        raise AssertionError("zero-length EOF range produced bytes")

    require_error(send_request(emu, 70, "file.stat", {"path": "../../escape"}), "INVALID_PATH")
    require_error(send_request(emu, 71, "file.stat", {"path": "/upload/../escape"}), "INVALID_PATH")
    require_error(send_request(emu, 72, "file.stat", {"path": "/missing.bin"}), "NOT_FOUND")
    require_error(send_request(emu, 730, "file.sha256", {"path": "/missing.bin"}), "NOT_FOUND")
    require_error(send_request(emu, 73, "file.stat", {"path": "/" + "a" * 193}), "INVALID_PATH")
    require_error(send_request(emu, 7301, "file.sha256", {"path": "/upload/../escape"}), "INVALID_PATH")
    require_error(send_request(emu, 731, "file.stat", {"path": "/upload//bad"}), "INVALID_PATH")
    require_error(send_request(emu, 732, "file.stat", {"path": "/upload/bad/"}), "INVALID_PATH")
    require_error(send_request(emu, 733, "file.stat", {"path": "/upload/" + "b" * 65}),
                  "INVALID_PATH")
    require_error(send_request(emu, 734, "file.read.begin", {"path": "/seed"}), "NOT_A_FILE")
    require_error(send_request(emu, 735, "file.write.begin", {
        "path": "/absent/file.bin", "size_bytes": 1,
    }), "PARENT_NOT_FOUND")
    require_error(send_request(emu, 736, "file.write.begin", {
        "path": "/upload/too-large.bin", "size_bytes": 192 * 1024 + 1,
    }), "NO_SPACE")

    emu.send(b'@ESP-NP2 {"v":1,"id":737,"cmd":"file.stat","params":{"path":"/upload/\xff.bin"}}\n')
    require_error(emu.wait_response(737), "INVALID_PATH")

    japanese = payload(53, 91)
    japanese_id = upload(emu, 74, "/upload/日本語.bin", japanese)
    assert japanese_id is not None
    require_completed(emu, 75, japanese_id, len(japanese), binascii.crc32(japanese) & 0xFFFFFFFF)
    _, japanese_back = download(emu, 77, "/upload/日本語.bin")
    if japanese_back != japanese:
        raise AssertionError("UTF-8 filename readback differs")
    upload_page = wire.require_response(send_request(
        emu, 78, "file.list", {"path": "/upload", "limit": 16}), 78)
    if "日本語.bin" not in [entry["name"] for entry in upload_page["entries"]]:
        raise AssertionError(f"UTF-8 filename missing from listing: {upload_page}")

    ping = wire.require_response(send_request(emu, 90, "system.ping"), 90)
    if ping != {"pong": True}:
        raise AssertionError(f"final ping failed: {ping}")


def main() -> int:
    args = parse_args()
    emu = wire.Emulator(args)
    error: BaseException | None = None
    exit_status: int | None = None
    try:
        emu.start()
        emu.wait_line(wire.READY_MARKER, timeout=10.0)
        run(emu)
        exit_status = emu.finish()
    except BaseException as exc:
        error = exc
    finally:
        if emu.process is not None and emu.process.poll() is None:
            emu.finish()
        process_output = emu.diagnostics()
        args.uart_log.parent.mkdir(parents=True, exist_ok=True)
        args.uart_log.write_bytes(bytes(emu.raw))
        args.log.parent.mkdir(parents=True, exist_ok=True)
        args.log.write_text(
            "UART bytes (hex):\n" + " ".join(f"{byte:02x}" for byte in emu.raw) +
            "\n\nesp-emu output:\n" + process_output.decode("utf-8", errors="replace"),
            encoding="utf-8",
        )
    if error is not None:
        traceback.print_exception(error)
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    if exit_status not in (0, -15, -9):
        print(f"ERROR: esp-emu exit status was {exit_status}", file=sys.stderr)
        return 1
    print("PASS: FILE TRANSFER BASE RAM ROUND TRIP OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
