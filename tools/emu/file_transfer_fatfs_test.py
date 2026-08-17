#!/usr/bin/env python3
"""FATFS-backed File Transfer regression over the real esp-emu UART protocol."""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import hashlib
import os
from pathlib import Path
import sys
import time
import traceback

import file_transfer_base_test as base
import uart_binary_data_plane_test as wire


MAX_FILE_BYTES = 2 * 1024 * 1024
LARGE_FILE_BYTES = 512 * 1024
REPLACEMENT_FILE_BYTES = 256 * 1024
PERSISTENCE_FILE_BYTES = 256 * 1024
HIGH_ADDRESS_FILE_BYTES = 64 * 1024
HIGH_ADDRESS_MARKER = b"STEP6A2-FATFS-FILE-TRANSFER-HIGH-ADDRESS-v1"
HIGH_ADDRESS_OFFSET = 1024 * 1024
READY_MARKER = wire.READY_MARKER
FATFS_MARKER = "ESP-NP2KAI UART FATFS MOUNTED"
FRAME_TIMEOUT = 30.0
PROGRESS_WATCHDOG_TIMEOUT = 30.0
PROGRESS_INTERVAL_BYTES = 64 * 1024
WATCHDOG_POLL_INTERVAL = 0.25
UART_TEST_BATCH_SIZE = 5000


@dataclass
class TransferMetrics:
    """Host-side transfer telemetry and acknowledged-progress watchdog."""

    phase: str
    direction: str
    expected_bytes: int
    started: float = field(default_factory=time.monotonic)
    last_progress: float = field(default_factory=time.monotonic)
    progress_bytes: int = 0
    frames: int = 0
    ack_cycles: int = 0
    retries: int = 0
    nacks: int = 0
    duplicate_frames: int = 0
    protocol_timeouts: int = 0
    max_progress_gap: float = 0.0
    next_report_bytes: int = PROGRESS_INTERVAL_BYTES
    finished: bool = False

    def progress(self, transferred_bytes: int, frame_count: int) -> None:
        now = time.monotonic()
        gap = now - self.last_progress
        self.max_progress_gap = max(self.max_progress_gap, gap)
        self.last_progress = now
        self.progress_bytes = transferred_bytes
        self.frames = max(self.frames, frame_count)
        if (transferred_bytes >= self.next_report_bytes or
                transferred_bytes >= self.expected_bytes):
            elapsed = now - self.started
            print(
                "FATFS_PROGRESS "
                f"phase={self.phase} direction={self.direction} "
                f"bytes={transferred_bytes}/{self.expected_bytes} "
                f"frames={self.frames} elapsed={elapsed:.3f}s"
            )
            while self.next_report_bytes <= transferred_bytes:
                self.next_report_bytes += PROGRESS_INTERVAL_BYTES

    def wait_frame(self, emu: wire.Emulator) -> dict:
        deadline = time.monotonic() + FRAME_TIMEOUT
        while True:
            now = time.monotonic()
            progress_gap = now - self.last_progress
            if progress_gap > PROGRESS_WATCHDOG_TIMEOUT:
                self.protocol_timeouts += 1
                raise AssertionError(
                    "payload progress watchdog expired: "
                    f"phase={self.phase} direction={self.direction} "
                    f"progress={self.progress_bytes}/{self.expected_bytes} "
                    f"gap={progress_gap:.3f}s threshold={PROGRESS_WATCHDOG_TIMEOUT:.1f}s"
                )
            remaining = deadline - now
            if remaining <= 0:
                self.protocol_timeouts += 1
                raise AssertionError(
                    "binary frame timeout: "
                    f"phase={self.phase} direction={self.direction} "
                    f"progress={self.progress_bytes}/{self.expected_bytes}"
                )
            try:
                return emu.wait_frame(min(WATCHDOG_POLL_INTERVAL, remaining))
            except AssertionError:
                continue

    def complete(self) -> None:
        if self.finished:
            return
        elapsed = time.monotonic() - self.started
        rate = (self.progress_bytes / 1024.0 / elapsed) if elapsed else 0.0
        print(
            "FATFS_TRANSFER_SUMMARY "
            f"phase={self.phase} direction={self.direction} "
            f"bytes={self.progress_bytes}/{self.expected_bytes} "
            f"frames={self.frames} ack_cycles={self.ack_cycles} "
            f"elapsed={elapsed:.3f}s kib_s={rate:.3f} "
            f"retries={self.retries} nacks={self.nacks} "
            f"duplicates={self.duplicate_frames} "
            f"protocol_timeouts={self.protocol_timeouts} "
            f"max_progress_gap={self.max_progress_gap:.3f}s"
        )
        self.finished = True


