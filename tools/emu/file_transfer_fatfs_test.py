#!/usr/bin/env python3
"""FATFS-backed File Transfer regression over the real esp-emu UART protocol."""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import hashlib
import json
import os
from pathlib import Path
import sys
import time
import traceback

import file_transfer_base_test as base
import uart_binary_data_plane_test as wire


MAX_FILE_BYTES = 2 * 1024 * 1024
LARGE_FILE_BYTES = 262145
REPLACEMENT_STRESS_CLUSTERS = 419
REPLACEMENT_SAFETY_MARGIN_CLUSTERS = 35
REPLACEMENT_STRESS_BYTES = 0x1A3000
PERSISTENCE_FILE_BYTES = 4097
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
NOSPACE_REQUEST_BYTES = 1024 * 1024
NOSPACE_PREFIX_BYTES = 4096


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
        choices=("basic", "large", "max-payload", "nospace", "nospace-preloaded",
                 "high-address-write",
                 "replacement-stress", "persistence-write", "persistence-read"),
        required=True,
    )
    parser.add_argument(
        "--large-size",
        type=int,
        default=LARGE_FILE_BYTES,
        help="large-mode payload size in bytes (default: 262145; 2 MiB is extended-only)",
    )
    parser.add_argument("--base-free-clusters", type=int)
    parser.add_argument("--cluster-size", type=int)
    parser.add_argument(
        "--persistence-size",
        type=int,
        default=PERSISTENCE_FILE_BYTES,
        help="persistence payload size in bytes (default: 4097)",
    )
    parser.add_argument(
        "--nospace-metadata",
        type=Path,
        help="geometry metadata generated for the nospace-preloaded fixture",
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
    for option_name in ("large_size", "persistence_size"):
        value = getattr(args, option_name)
        if value <= 0 or value > MAX_FILE_BYTES:
            parser.error(f"--{option_name.replace('_', '-')} must be in the range 1..{MAX_FILE_BYTES}")
    if args.mode == "large" and args.large_size != LARGE_FILE_BYTES:
        print(
            "WARNING: --large-size overrides the routine 262145-byte large-mode workload; "
            "use only for extended/manual validation",
            file=sys.stderr,
        )
    geometry_mode = args.mode in ("max-payload", "replacement-stress")
    if geometry_mode and (args.base_free_clusters is None or args.cluster_size is None):
        parser.error(
            "--base-free-clusters and --cluster-size are required for "
            f"{args.mode}"
        )
    if not geometry_mode and (
            args.base_free_clusters is not None or args.cluster_size is not None):
        parser.error(
            "--base-free-clusters and --cluster-size are only valid for "
            "max-payload or replacement-stress"
        )
    if args.base_free_clusters is not None and args.base_free_clusters < 0:
        parser.error("--base-free-clusters must be non-negative")
    if args.cluster_size is not None and args.cluster_size <= 0:
        parser.error("--cluster-size must be positive")
    if args.mode == "persistence-write" and not args.save_state:
        parser.error("--save-state is required for persistence-write")
    if args.mode == "persistence-read" and args.save_state:
        parser.error("--save-state is only valid for persistence-write")
    if args.mode == "nospace-preloaded" and args.nospace_metadata is None:
        parser.error("--nospace-metadata is required for nospace-preloaded")
    if args.mode != "nospace-preloaded" and args.nospace_metadata is not None:
        parser.error("--nospace-metadata is only valid for nospace-preloaded")
    if args.mode == "high-address-write":
        if not args.save_state:
            parser.error("--save-state is required for high-address-write")
        if args.verify_state is None:
            args.verify_state = args.firmware
    elif args.verify_state is not None:
        parser.error("--verify-state is only valid for high-address-write")
    bounded_mode = args.mode not in ("max-payload", "replacement-stress", "nospace")
    args.emulator_timeout = "600s" if bounded_mode else "3600s"
    args.process_timeout = 620.0 if bounded_mode else 3620.0
    args.extra_emu_args = ["--batch-size", str(UART_TEST_BATCH_SIZE)]
    if args.save_state:
        args.extra_emu_args.append("--save-state")
    return args


def load_nospace_metadata(path: Path) -> dict[str, int]:
    try:
        root = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise AssertionError(f"cannot read NoSpace metadata {path}: {exc}") from exc
    if not isinstance(root, dict) or root.get("schema_version") != 1:
        raise AssertionError(f"invalid NoSpace metadata schema: {path}")
    names = (
        "storage_offset", "storage_size", "storage_end", "cluster_size",
        "usable_clusters", "base_allocated_clusters", "base_free_clusters",
        "nospace_request_bytes", "nospace_required_clusters",
        "nospace_target_free_clusters", "nospace_prefill_clusters",
        "nospace_prefill_bytes", "nospace_prefill_byte",
        "final_allocated_clusters", "final_free_clusters",
    )
    values: dict[str, int] = {}
    for name in names:
        value = root.get(name)
        if isinstance(value, bool) or not isinstance(value, int):
            raise AssertionError(f"NoSpace metadata field is not an integer: {name}")
        values[name] = value
    if values["storage_end"] != values["storage_offset"] + values["storage_size"]:
        raise AssertionError(f"NoSpace storage geometry is inconsistent: {values}")
    request_bytes = values["nospace_request_bytes"]
    cluster_size = values["cluster_size"]
    if request_bytes != NOSPACE_REQUEST_BYTES:
        raise AssertionError(
            f"NoSpace request changed: {request_bytes} != {NOSPACE_REQUEST_BYTES}"
        )
    if cluster_size <= 0:
        raise AssertionError(f"invalid NoSpace cluster size: {cluster_size}")
    required_clusters = (request_bytes + cluster_size - 1) // cluster_size
    target_free_clusters = required_clusters - 1
    if values["nospace_required_clusters"] != required_clusters:
        raise AssertionError(f"NoSpace required-cluster metadata is inconsistent: {values}")
    if values["nospace_target_free_clusters"] != target_free_clusters:
        raise AssertionError(f"NoSpace target-free metadata is inconsistent: {values}")
    if values["base_free_clusters"] - target_free_clusters != values["nospace_prefill_clusters"]:
        raise AssertionError(f"NoSpace prefill-cluster metadata is inconsistent: {values}")
    if values["nospace_prefill_clusters"] <= 0:
        raise AssertionError(f"NoSpace prefill must be positive: {values}")
    if values["nospace_prefill_bytes"] != (
            values["nospace_prefill_clusters"] * cluster_size):
        raise AssertionError(f"NoSpace prefill-byte metadata is inconsistent: {values}")
    if values["final_free_clusters"] != target_free_clusters:
        raise AssertionError(f"NoSpace final-free metadata is inconsistent: {values}")
    if not 0 <= values["nospace_prefill_byte"] <= 0xFF:
        raise AssertionError(f"invalid NoSpace prefill byte: {values}")
    return values


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


def require_base_capacity(mode: str, base_free_clusters: int, cluster_size: int,
                           payload_bytes: int, required_clusters: int) -> None:
    if base_free_clusters < required_clusters:
        raise AssertionError(
            f"{mode} clean-base capacity is insufficient: "
            f"free_clusters={base_free_clusters} required_clusters={required_clusters}"
        )
    print(
        f"FATFS_{mode.upper().replace('-', '_')}_GEOMETRY "
        f"cluster_size={cluster_size} base_free_clusters={base_free_clusters} "
        f"payload_bytes={payload_bytes} payload_clusters={required_clusters}"
    )


def run_large(emu: wire.Emulator, size: int, extended_ranges: bool = False) -> None:
    require_file_transfer_capability(emu)
    data = payload(size, 31)
    path = "/upload/step6a2-large.bin"
    transfer_id = check_upload(emu, 100, path, data)
    if transfer_id is None:
        raise AssertionError("large upload was synchronous")
    marker = "MAX_PAYLOAD" if extended_ranges else "LARGE"
    print(f"FATFS_FILE_TRANSFER_{marker} upload=PASS size={size} sha256={digest(data)}")
    check_download(emu, 102, path, data)
    print(f"FATFS_FILE_TRANSFER_{marker} download=PASS size={size} sha256={digest(data)}")
    if extended_ranges:
        ranges = [
            (0, 513),
            (511, 1025),
            (512, 4096),
            (4095, 4097),
            (4096, 8193),
            (1024 * 1024, 65537),
            (size - 1025, 1025),
        ]
    else:
        ranges = [
            (max(0, 512 - 1), 1025),
            (max(0, 4096 - 1), 4097),
            (max(0, size - 1025), min(1025, size)),
        ]
    for index, (offset, length) in enumerate(ranges):
        check_download(emu, 110 + index * 2, path, data, offset, length)
    eof_request_id = 110 + len(ranges) * 2
    check_download(emu, eof_request_id, path, data, size, 0)
    require_staging_namespace_hidden(emu, eof_request_id + 2)
    if require_response(emu, eof_request_id + 4, "system.ping") != {"pong": True}:
        raise AssertionError("large final ping failed")
    print(
        f"FATFS_FILE_TRANSFER_{marker}=PASS "
        f"size={size} sha256={digest(data)} boundary_ranges=PASS"
    )


def run_max_payload(emu: wire.Emulator, base_free_clusters: int,
                    cluster_size: int) -> None:
    required_clusters = (
        MAX_FILE_BYTES + cluster_size - 1
    ) // cluster_size
    require_base_capacity(
        "max-payload", base_free_clusters, cluster_size,
        MAX_FILE_BYTES, required_clusters
    )
    run_large(emu, MAX_FILE_BYTES, extended_ranges=True)
    print(
        "FATFS_FILE_TRANSFER_MAX_PAYLOAD=PASS "
        f"size={MAX_FILE_BYTES} clusters={required_clusters} "
        "replacement=not-attempted"
    )


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


def run_nospace_preloaded(emu: wire.Emulator, metadata: dict[str, int]) -> None:
    failed_size = NOSPACE_REQUEST_BYTES
    prefill_size = metadata["nospace_prefill_bytes"]
    prefill_prefix = bytes([metadata["nospace_prefill_byte"]]) * min(
        NOSPACE_PREFIX_BYTES, prefill_size
    )
    prefilled = require_response(emu, 199, "file.stat", {
        "path": "/upload/prefill.bin",
    })
    if (prefilled.get("type") != "file" or
            prefilled.get("size_bytes") != prefill_size):
        raise AssertionError(f"prefilled capacity fixture is missing: {prefilled}")
    require_staging_namespace_hidden(emu, 198)
    prefilled_before = bytearray()
    check_download(
        emu, 205, "/upload/prefill.bin", prefill_prefix, 0,
        len(prefill_prefix), prefilled_before
    )
    response = send_request(emu, 200, "file.write.begin", {
        "path": "/upload/nospace-preloaded.bin",
        "size_bytes": failed_size,
    })
    base.require_error(response, "NO_SPACE")
    if response.get("result", {}).get("transfer_id") is not None:
        raise AssertionError(f"NoSpace begin unexpectedly returned a transfer: {response}")
    probe = send_request(emu, 201, "file.write.begin", {
        "path": "/upload/post-nospace-preloaded-probe.bin", "size_bytes": 0,
    })
    if probe.get("ok") is not True or probe.get("result", {}).get("state") != "completed":
        raise AssertionError(f"endpoint did not recover after preloaded NO_SPACE: {probe}")
    intact = require_response(emu, 203, "file.stat", {
        "path": "/upload/prefill.bin",
    })
    if (intact.get("type") != "file" or
            intact.get("size_bytes") != prefill_size):
        raise AssertionError(f"prefilled file changed after NO_SPACE: {intact}")
    prefilled_after = bytearray()
    check_download(
        emu, 207, "/upload/prefill.bin", prefill_prefix, 0,
        len(prefill_prefix), prefilled_after
    )
    if prefilled_before != prefilled_after:
        raise AssertionError("prefilled file bytes changed after NO_SPACE")
    require_staging_namespace_hidden(emu, 206)
    if require_response(emu, 202, "system.ping") != {"pong": True}:
        raise AssertionError("post-NO_SPACE preloaded ping failed")
    print(
        "FATFS_FILE_TRANSFER_NOSPACE=PASS "
        f"failed_size={failed_size} error=NO_SPACE failure_phase=begin "
        f"payload_frames=0 prefill_bytes={prefill_size} "
        f"target_free_clusters={metadata['nospace_target_free_clusters']} "
        f"final_free_clusters={metadata['final_free_clusters']} committed=preloaded "
        "preexisting_intact=1 endpoint_idle=1 endpoint_recoverable=1 "
        "staging_namespace_hidden=1"
    )


def run_replacement_stress(emu: wire.Emulator, base_free_clusters: int,
                           cluster_size: int) -> None:
    if REPLACEMENT_STRESS_BYTES != REPLACEMENT_STRESS_CLUSTERS * cluster_size:
        raise AssertionError(
            "replacement geometry changed: "
            f"cluster_size={cluster_size} bytes={REPLACEMENT_STRESS_BYTES} "
            f"clusters={REPLACEMENT_STRESS_CLUSTERS}"
        )
    required_peak_free_clusters = (
        2 * REPLACEMENT_STRESS_CLUSTERS + REPLACEMENT_SAFETY_MARGIN_CLUSTERS
    )
    if base_free_clusters < required_peak_free_clusters:
        raise AssertionError(
            "replacement clean-base capacity is insufficient: "
            f"free_clusters={base_free_clusters} "
            f"required_peak_free_clusters={required_peak_free_clusters}"
        )
    print(
        "FATFS_REPLACEMENT_GEOMETRY "
        f"cluster_size={cluster_size} measured_free_clusters={base_free_clusters} "
        f"replacement_clusters={REPLACEMENT_STRESS_CLUSTERS} "
        f"replacement_bytes={REPLACEMENT_STRESS_BYTES} "
        f"safety_margin_clusters={REPLACEMENT_SAFETY_MARGIN_CLUSTERS} "
        f"required_peak_free_clusters={required_peak_free_clusters}"
    )

    path = "/upload/step6a2-replacement.bin"
    for request_id, stale_path in enumerate((
        path,
        "/upload/step6a2-large.bin",
        "/upload/step6a2-a.bin",
    ), start=690):
        require_error(emu, request_id, "file.stat", "NOT_FOUND", {"path": stale_path})
    require_staging_namespace_hidden(emu, 694)

    original = payload(REPLACEMENT_STRESS_BYTES, 31)
    replacement = payload(REPLACEMENT_STRESS_BYTES, 47)
    check_upload(emu, 700, path, original)
    check_download(emu, 702, path, original)
    check_upload(emu, 704, path, replacement, replace=True)
    check_download(emu, 706, path, replacement)
    require_staging_namespace_hidden(emu, 708)
    if require_response(emu, 710, "system.ping") != {"pong": True}:
        raise AssertionError("replacement endpoint did not recover after success")

    abort_data = payload(REPLACEMENT_STRESS_BYTES, 99)
    partial_bytes = min(len(abort_data), 16 * wire.MAX_PAYLOAD)
    manual_abort(emu, 712, path, abort_data, replace=True)
    check_download(emu, 716, path, replacement)
    require_staging_namespace_hidden(emu, 718)
    if require_response(emu, 720, "system.ping") != {"pong": True}:
        raise AssertionError("replacement endpoint did not recover after abort")
    print(
        "FATFS_FILE_TRANSFER_REPLACEMENT_STRESS=PASS "
        f"size={REPLACEMENT_STRESS_BYTES} partial_abort_bytes={partial_bytes} "
        "replace=1 abort_preserved=1"
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
    nospace_metadata = (
        load_nospace_metadata(args.nospace_metadata.resolve())
        if args.mode == "nospace-preloaded" else None
    )
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
            "max-payload": MAX_FILE_BYTES,
            "replacement-stress": REPLACEMENT_STRESS_BYTES,
            "persistence-write": args.persistence_size,
            "persistence-read": args.persistence_size,
            "high-address-write": HIGH_ADDRESS_FILE_BYTES,
        }.get(args.mode, 0))
        if args.mode == "basic":
            run_basic(emu)
        elif args.mode == "large":
            run_large(emu, args.large_size)
        elif args.mode == "max-payload":
            assert args.base_free_clusters is not None
            assert args.cluster_size is not None
            run_max_payload(emu, args.base_free_clusters, args.cluster_size)
        elif args.mode == "nospace":
            run_nospace(emu)
        elif args.mode == "nospace-preloaded":
            assert nospace_metadata is not None
            run_nospace_preloaded(emu, nospace_metadata)
        elif args.mode == "high-address-write":
            run_high_address_write(emu)
        elif args.mode == "replacement-stress":
            assert args.base_free_clusters is not None
            assert args.cluster_size is not None
            run_replacement_stress(
                emu, args.base_free_clusters, args.cluster_size
            )
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
