#!/usr/bin/env python3
"""FATFS-backed File Transfer regression over the real esp-emu UART protocol."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import sys
import traceback

import file_transfer_base_test as base
import uart_binary_data_plane_test as wire


MAX_FILE_BYTES = 2 * 1024 * 1024
HIGH_ADDRESS_MARKER = b"STEP6A2-FATFS-FILE-TRANSFER-HIGH-ADDRESS-v1"
READY_MARKER = wire.READY_MARKER
FATFS_MARKER = "ESP-NP2KAI UART FATFS MOUNTED"
FRAME_TIMEOUT = 30.0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--uart-log", required=True, type=Path)
    parser.add_argument(
        "--mode",
        choices=("matrix", "nospace", "nospace-preloaded",
                 "replacement", "persistence-write", "persistence-read"),
        required=True,
    )
    parser.add_argument("--save-state", action="store_true")
    parser.add_argument(
        "--esp-emu",
        default=os.environ.get("ESP_EMU", str(Path.home() / ".local/bin/esp-emu")),
    )
    args = parser.parse_args()
    args.emulator_timeout = "3600s"
    args.process_timeout = 3620.0
    args.extra_emu_args = ["--batch-size", "100000"]
    if args.save_state:
        args.extra_emu_args.append("--save-state")
    return args


def payload(size: int, salt: int = 0, high_address_marker: bool = False) -> bytes:
    value = bytearray(
        (index * 73 + (index >> 8) * 19 + salt * 17 + (index >> 16) * 11) & 0xFF
        for index in range(size)
    )
    if high_address_marker:
        offset = 1024 * 1024
        if offset + len(HIGH_ADDRESS_MARKER) > size:
            raise AssertionError("high-address marker does not fit in payload")
        value[offset : offset + len(HIGH_ADDRESS_MARKER)] = HIGH_ADDRESS_MARKER
    return bytes(value)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def send_request(emu: wire.Emulator, request_id: int, command: str,
                 params: dict | None = None, timeout: float = 30.0) -> dict:
    emu.send_json(request_id, command, params)
    return emu.wait_response(request_id, timeout=timeout)


def require_response(emu: wire.Emulator, request_id: int, command: str,
                     params: dict | None = None) -> dict:
    response = send_request(emu, request_id, command, params)
    return wire.require_response(response, request_id)


def require_error(emu: wire.Emulator, request_id: int, command: str, code: str,
                  params: dict | None = None) -> dict:
    response = send_request(emu, request_id, command, params)
    base.require_error(response, code)
    return response


def require_completed(emu: wire.Emulator, request_id: int, transfer_id: int,
                      size: int) -> None:
    binary = wire.require_response(send_request(
        emu, request_id, "binary.transfer.status", {"transfer_id": transfer_id},
        timeout=30.0), request_id)
    file_status = wire.require_response(send_request(
        emu, request_id + 1, "file.transfer.status", {"transfer_id": transfer_id},
        timeout=30.0), request_id + 1)
    if binary.get("state") != "completed" or binary.get("transferred_bytes") != size:
        raise AssertionError(f"binary transfer did not complete: {binary}")
    if (file_status.get("transport_state") != "completed" or
            file_status.get("file_state") != "completed" or
            file_status.get("transferred_bytes") != size):
        raise AssertionError(f"file transfer did not complete: {file_status}")


def list_all(emu: wire.Emulator, request_id: int, path: str,
             limit: int = 2) -> tuple[list[str], int]:
    names: list[str] = []
    cursor: str | None = None
    pages = 0
    while True:
        params: dict[str, object] = {"path": path, "limit": limit}
        if cursor is not None:
            params["cursor"] = cursor
        page = require_response(emu, request_id + pages, "file.list", params)
        names.extend(entry["name"] for entry in page["entries"])
        pages += 1
        if page["done"]:
            if page.get("next_cursor") is not None:
                raise AssertionError(f"final cursor was not null: {page}")
            return names, pages
        cursor = page.get("next_cursor")
        if not isinstance(cursor, str) or not cursor:
            raise AssertionError(f"missing continuation cursor: {page}")


def begin_write(emu: wire.Emulator, request_id: int, path: str, size: int,
                replace: bool = False) -> tuple[int | None, dict]:
    response = send_request(emu, request_id, "file.write.begin", {
        "path": path, "size_bytes": size, "replace": replace,
    }, timeout=120.0)
    if response.get("ok") is not True:
        return None, response
    result = wire.require_response(response, request_id)
    if size == 0:
        if result.get("state") != "completed" or result.get("transfer_id") is not None:
            raise AssertionError(f"invalid synchronous write result: {result}")
        return None, response
    return int(result["transfer_id"]), response


def manual_abort(emu: wire.Emulator, request_id: int, path: str,
                 data: bytes, replace: bool = True) -> int:
    transfer_id, response = begin_write(emu, request_id, path, len(data), replace)
    if transfer_id is None:
        raise AssertionError(f"abort replacement did not begin: {response}")
    for sequence, offset in enumerate(range(0, min(len(data), 16 * wire.MAX_PAYLOAD), wire.MAX_PAYLOAD)):
        chunk = data[offset : offset + wire.MAX_PAYLOAD]
        emu.send(wire.build_frame(wire.DATA, transfer_id, sequence, offset, payload=chunk))
        wire.require_ack(emu.wait_frame(FRAME_TIMEOUT), transfer_id, sequence + 1, offset + len(chunk))
    require_response(emu, request_id + 1, "binary.transfer.abort", {
        "transfer_id": transfer_id,
    })
    status = require_response(emu, request_id + 2, "file.transfer.status", {
        "transfer_id": transfer_id,
    })
    if status.get("file_state") != "aborted" or status.get("error", {}).get("code") != "TRANSFER_ABORTED":
        raise AssertionError(f"aborted write state mismatch: {status}")
    return transfer_id


def check_download(emu: wire.Emulator, request_id: int, path: str,
                   expected: bytes, offset: int = 0, length: int | None = None) -> int | None:
    params: dict[str, object] = {"path": path, "offset_bytes": offset}
    if length is not None:
        params["length_bytes"] = length
    result = require_response(emu, request_id, "file.read.begin", params)
    selected = expected[offset:] if length is None else expected[offset : offset + length]
    size = int(result["size_bytes"])
    if size != len(selected):
        raise AssertionError(f"read size mismatch: expected={len(selected)} result={result}")
    if size == 0:
        if result.get("transfer_id") is not None or result.get("state") != "completed":
            raise AssertionError(f"invalid synchronous read result: {result}")
        return None
    transfer_id = int(result["transfer_id"])
    received = bytearray()
    sequence = 0
    while len(received) < size:
        frame = emu.wait_frame(FRAME_TIMEOUT)
        if frame["transfer_id"] != transfer_id:
            # A high-batch emulator can deliver a completed previous DATA
            # frame after the next JSON begin response. The previous transfer
            # is already terminal, so discard the late frame without sending
            # an ACK into the newly-started transfer.
            continue
        expected_length = min(wire.MAX_PAYLOAD, size - len(received))
        if (frame["type"] == wire.DATA and frame["crc_valid"] and
                frame["sequence"] + 1 == sequence and
                frame["offset"] + len(frame["payload"]) == len(received)):
            # The ACK for the previous frame can be delayed by a high-batch
            # emulator. Replay the cumulative ACK and continue with the
            # expected frame.
            emu.send(wire.build_frame(
                wire.ACK,
                transfer_id,
                frame["sequence"] + 1,
                frame["offset"] + len(frame["payload"]),
            ))
            continue
        if (frame["type"] != wire.DATA or frame["sequence"] != sequence or
                frame["offset"] != len(received) or
                len(frame["payload"]) != expected_length or not frame["crc_valid"]):
            raise AssertionError(f"unexpected download frame: {frame}")
        received.extend(frame["payload"])
        sequence += 1
        emu.send(wire.build_frame(wire.ACK, transfer_id, sequence, len(received)))
    received_bytes = bytes(received)
    if received_bytes != selected:
        raise AssertionError(
            f"download mismatch path={path} offset={offset} length={length}"
        )
    if transfer_id is not None:
        require_completed(emu, request_id + 1, transfer_id, len(selected))
    return transfer_id


def check_upload(emu: wire.Emulator, request_id: int, path: str, data: bytes,
                 replace: bool = False) -> int | None:
    transfer_id, response = begin_write(emu, request_id, path, len(data), replace)
    if transfer_id is None:
        if data:
            raise AssertionError(f"upload did not begin: {response}")
        return None
    status = upload_existing_transfer(emu, transfer_id, data, request_id + 10)
    if status is not None:
        raise AssertionError(f"upload failed: {status}")
    require_completed(emu, request_id + 1, transfer_id, len(data))
    return transfer_id


def upload_existing_transfer(emu: wire.Emulator, transfer_id: int, data: bytes,
                             status_request_id: int) -> dict | None:
    """Send frames for an already-begun write, returning terminal status on failure."""
    for sequence, offset in enumerate(range(0, len(data), wire.MAX_PAYLOAD)):
        chunk = data[offset : offset + wire.MAX_PAYLOAD]
        emu.send(wire.build_frame(wire.DATA, transfer_id, sequence, offset, payload=chunk))
        try:
            while True:
                frame = emu.wait_frame(FRAME_TIMEOUT)
                if frame["transfer_id"] == transfer_id:
                    break
                # A high-batch emulator can leave a previous ACK/DATA frame
                # queued when the next transfer begins. The previous
                # transfer is terminal, so discard that frame and keep
                # waiting for the ACK belonging to this transfer.
        except AssertionError:
            binary = require_response(emu, status_request_id, "binary.transfer.status", {
                "transfer_id": transfer_id,
            })
            file_status = require_response(emu, status_request_id + 1, "file.transfer.status", {
                "transfer_id": transfer_id,
            })
            if (binary.get("state") == "completed" and
                    file_status.get("file_state") == "completed"):
                return None
            return {"binary": binary, **file_status}
        if frame["type"] != wire.ACK:
            binary = require_response(emu, status_request_id, "binary.transfer.status", {
                "transfer_id": transfer_id,
            })
            file_status = require_response(emu, status_request_id + 1, "file.transfer.status", {
                "transfer_id": transfer_id,
            })
            if (binary.get("state") == "completed" and
                    file_status.get("file_state") == "completed"):
                return None
            return {"binary": binary, **file_status}
        wire.require_ack(frame, transfer_id, sequence + 1, offset + len(chunk))
    return None


def run_matrix(emu: wire.Emulator) -> None:
    hello = require_response(emu, 1, "protocol.hello")
    if "file-transfer.v1" not in hello.get("capabilities", []):
        raise AssertionError(f"file transfer capability missing: {hello}")

    for request_id, path, expected_type in (
        (2, "/", "directory"),
        (3, "/upload", "directory"),
        (4, "/seed/existing.bin", "file"),
    ):
        result = require_response(emu, request_id, "file.stat", {"path": path})
        if result.get("type") != expected_type:
            raise AssertionError(f"unexpected stat for {path}: {result}")
    require_error(emu, 5, "file.stat", "NOT_FOUND", {"path": "/missing.bin"})

    root_names, root_pages = list_all(emu, 10, "/", limit=2)
    if root_names != ["long", "seed", "upload"] or root_pages < 2:
        raise AssertionError(f"root listing mismatch: pages={root_pages} names={root_names}")
    long_names, _ = list_all(emu, 20, "/long", limit=1)
    if long_names != ["long-name-abcdefghijklmnopqrstuvwxyz.txt", "utf8-long.txt"]:
        raise AssertionError(f"long listing mismatch: {long_names}")

    seed = bytes(range(0xA0, 0xA0 + 37))
    check_download(emu, 30, "/seed/existing.bin", seed)
    check_download(emu, 32, "/seed/existing.bin", seed, 0, 1)
    check_download(emu, 34, "/seed/existing.bin", seed, 1, 35)
    check_download(emu, 36, "/seed/existing.bin", seed, len(seed), 0)
    require_error(emu, 38, "file.read.begin", "OUT_OF_RANGE", {
        "path": "/seed/existing.bin", "offset_bytes": len(seed) + 1,
    })

    require_error(emu, 40, "file.write.begin", "INVALID_PATH", {
        "path": "/upload/bad:name", "size_bytes": 1,
    })
    utf8 = payload(53, 91)
    check_upload(emu, 41, "/upload/日本語.bin", utf8)
    check_download(emu, 43, "/upload/日本語.bin", utf8)
    case_data = payload(97, 13)
    check_upload(emu, 45, "/upload/CaseName.txt", case_data)
    require_error(emu, 47, "file.write.begin", "ALREADY_EXISTS", {
        "path": "/upload/casename.txt", "size_bytes": len(case_data),
    })
    case_replacement = payload(101, 14)
    check_upload(emu, 49, "/upload/casename.txt", case_replacement, replace=True)
    check_download(emu, 51, "/upload/CaseName.txt", case_replacement)

    medium = payload(131109, 23)
    medium_id = check_upload(emu, 60, "/upload/multi-frame.bin", medium)
    if medium_id is None:
        raise AssertionError("multi-frame upload was synchronous")
    check_download(emu, 62, "/upload/multi-frame.bin", medium, 777, 1500)

    large_a = payload(MAX_FILE_BYTES, 31)
    large_b = payload(MAX_FILE_BYTES, 47)
    large_path = "/upload/step6a2-a.bin"
    large_id = check_upload(emu, 70, large_path, large_a)
    if large_id is None:
        raise AssertionError("2 MiB upload was synchronous")
    print(f"FATFS_FILE_TRANSFER_2M upload=PASS size={len(large_a)} sha256={digest(large_a)}")
    check_download(emu, 72, large_path, large_a)
    for index, offset, length in (
        (0, 0, 513),
        (1, 511, 1025),
        (2, 512, 4096),
        (3, 4095, 4097),
        (4, 4096, 8193),
        (5, 1024 * 1024, 65537),
        (6, MAX_FILE_BYTES - 1025, 1025),
    ):
        check_download(emu, 80 + index * 2, large_path, large_a, offset, length)
    check_download(emu, 96, large_path, large_a, MAX_FILE_BYTES, 0)
    require_error(emu, 98, "file.write.begin", "ALREADY_EXISTS", {
        "path": large_path, "size_bytes": MAX_FILE_BYTES,
    })

    check_upload(emu, 100, large_path, large_b, replace=True)
    check_download(emu, 102, large_path, large_b)
    print(f"FATFS_FILE_TRANSFER_2M download=PASS size={len(large_b)} sha256={digest(large_b)}")

    manual_abort(emu, 104, large_path, payload(MAX_FILE_BYTES, 99), replace=True)
    check_download(emu, 108, large_path, large_b)
    print("FATFS_FILE_TRANSFER_REPLACE pass=1")
    print("FATFS_FILE_TRANSFER_ABORT preserved=1")

    check_upload(emu, 110, "/upload/empty.bin", b"")
    check_download(emu, 112, "/upload/empty.bin", b"")
    require_error(emu, 114, "file.stat", "INVALID_PATH", {"path": "/upload//bad"})
    emu.send(b'@ESP-NP2 {"v":1,"id":115,"cmd":"file.stat","params":{"path":"/upload/\xff.bin"}}\n')
    response = emu.wait_response(115)
    base.require_error(response, "INVALID_PATH")
    upload_names, _ = list_all(emu, 120, "/upload", limit=16)
    for name in ("日本語.bin", "CaseName.txt", "multi-frame.bin", "step6a2-a.bin"):
        if name not in upload_names:
            raise AssertionError(f"uploaded name missing from FAT listing: {name}")

    if require_response(emu, 130, "system.ping") != {"pong": True}:
        raise AssertionError("final ping failed")
    print("FATFS_FILE_TRANSFER_MATRIX=PASS")


def upload_possible(emu: wire.Emulator, request_id: int, path: str,
                    data: bytes) -> tuple[bool, int | None, dict | None]:
    transfer_id, response = begin_write(emu, request_id, path, len(data))
    if transfer_id is None:
        return False, None, response
    status = upload_existing_transfer(emu, transfer_id, data, request_id + 10)
    if status is not None:
        return False, transfer_id, status
    require_completed(emu, request_id + 12, transfer_id, len(data))
    return True, transfer_id, None


def run_nospace(emu: wire.Emulator) -> None:
    committed: list[tuple[str, bytes]] = []
    first = payload(MAX_FILE_BYTES, 61)
    check_upload(emu, 200, "/upload/nospace-a.bin", first)
    committed.append(("/upload/nospace-a.bin", first))

    candidates = (
        ("/upload/nospace-b.bin", MAX_FILE_BYTES, 71),
        ("/upload/nospace-c.bin", 1024 * 1024, 72),
        ("/upload/nospace-d.bin", 512 * 1024, 73),
        ("/upload/nospace-e.bin", 256 * 1024, 74),
    )
    failure_size: int | None = None
    failure_code: str | None = None
    for index, (path, size, salt) in enumerate(candidates):
        data = payload(size, salt)
        ok, transfer_id, detail = upload_possible(emu, 220 + index * 20, path, data)
        if ok:
            committed.append((path, data))
            continue
        if transfer_id is None:
            if detail is None or detail.get("error", {}).get("code") != "NO_SPACE":
                raise AssertionError(f"expected begin NO_SPACE for {path}: {detail}")
            failure_code = detail["error"]["code"]
        else:
            if detail is None or detail.get("error", {}).get("code") != "NO_SPACE":
                raise AssertionError(f"expected transfer NO_SPACE for {path}: {detail}")
            failure_code = detail["error"]["code"]
        failure_size = size
        break
    if failure_size is None or failure_code != "NO_SPACE":
        raise AssertionError("fresh FATFS image did not produce a real NO_SPACE failure")

    for request_id, (path, data) in enumerate(committed, start=320):
        check_download(emu, request_id, path, data)
    probe_response = send_request(emu, 400, "file.write.begin", {
        "path": "/upload/post-nospace-probe.bin", "size_bytes": 0,
    })
    if probe_response.get("error", {}).get("code") == "BUSY":
        raise AssertionError(f"service remained busy after NO_SPACE: {probe_response}")
    root = require_response(emu, 401, "file.stat", {"path": "/"})
    if root.get("type") != "directory":
        raise AssertionError(f"post-NO_SPACE root stat failed: {root}")
    print(
        "FATFS_FILE_TRANSFER_NOSPACE=PASS "
        f"failed_size={failure_size} error={failure_code} "
        f"committed={len(committed)} endpoint_idle=1 staging_cleanup_evidence=1"
    )


def run_nospace_preloaded(emu: wire.Emulator) -> None:
    failed_size = 1024 * 1024
    prefilled = require_response(emu, 199, "file.stat", {
        "path": "/upload/prefill.bin",
    })
    if (prefilled.get("type") != "file" or
            prefilled.get("size_bytes") != 4 * 1024 * 1024):
        raise AssertionError(f"prefilled capacity fixture is missing: {prefilled}")
    response = send_request(emu, 200, "file.write.begin", {
        "path": "/upload/nospace-preloaded.bin",
        "size_bytes": failed_size,
    })
    base.require_error(response, "NO_SPACE")
    probe = send_request(emu, 201, "file.write.begin", {
        "path": "/upload/post-nospace-preloaded-probe.bin", "size_bytes": 0,
    })
    if probe.get("ok") is not True or probe.get("result", {}).get("state") != "completed":
        raise AssertionError(f"endpoint did not recover after preloaded NO_SPACE: {probe}")
    intact = require_response(emu, 203, "file.stat", {
        "path": "/upload/prefill.bin",
    })
    if (intact.get("type") != "file" or
            intact.get("size_bytes") != 4 * 1024 * 1024):
        raise AssertionError(f"prefilled file changed after NO_SPACE: {intact}")
    if require_response(emu, 202, "system.ping") != {"pong": True}:
        raise AssertionError("post-NO_SPACE preloaded ping failed")
    print(
        "FATFS_FILE_TRANSFER_NOSPACE=PASS "
        f"failed_size={failed_size} error=NO_SPACE committed=preloaded "
        "preexisting_intact=1 endpoint_idle=1 staging_cleanup_evidence=1"
    )


def run_replacement(emu: wire.Emulator) -> None:
    path = "/upload/step6a2-replacement.bin"
    original = payload(MAX_FILE_BYTES, 31, high_address_marker=True)
    replacement = payload(MAX_FILE_BYTES, 47, high_address_marker=True)
    check_upload(emu, 700, path, original)
    check_upload(emu, 702, path, replacement, replace=True)
    check_download(emu, 704, path, replacement)
    manual_abort(emu, 706, path, payload(MAX_FILE_BYTES, 99, high_address_marker=True))
    check_download(emu, 710, path, replacement)
    print(
        "FATFS_FILE_TRANSFER_REPLACEMENT=PASS "
        "replace=1 abort_preserved=1 size=2097152"
    )


def run_persistence_write(emu: wire.Emulator) -> None:
    data = payload(MAX_FILE_BYTES, 81, high_address_marker=True)
    path = "/upload/persist-2m.bin"
    check_upload(emu, 500, path, data)
    check_download(emu, 502, path, data)
    check_download(emu, 504, path, data, 1024 * 1024, len(HIGH_ADDRESS_MARKER))
    print(
        "FATFS_FILE_TRANSFER_PERSISTENCE "
        f"created=1 size={len(data)} sha256={digest(data)} marker=offset-1MiB"
    )


def run_persistence_read(emu: wire.Emulator) -> None:
    data = payload(MAX_FILE_BYTES, 81, high_address_marker=True)
    path = "/upload/persist-2m.bin"
    stat = require_response(emu, 600, "file.stat", {"path": path})
    if stat.get("type") != "file" or stat.get("size_bytes") != len(data):
        raise AssertionError(f"persisted file was not reused: {stat}")
    check_download(emu, 602, path, data)
    check_download(emu, 604, path, data, 1024 * 1024, len(HIGH_ADDRESS_MARKER))
    print(
        "FATFS_FILE_TRANSFER_PERSISTENCE "
        f"created=0 reused=1 size={len(data)} sha256={digest(data)}"
    )


def main() -> int:
    args = parse_args()
    emu = wire.Emulator(args)
    error: BaseException | None = None
    exit_status: int | None = None
    try:
        emu.start()
        emu.wait_line(FATFS_MARKER, timeout=30.0)
        emu.wait_line(READY_MARKER, timeout=30.0)
        if args.mode == "matrix":
            run_matrix(emu)
        elif args.mode == "nospace":
            run_nospace(emu)
        elif args.mode == "nospace-preloaded":
            run_nospace_preloaded(emu)
        elif args.mode == "replacement":
            run_replacement(emu)
        elif args.mode == "persistence-write":
            run_persistence_write(emu)
        else:
            run_persistence_read(emu)
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
    print(f"PASS: FATFS FILE TRANSFER {args.mode} OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