class ProgressObserver:
    def __init__(self, mode: str) -> None:
        self.mode = mode
        self.started = time.monotonic()
        self.transfers: list[TransferMetrics] = []

    def phase_start(self, name: str, expected_bytes: int = 0) -> None:
        print(
            "FATFS_PHASE_START "
            f"mode={self.mode} phase={name} expected_bytes={expected_bytes}"
        )

    def phase_complete(self, name: str, transferred_bytes: int = 0,
                       frame_count: int = 0) -> None:
        elapsed = time.monotonic() - self.started
        print(
            "FATFS_PHASE_COMPLETE "
            f"mode={self.mode} phase={name} bytes={transferred_bytes} "
            f"frames={frame_count} elapsed={elapsed:.3f}s"
        )

    def transfer(self, phase: str, direction: str, expected_bytes: int) -> TransferMetrics:
        metric = TransferMetrics(phase, direction, expected_bytes)
        self.transfers.append(metric)
        return metric

    def summary(self, result: str = "PASS") -> None:
        elapsed = time.monotonic() - self.started
        upload_bytes = sum(
            item.progress_bytes for item in self.transfers if item.direction == "upload"
        )
        download_bytes = sum(
            item.progress_bytes for item in self.transfers if item.direction == "download"
        )
        retries = sum(item.retries for item in self.transfers)
        nacks = sum(item.nacks for item in self.transfers)
        duplicates = sum(item.duplicate_frames for item in self.transfers)
        timeouts = sum(item.protocol_timeouts for item in self.transfers)
        print(
            "FATFS_MODE_SUMMARY "
            f"mode={self.mode} upload_bytes={upload_bytes} "
            f"download_bytes={download_bytes} elapsed={elapsed:.3f}s "
            f"retries={retries} nacks={nacks} duplicates={duplicates} "
            f"protocol_timeouts={timeouts} result={result}"
        )


ACTIVE_OBSERVER: ProgressObserver | None = None


def new_transfer(phase: str, direction: str, expected_bytes: int) -> TransferMetrics | None:
    if ACTIVE_OBSERVER is None:
        return None
    return ACTIVE_OBSERVER.transfer(phase, direction, expected_bytes)


def wait_transfer_frame(emu: wire.Emulator, metric: TransferMetrics | None,
                        transfer_id: int) -> dict:
    while True:
        frame = metric.wait_frame(emu) if metric is not None else emu.wait_frame(FRAME_TIMEOUT)
        if frame["transfer_id"] == transfer_id:
            return frame


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--uart-log", required=True, type=Path)
    parser.add_argument(
        "--mode",
        choices=("basic", "large", "matrix", "nospace", "nospace-preloaded",
                 "high-address-write",
                 "replacement", "persistence-write", "persistence-read"),
        required=True,
    )
    parser.add_argument(
        "--large-size",
        type=int,
        default=LARGE_FILE_BYTES,
        help="large-mode payload size in bytes (default: 524288; 2 MiB is extended-only)",
    )
    parser.add_argument(
        "--replacement-size",
        type=int,
        default=REPLACEMENT_FILE_BYTES,
        help="replacement-mode payload size in bytes (default: 262144)",
    )
    parser.add_argument(
        "--persistence-size",
        type=int,
        default=PERSISTENCE_FILE_BYTES,
        help="persistence payload size in bytes (default: 262144)",
    )
    parser.add_argument("--save-state", action="store_true")
    parser.add_argument("--verify-state", type=Path)
    parser.add_argument("--marker", default=HIGH_ADDRESS_MARKER.decode("ascii"))
    parser.add_argument(
        "--minimum-offset",
        type=lambda value: int(value, 0),
        default=0x400000,
        help="minimum physical marker offset for high-address-write",
    )
    parser.add_argument(
        "--esp-emu",
        default=os.environ.get("ESP_EMU", str(Path.home() / ".local/bin/esp-emu")),
    )
    args = parser.parse_args()
    for option_name in ("large_size", "replacement_size", "persistence_size"):
        value = getattr(args, option_name)
        if value <= 0 or value > MAX_FILE_BYTES:
            parser.error(f"--{option_name.replace('_', '-')} must be in the range 1..{MAX_FILE_BYTES}")
    if args.mode == "large" and args.large_size != LARGE_FILE_BYTES:
        print(
            "WARNING: --large-size overrides the routine 512 KiB large-mode workload; "
            "use only for extended/manual validation",
            file=sys.stderr,
        )
    if args.mode == "persistence-write" and not args.save_state:
        parser.error("--save-state is required for persistence-write")
    if args.mode == "persistence-read" and args.save_state:
        parser.error("--save-state is only valid for persistence-write")
    if args.mode == "high-address-write":
        if not args.save_state:
            parser.error("--save-state is required for high-address-write")
        if args.verify_state is None:
            args.verify_state = args.firmware
    elif args.verify_state is not None:
        parser.error("--verify-state is only valid for high-address-write")
    bounded_mode = args.mode not in ("matrix", "nospace")
    args.emulator_timeout = "600s" if bounded_mode else "3600s"
    args.process_timeout = 620.0 if bounded_mode else 3620.0
    args.extra_emu_args = ["--batch-size", str(UART_TEST_BATCH_SIZE)]
    if args.save_state:
        args.extra_emu_args.append("--save-state")
    return args


def payload(size: int, salt: int = 0, high_address_marker: bool = False,
            marker_offset: int | None = None) -> bytes:
    value = bytearray(
        (index * 73 + (index >> 8) * 19 + salt * 17 + (index >> 16) * 11) & 0xFF
        for index in range(size)
    )
    if high_address_marker:
        offset = HIGH_ADDRESS_OFFSET if marker_offset is None else marker_offset
        if offset < 0 or offset + len(HIGH_ADDRESS_MARKER) > size:
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


def require_staging_namespace_hidden(emu: wire.Emulator, request_id: int) -> None:
    response = send_request(emu, request_id, "file.stat", {"path": "/.np2-staging"})
    if response.get("ok") is not True:
        if response.get("error", {}).get("code") == "NOT_FOUND":
            return
        raise AssertionError(f"staging stat failed: {response}")
    raise AssertionError(
        "private staging namespace is visible through File Transfer: "
        f"{response}"
    )


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
    partial_size = min(len(data), 16 * wire.MAX_PAYLOAD)
    metric = new_transfer(f"abort:{path}", "upload", partial_size)
    try:
        for sequence, offset in enumerate(range(0, partial_size, wire.MAX_PAYLOAD)):
            chunk = data[offset : offset + wire.MAX_PAYLOAD]
            emu.send(wire.build_frame(wire.DATA, transfer_id, sequence, offset, payload=chunk))
            frame = wait_transfer_frame(emu, metric, transfer_id)
            if frame["type"] == wire.NACK:
                if metric is not None:
                    metric.nacks += 1
                raise AssertionError(f"abort upload received NACK: {frame}")
            wire.require_ack(frame, transfer_id, sequence + 1, offset + len(chunk))
            if metric is not None:
                metric.ack_cycles += 1
                metric.progress(offset + len(chunk), sequence + 1)
        require_response(emu, request_id + 1, "binary.transfer.abort", {
            "transfer_id": transfer_id,
        })
        status = require_response(emu, request_id + 2, "file.transfer.status", {
            "transfer_id": transfer_id,
        })
        if (status.get("file_state") != "aborted" or
                status.get("error", {}).get("code") != "TRANSFER_ABORTED"):
            raise AssertionError(f"aborted write state mismatch: {status}")
    finally:
        if metric is not None:
            metric.complete()
    return transfer_id


def check_download(emu: wire.Emulator, request_id: int, path: str,
                   expected: bytes | None, offset: int = 0, length: int | None = None,
                   captured: bytearray | None = None) -> int | None:
    params: dict[str, object] = {"path": path, "offset_bytes": offset}
    if length is not None:
        params["length_bytes"] = length
    result = require_response(emu, request_id, "file.read.begin", params)
    selected = None if expected is None else (
        expected[offset:] if length is None else expected[offset : offset + length]
    )
    size = int(result["size_bytes"])
    if selected is not None and size != len(selected):
        raise AssertionError(f"read size mismatch: expected={len(selected)} result={result}")
    if size == 0:
        if result.get("transfer_id") is not None or result.get("state") != "completed":
            raise AssertionError(f"invalid synchronous read result: {result}")
        return None
    transfer_id = int(result["transfer_id"])
    metric = new_transfer(f"download:{path}:{offset}", "download", size)
    try:
        received = bytearray()
        sequence = 0
        while len(received) < size:
            frame = wait_transfer_frame(emu, metric, transfer_id)
            expected_length = min(wire.MAX_PAYLOAD, size - len(received))
            if frame["type"] == wire.NACK:
                if metric is not None:
                    metric.nacks += 1
                raise AssertionError(f"download received NACK: {frame}")
            if (frame["type"] == wire.DATA and frame["crc_valid"] and
                    frame["sequence"] + 1 == sequence and
                    frame["offset"] + len(frame["payload"]) == len(received)):
                # A cumulative ACK can be replayed if the emulator delivered a
                # duplicate DATA frame after the prior ACK.
                if metric is not None:
                    metric.duplicate_frames += 1
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
            if metric is not None:
                metric.ack_cycles += 1
                metric.progress(len(received), sequence)
        received_bytes = bytes(received)
        if captured is not None:
            captured.extend(received_bytes)
        if selected is not None and received_bytes != selected:
            raise AssertionError(
                f"download mismatch path={path} offset={offset} length={length}"
            )
        require_completed(emu, request_id + 1, transfer_id, size)
        return transfer_id
    finally:
        if metric is not None:
            metric.complete()


def check_upload(emu: wire.Emulator, request_id: int, path: str, data: bytes,
                 replace: bool = False) -> int | None:
    transfer_id, response = begin_write(emu, request_id, path, len(data), replace)
    if transfer_id is None:
        if data:
            raise AssertionError(f"upload did not begin: {response}")
        return None
    metric = new_transfer(f"upload:{path}", "upload", len(data))
    try:
        status = upload_existing_transfer(emu, transfer_id, data, request_id + 10, metric)
        if status is not None:
            raise AssertionError(f"upload failed: {status}")
        require_completed(emu, request_id + 1, transfer_id, len(data))
        return transfer_id
    finally:
        if metric is not None:
            metric.complete()


def upload_existing_transfer(emu: wire.Emulator, transfer_id: int, data: bytes,
                             status_request_id: int,
                             metric: TransferMetrics | None = None) -> dict | None:
    """Send frames for an already-begun write, returning terminal status on failure."""
    for sequence, offset in enumerate(range(0, len(data), wire.MAX_PAYLOAD)):
        chunk = data[offset : offset + wire.MAX_PAYLOAD]
        emu.send(wire.build_frame(wire.DATA, transfer_id, sequence, offset, payload=chunk))
        try:
            frame = wait_transfer_frame(emu, metric, transfer_id)
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
        if frame["type"] == wire.NACK:
            if metric is not None:
                metric.nacks += 1
            binary = require_response(emu, status_request_id, "binary.transfer.status", {
                "transfer_id": transfer_id,
            })
            file_status = require_response(emu, status_request_id + 1, "file.transfer.status", {
                "transfer_id": transfer_id,
            })
            return {"binary": binary, **file_status, "nack": frame}
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
        if metric is not None:
            metric.ack_cycles += 1
            metric.progress(offset + len(chunk), sequence + 1)
    return None


def require_file_transfer_capability(emu: wire.Emulator) -> None:
    hello = require_response(emu, 1, "protocol.hello")
    if "file-transfer.v1" not in hello.get("capabilities", []):
        raise AssertionError(f"file transfer capability missing: {hello}")


def run_basic(emu: wire.Emulator) -> None:
    require_file_transfer_capability(emu)
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
        "path": "/upload/CaseName.txt", "size_bytes": len(case_data),
    })
    require_error(emu, 48, "file.write.begin", "ALREADY_EXISTS", {
        "path": "/upload/casename.txt", "size_bytes": len(case_data),
    })
    case_replacement = payload(101, 14)
    check_upload(emu, 49, "/upload/casename.txt", case_replacement, replace=True)
    check_download(emu, 51, "/upload/CaseName.txt", case_replacement)

    small = payload(16 * 1024, 23)
    small_id = check_upload(emu, 60, "/upload/small-multi-frame.bin", small)
    if small_id is None:
        raise AssertionError("small multi-frame upload was synchronous")
    check_download(emu, 62, "/upload/small-multi-frame.bin", small, 511, 1025)
    check_download(emu, 64, "/upload/small-multi-frame.bin", small, 4095, 4097)
    check_upload(emu, 66, "/upload/empty.bin", b"")
    check_download(emu, 68, "/upload/empty.bin", b"")

    require_error(emu, 70, "file.stat", "INVALID_PATH", {"path": "../../escape"})
    require_error(emu, 71, "file.stat", "INVALID_PATH", {"path": "/upload/../escape"})
    require_error(emu, 72, "file.stat", "INVALID_PATH", {"path": "/upload//bad"})
    require_error(emu, 73, "file.stat", "INVALID_PATH", {"path": "/upload/bad/"})
    require_error(emu, 74, "file.stat", "NOT_FOUND", {"path": "/missing.bin"})
    emu.send(b'@ESP-NP2 {"v":1,"id":75,"cmd":"file.stat","params":{"path":"/upload/\xff.bin"}}\n')
    base.require_error(emu.wait_response(75), "INVALID_PATH")

    upload_names, _ = list_all(emu, 80, "/upload", limit=16)
    for name in ("日本語.bin", "small-multi-frame.bin", "empty.bin"):
        if name not in upload_names:
            raise AssertionError(f"basic uploaded name missing from FAT listing: {name}")
    if sum(name.casefold() == "casename.txt" for name in upload_names) != 1:
        raise AssertionError(f"case-collision listing mismatch: {upload_names}")
    if require_response(emu, 90, "system.ping") != {"pong": True}:
        raise AssertionError("basic final ping failed")
    print("FATFS_FILE_TRANSFER_BASIC=PASS")


def run_large(emu: wire.Emulator, size: int) -> None:
    require_file_transfer_capability(emu)
    data = payload(size, 31)
    path = "/upload/step6a2-large.bin"
    transfer_id = check_upload(emu, 100, path, data)
    if transfer_id is None:
        raise AssertionError("large upload was synchronous")
    print(f"FATFS_FILE_TRANSFER_LARGE upload=PASS size={size} sha256={digest(data)}")
    check_download(emu, 102, path, data)
    print(f"FATFS_FILE_TRANSFER_LARGE download=PASS size={size} sha256={digest(data)}")
    for index, (offset, length) in enumerate((
        (max(0, 512 - 1), 1025),
        (max(0, 4096 - 1), 4097),
        (max(0, size - 1025), min(1025, size)),
    )):
        check_download(emu, 110 + index * 2, path, data, offset, length)
    check_download(emu, 118, path, data, size, 0)
    if require_response(emu, 120, "system.ping") != {"pong": True}:
        raise AssertionError("large final ping failed")
    print(
        "FATFS_FILE_TRANSFER_LARGE=PASS "
        f"size={size} sha256={digest(data)} boundary_ranges=PASS"
    )


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
    metric = new_transfer(f"upload:{path}", "upload", len(data))
    try:
        status = upload_existing_transfer(emu, transfer_id, data, request_id + 10, metric)
        if status is not None:
            return False, transfer_id, status
        require_completed(emu, request_id + 12, transfer_id, len(data))
        return True, transfer_id, None
    finally:
        if metric is not None:
            metric.complete()


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
    require_staging_namespace_hidden(emu, 402)
    print(
        "FATFS_FILE_TRANSFER_NOSPACE=PASS "
        f"failed_size={failure_size} error={failure_code} "
        f"committed={len(committed)} endpoint_idle=1 endpoint_recoverable=1 "
        "staging_namespace_hidden=1"
    )


def run_nospace_preloaded(emu: wire.Emulator) -> None:
    failed_size = 1024 * 1024
    prefilled = require_response(emu, 199, "file.stat", {
        "path": "/upload/prefill.bin",
    })
    if (prefilled.get("type") != "file" or
            prefilled.get("size_bytes") != 4 * 1024 * 1024):
        raise AssertionError(f"prefilled capacity fixture is missing: {prefilled}")
    require_staging_namespace_hidden(emu, 198)
    prefilled_before = bytearray()
    check_download(emu, 205, "/upload/prefill.bin", None, 0, 4096, prefilled_before)
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
    prefilled_after = bytearray()
    check_download(emu, 207, "/upload/prefill.bin", None, 0, 4096, prefilled_after)
    if prefilled_before != prefilled_after:
        raise AssertionError("prefilled file bytes changed after NO_SPACE")
    require_staging_namespace_hidden(emu, 206)
    if require_response(emu, 202, "system.ping") != {"pong": True}:
        raise AssertionError("post-NO_SPACE preloaded ping failed")
    print(
        "FATFS_FILE_TRANSFER_NOSPACE=PASS "
        f"failed_size={failed_size} error=NO_SPACE committed=preloaded "
        "preexisting_intact=1 endpoint_idle=1 endpoint_recoverable=1 "
        "staging_namespace_hidden=1"
    )


def run_replacement(emu: wire.Emulator, size: int) -> None:
    path = "/upload/step6a2-replacement.bin"
    high_address = size > HIGH_ADDRESS_OFFSET + len(HIGH_ADDRESS_MARKER)
    original = payload(size, 31, high_address_marker=high_address)
    replacement = payload(size, 47, high_address_marker=high_address)
    check_upload(emu, 700, path, original)
    check_upload(emu, 702, path, replacement, replace=True)
    check_download(emu, 704, path, replacement)
    manual_abort(emu, 706, path, payload(size, 99, high_address_marker=high_address))
    check_download(emu, 710, path, replacement)
    print(
        "FATFS_FILE_TRANSFER_REPLACEMENT=PASS "
        f"replace=1 abort_preserved=1 size={size}"
    )


def run_persistence_write(emu: wire.Emulator, size: int) -> None:
    high_address = size > HIGH_ADDRESS_OFFSET + len(HIGH_ADDRESS_MARKER)
    data = payload(size, 81, high_address_marker=high_address)
    path = "/upload/persist.bin"
    check_upload(emu, 500, path, data)
    print(
        "FATFS_FILE_TRANSFER_PERSISTENCE "
        f"created=1 size={len(data)} sha256={digest(data)} "
        f"marker={'offset-1MiB' if high_address else 'not-required'}"
    )


def run_persistence_read(emu: wire.Emulator, size: int) -> None:
    high_address = size > HIGH_ADDRESS_OFFSET + len(HIGH_ADDRESS_MARKER)
    data = payload(size, 81, high_address_marker=high_address)
    path = "/upload/persist.bin"
    stat = require_response(emu, 600, "file.stat", {"path": path})
    if stat.get("type") != "file" or stat.get("size_bytes") != len(data):
        raise AssertionError(f"persisted file was not reused: {stat}")
    check_download(emu, 602, path, data)
    range_offset = HIGH_ADDRESS_OFFSET if high_address else max(0, size - 1025)
    range_length = len(HIGH_ADDRESS_MARKER) if high_address else min(1025, size)
    check_download(emu, 604, path, data, range_offset, range_length)
    print(
        "FATFS_FILE_TRANSFER_PERSISTENCE "
        f"created=0 reused=1 size={len(data)} sha256={digest(data)} "
        f"range_offset={range_offset}"
    )


def run_high_address_write(emu: wire.Emulator) -> None:
    require_file_transfer_capability(emu)
    data = payload(
        HIGH_ADDRESS_FILE_BYTES,
        113,
        high_address_marker=True,
        marker_offset=0,
    )
    path = "/upload/step6a2-high-address.bin"
    transfer_id = check_upload(emu, 1000, path, data)
    if transfer_id is None:
        raise AssertionError("high-address upload was synchronous")
    check_download(emu, 1002, path, data)
    print(
        "FATFS_FILE_TRANSFER_HIGH_ADDRESS "
        f"upload_size={len(data)} sha256={digest(data)} readback=PASS"
    )


def main() -> int:
    global ACTIVE_OBSERVER
    args = parse_args()
    emu = wire.Emulator(args)
    observer = ProgressObserver(args.mode)
    ACTIVE_OBSERVER = observer
    error: BaseException | None = None
    exit_status: int | None = None
    try:
        emu.start()
        emu.wait_line(FATFS_MARKER, timeout=30.0)
        emu.wait_line(READY_MARKER, timeout=30.0)
        observer.phase_start(args.mode, {
            "large": args.large_size,
            "replacement": args.replacement_size,
            "persistence-write": args.persistence_size,
            "persistence-read": args.persistence_size,
            "high-address-write": HIGH_ADDRESS_FILE_BYTES,
        }.get(args.mode, 0))
        if args.mode == "basic":
            run_basic(emu)
        elif args.mode == "large":
            run_large(emu, args.large_size)
        elif args.mode == "matrix":
            run_matrix(emu)
        elif args.mode == "nospace":
            run_nospace(emu)
        elif args.mode == "nospace-preloaded":
            run_nospace_preloaded(emu)
        elif args.mode == "high-address-write":
            run_high_address_write(emu)
        elif args.mode == "replacement":
            run_replacement(emu, args.replacement_size)
        elif args.mode == "persistence-write":
            run_persistence_write(emu, args.persistence_size)
        else:
            run_persistence_read(emu, args.persistence_size)
        observer.phase_complete(args.mode)
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
        observer.summary("FAIL")
        ACTIVE_OBSERVER = None
        traceback.print_exception(error)
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    if exit_status not in (0, -15, -9):
        observer.summary("FAIL")
        ACTIVE_OBSERVER = None
        print(f"ERROR: esp-emu exit status was {exit_status}", file=sys.stderr)
        return 1
    if args.mode == "high-address-write":
        try:
            import build_storage_fatfs_flash as flash_builder
            flash_builder.verify_high_address_state(
                args.verify_state.resolve(), args.marker, args.minimum_offset
            )
        except SystemExit as exc:
            observer.summary("FAIL")
            ACTIVE_OBSERVER = None
            print(f"ERROR: high-address verification failed: {exc}", file=sys.stderr)
            return 1
        print(
            "FATFS_FILE_TRANSFER_HIGH_ADDRESS=PASS "
            f"save_state=PASS marker={args.marker} "
            f"minimum_offset=0x{args.minimum_offset:x}"
        )
    observer.summary("PASS")
    ACTIVE_OBSERVER = None
    print(f"PASS: FATFS FILE TRANSFER {args.mode} OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
