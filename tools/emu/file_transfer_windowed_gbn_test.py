#!/usr/bin/env python3
"""Opt-in raw W=2 bounded go-back-N upload regressions."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import time
import traceback
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from file_transfer_ack_loss_test import RecordingEmulatorSerial
from np2_fixture_cache import (
    CAPABILITY_WINDOWED_GBN_V1,
    DATA_WINDOW_FRAMES,
    ENCODING_ZERO_RLE_V1,
    RemoteError,
    SerialFileTransferClient,
    UploadModeError,
    _SerialParser,
)
import uart_binary_data_plane_test as wire


PAYLOAD_BYTES = 8193
DATA_FRAMES = (PAYLOAD_BYTES + wire.MAX_PAYLOAD - 1) // wire.MAX_PAYLOAD
FINAL_WINDOW_SEQUENCES = {DATA_FRAMES - 2, DATA_FRAMES - 1}
FAULT_NORMAL = "normal"
FAULT_ACK0_LOSS = "ack0-loss"
FAULT_WINDOW_ACK_LOSS = "window-ack-loss"
FAULT_DATA0_LOSS = "data0-loss"
FAULT_DATA1_CORRUPT = "data1-corrupt"
FAULT_FINAL_ACK_LOSS = "final-window-ack-loss"
CONTROL_STALE_ACK = "stale-ack"
CONTROL_STALE_NACK = "stale-nack"
CONTROL_FUTURE_ACK = "future-ack"
CONTROL_MISMATCHED_ACK = "mismatched-ack"
CONTROL_MISMATCHED_NACK = "mismatched-nack"
CONTROL_WRONG_TRANSFER_ACK = "wrong-transfer-ack"
CONTROL_CORRUPT_ACK = "corrupt-ack"
CONTROL_RAW_ACK_PAYLOAD = "raw-ack-payload"
CONTROL_RAW_ACK_STATUS = "raw-ack-status"
CONTROL_RAW_NACK_PAYLOAD = "raw-nack-payload"
CONTROL_RAW_NACK_ZERO_STATUS = "raw-nack-zero-status"
CONTROL_RAW_NACK_INVALID_STATUS = "raw-nack-invalid-status"

CONTROL_SUCCESS_CASES = {
    CONTROL_STALE_ACK,
    CONTROL_STALE_NACK,
    CONTROL_WRONG_TRANSFER_ACK,
    CONTROL_CORRUPT_ACK,
}
CONTROL_PROTOCOL_ERROR_CASES = {
    CONTROL_FUTURE_ACK,
    CONTROL_MISMATCHED_ACK,
    CONTROL_MISMATCHED_NACK,
    CONTROL_RAW_ACK_PAYLOAD,
    CONTROL_RAW_ACK_STATUS,
    CONTROL_RAW_NACK_PAYLOAD,
    CONTROL_RAW_NACK_ZERO_STATUS,
    CONTROL_RAW_NACK_INVALID_STATUS,
}
CONTROL_RAW_CASES = {
    CONTROL_RAW_ACK_PAYLOAD,
    CONTROL_RAW_ACK_STATUS,
    CONTROL_RAW_NACK_PAYLOAD,
    CONTROL_RAW_NACK_ZERO_STATUS,
    CONTROL_RAW_NACK_INVALID_STATUS,
}


class DropAckParser(_SerialParser):
    """Drop a bounded number of actual firmware ACK frames."""

    def __init__(self, predicate: Callable[[dict], bool], count: int) -> None:
        super().__init__()
        self.predicate = predicate
        self.remaining = count
        self.dropped_frames: list[dict] = []

    def feed(self, data: bytes) -> None:
        super().feed(data)
        if self.remaining == 0:
            return
        for index in range(len(self.frames) - 1, -1, -1):
            frame = self.frames[index]
            if (
                self.remaining > 0
                and frame.get("type") == wire.ACK
                and self.predicate(frame)
            ):
                del self.frames[index]
                self.dropped_frames.append(frame)
                self.remaining -= 1
                print(
                    "FAULT_INJECT action=drop-ack direction=firmware-to-host "
                    f"sequence={frame['sequence']} offset={frame['offset']} "
                    f"remaining={self.remaining}"
                )


def parsed_control_frame(frame_type: int, transfer_id: int, sequence: int,
                         offset: int, status: int = 0) -> dict:
    encoded = wire.build_frame(
        frame_type, transfer_id, sequence, offset, status=status
    )
    return wire.parse_frame(wire.cobs_decode(encoded[2:-1]))


class WindowControlValidationParser(_SerialParser):
    """Inject one W=2 control response after a real firmware ACK."""

    def __init__(self, fault: str) -> None:
        super().__init__()
        self.fault = fault
        self.injections: list[dict] = []

    def _raw_frame(self, frame: dict) -> bytes:
        transfer_id = int(frame["transfer_id"])
        if self.fault == CONTROL_RAW_ACK_PAYLOAD:
            return wire.build_unchecked_control_frame(
                wire.ACK, transfer_id, 1, 1024, payload=b"unexpected-ack-payload"
            )
        if self.fault == CONTROL_RAW_ACK_STATUS:
            return wire.build_unchecked_control_frame(
                wire.ACK, transfer_id, 1, 1024, status=1
            )
        if self.fault == CONTROL_RAW_NACK_PAYLOAD:
            return wire.build_unchecked_control_frame(
                wire.NACK, transfer_id, 1, 1024, status=1,
                payload=b"unexpected-nack-payload",
            )
        if self.fault == CONTROL_RAW_NACK_ZERO_STATUS:
            return wire.build_unchecked_control_frame(
                wire.NACK, transfer_id, 1, 1024, status=0
            )
        if self.fault == CONTROL_RAW_NACK_INVALID_STATUS:
            return wire.build_unchecked_control_frame(
                wire.NACK, transfer_id, 1, 1024, status=0xFFFF
            )
        raise AssertionError(f"unsupported raw W=2 control case: {self.fault}")

    def _injection(self, frame: dict) -> dict | None:
        if frame.get("type") != wire.ACK or self.injections:
            return None
        transfer_id = int(frame["transfer_id"])
        sequence = int(frame["sequence"])
        offset = int(frame["offset"])
        if self.fault == CONTROL_STALE_ACK and (sequence, offset) == (2, 2048):
            return parsed_control_frame(wire.ACK, transfer_id, 1, 1024)
        if self.fault == CONTROL_STALE_NACK and (sequence, offset) == (2, 2048):
            return parsed_control_frame(wire.NACK, transfer_id, 0, 0, status=1)
        if self.fault == CONTROL_FUTURE_ACK and (sequence, offset) == (1, 1024):
            return parsed_control_frame(wire.ACK, transfer_id, 4, 4096)
        if self.fault == CONTROL_MISMATCHED_ACK and (sequence, offset) == (1, 1024):
            return parsed_control_frame(wire.ACK, transfer_id, 2, 2049)
        if self.fault == CONTROL_MISMATCHED_NACK and (sequence, offset) == (1, 1024):
            return parsed_control_frame(wire.NACK, transfer_id, 2, 2049, status=1)
        if self.fault == CONTROL_WRONG_TRANSFER_ACK and (sequence, offset) == (1, 1024):
            return parsed_control_frame(wire.ACK, transfer_id + 1, 4, 4096)
        if self.fault == CONTROL_CORRUPT_ACK and (sequence, offset) == (1, 1024):
            corrupt = dict(parsed_control_frame(wire.ACK, transfer_id, 4, 4096))
            corrupt["crc_valid"] = False
            return corrupt
        return None

    def feed(self, data: bytes) -> None:
        before = len(self.frames)
        super().feed(data)
        if self.injections:
            return
        for index in range(before, len(self.frames)):
            frame = self.frames[index]
            if frame.get("type") != wire.ACK:
                continue
            if self.fault in CONTROL_RAW_CASES:
                if (frame.get("sequence"), frame.get("offset")) != (1, 1024):
                    continue
                raw = self._raw_frame(frame)
                try:
                    wire.parse_frame(wire.cobs_decode(raw[2:-1]))
                except (AssertionError, ValueError):
                    pass
                else:
                    if self.fault != CONTROL_RAW_NACK_INVALID_STATUS:
                        raise AssertionError(
                            "canonical parser unexpectedly accepted malformed W=2 control"
                        )
                real_ack = self.frames[index]
                del self.frames[index]
                super().feed(raw)
                injected = self.frames.pop()
                self.frames.insert(index, injected)
                self.frames.insert(index + 1, real_ack)
                self.injections.append(injected)
                print(
                    "FAULT_INJECT action=append-encoded direction=firmware-to-host "
                    f"case={self.fault} type={injected.get('type')} "
                    f"transfer_id={injected.get('transfer_id')} "
                    f"sequence={injected.get('sequence')} offset={injected.get('offset')} "
                    f"crc_valid={injected.get('crc_valid')}"
                )
                return
            injected = self._injection(frame)
            if injected is None:
                continue
            self.frames.append(injected)
            self.injections.append(injected)
            print(
                "FAULT_INJECT action=append-delayed direction=firmware-to-host "
                f"case={self.fault} type={injected.get('type')} "
                f"transfer_id={injected.get('transfer_id')} "
                f"sequence={injected.get('sequence')} offset={injected.get('offset')} "
                f"crc_valid={injected.get('crc_valid')}"
            )


def corrupt_data_wire(data: bytes) -> bytes:
    decoded = bytearray(wire.cobs_decode(data[2:-1]))
    decoded[wire.HEADER_BYTES] ^= 0x01
    return b"\x00\x00" + wire.cobs_encode(bytes(decoded)) + b"\x00"


class WindowFaultSerial(RecordingEmulatorSerial):
    """Record host DATA and deterministically drop/corrupt selected frames."""

    def __init__(self, emu: wire.Emulator, fault: str) -> None:
        super().__init__(emu)
        self.fault = fault
        self.host_parser: _SerialParser | None = None
        self.data_send_parser_queue: list[int] = []
        self.dropped_data: list[dict] = []
        self.corrupted_data: list[dict] = []

    def write(self, data: bytes) -> int:
        self.tx_parser.feed(data)
        frames = list(self.tx_parser.frames)
        self.tx_parser.frames.clear()
        outgoing = data
        for frame in frames:
            if frame.get("type") != wire.DATA:
                continue
            self.data_frames.append(frame)
            if self.host_parser is not None:
                self.data_send_parser_queue.append(len(self.host_parser.frames))
            sequence = frame.get("sequence")
            if self.fault == FAULT_DATA0_LOSS and sequence == 0 and not self.dropped_data:
                self.dropped_data.append(frame)
                print("FAULT_INJECT action=drop-data sequence=0")
                return len(data)
            if self.fault == FAULT_DATA1_CORRUPT and sequence == 1 and not self.corrupted_data:
                self.corrupted_data.append(frame)
                outgoing = corrupt_data_wire(data)
                print("FAULT_INJECT action=corrupt-data sequence=1")
        self.sock.sendall(outgoing)
        return len(data)


def payload() -> bytes:
    return bytes(
        (index * 73 + (index >> 8) * 19 + 0x41) & 0xFF
        for index in range(PAYLOAD_BYTES)
    )


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
    args.emulator_timeout = "120s"
    args.process_timeout = 140.0
    return args


def require_response(response: dict, request_id: int) -> dict:
    if response.get("id") != request_id:
        raise AssertionError(f"unexpected response id: {response}")
    return response


def require_ok(response: dict, request_id: int) -> dict:
    response = require_response(response, request_id)
    if response.get("ok") is not True:
        raise AssertionError(f"request failed: {response}")
    result = response.get("result")
    if not isinstance(result, dict):
        raise AssertionError(f"response result is invalid: {response}")
    return result


def require_error(response: dict, request_id: int, code: str) -> None:
    response = require_response(response, request_id)
    if response.get("ok") is not False or response.get("error", {}).get("code") != code:
        raise AssertionError(f"expected {code}: {response}")


def run_negotiation_cases(emu: wire.Emulator) -> None:
    hello = require_ok(emu.send_json(10, "protocol.hello") or emu.wait_response(10), 10)
    if CAPABILITY_WINDOWED_GBN_V1 not in hello.get("capabilities", []):
        raise AssertionError(f"windowed capability missing: {hello}")

    default = require_ok(
        emu.send_json(11, "file.write.begin", {
            "path": "/upload/e2-default.bin",
            "size_bytes": 1,
            "replace": True,
        }) or emu.wait_response(11),
        11,
    )
    if "transport" in default or "window_frames" in default:
        raise AssertionError(f"W=1 default unexpectedly negotiated: {default}")
    default_id = int(default["transfer_id"])
    require_ok(
        emu.send_json(12, "binary.transfer.abort", {"transfer_id": default_id}) or
        emu.wait_response(12),
        12,
    )

    selected = require_ok(
        emu.send_json(13, "file.write.begin", {
            "path": "/upload/e2-selected.bin",
            "size_bytes": 1,
            "replace": True,
            "transport": "windowed-gbn-v1",
            "window_frames": 2,
        }) or emu.wait_response(13),
        13,
    )
    if selected.get("transport") != "windowed-gbn-v1" or selected.get("window_frames") != 2:
        raise AssertionError(f"W=2 selection was not echoed: {selected}")
    selected_id = int(selected["transfer_id"])
    require_ok(
        emu.send_json(14, "binary.transfer.abort", {"transfer_id": selected_id}) or
        emu.wait_response(14),
        14,
    )

    require_error(
        emu.send_json(15, "file.write.begin", {
            "path": "/upload/e2-window4.bin",
            "size_bytes": 1,
            "replace": True,
            "transport": "windowed-gbn-v1",
            "window_frames": 4,
        }) or emu.wait_response(15),
        15,
        "UNSUPPORTED",
    )
    require_error(
        emu.send_json(16, "file.write.begin", {
            "path": "/upload/e2-zero-rle.bin",
            "size_bytes": 1,
            "wire_size_bytes": 6,
            "replace": True,
            "encoding": ENCODING_ZERO_RLE_V1,
            "transport": "windowed-gbn-v1",
            "window_frames": 2,
        }) or emu.wait_response(16),
        16,
        "UNSUPPORTED",
    )
    print("E2_NEGOTIATION default=W1 selected=W2 window4=REJECT zero_rle=REJECT")


def run_host_negotiation_unit_cases() -> None:
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "e2-negotiation.bin"
        source.write_bytes(b"x")

        old_client = object.__new__(SerialFileTransferClient)
        old_client.capabilities = frozenset()
        old_client.request = lambda *args, **kwargs: (_ for _ in ()).throw(
            AssertionError("old peer must be rejected before begin")
        )
        try:
            old_client.upload(
                "/upload/e2-old-peer.bin", source, replace=True,
                encoding="raw", transport="windowed-gbn-v1", window_frames=2,
            )
        except UploadModeError:
            pass
        else:
            raise AssertionError("windowed upload unexpectedly fell back without capability")

        mismatch_client = object.__new__(SerialFileTransferClient)
        mismatch_client.capabilities = frozenset({CAPABILITY_WINDOWED_GBN_V1})
        requests: list[tuple[str, dict | None]] = []

        def request(command: str, params: dict | None = None,
                    timeout: float | None = None) -> dict:
            requests.append((command, params))
            if command == "file.write.begin":
                return {"transfer_id": 77}
            if command == "binary.transfer.abort":
                return {}
            raise AssertionError(f"unexpected fake request: {command}")

        mismatch_client.request = request
        mismatch_client.send_raw = lambda data: len(data)
        try:
            mismatch_client.upload(
                "/upload/e2-mismatch.bin", source, replace=True,
                encoding="raw", transport="windowed-gbn-v1", window_frames=2,
            )
        except RemoteError as error:
            if error.code != "PROTOCOL_ERROR":
                raise
        else:
            raise AssertionError("negotiation mismatch did not fail")
        if any(command == "binary.transfer.abort" for command, _ in requests) is not True:
            raise AssertionError("negotiation mismatch did not abort the selected transfer")
        if any(command == "file.transfer.status" for command, _ in requests):
            raise AssertionError("negotiation mismatch sent data")
    print("E2_NEGOTIATION host_old_peer=REJECT mismatch=PROTOCOL_ERROR")


def make_client(emu: wire.Emulator, fault: str) -> tuple[
    SerialFileTransferClient, WindowFaultSerial
]:
    ack_predicate: Callable[[dict], bool] = lambda frame: False
    ack_count = 0
    if fault == FAULT_ACK0_LOSS:
        ack_predicate = lambda frame: frame.get("sequence") == 1
        ack_count = 1
    elif fault == FAULT_WINDOW_ACK_LOSS:
        ack_predicate = lambda frame: frame.get("sequence") in {1, 2}
        ack_count = 2
    elif fault == FAULT_DATA1_CORRUPT:
        # Keep DATA2 out of flight so the NACK frontier demonstrates a retry
        # from DATA1 only, as specified by the deterministic fault.
        ack_predicate = lambda frame: frame.get("sequence") == 1
        ack_count = 1
    elif fault == FAULT_FINAL_ACK_LOSS:
        ack_predicate = lambda frame: frame.get("sequence") in {
            DATA_FRAMES - 1, DATA_FRAMES
        }
        ack_count = 2
    serial = WindowFaultSerial(emu, fault)
    parser = (
        WindowControlValidationParser(fault)
        if fault in CONTROL_SUCCESS_CASES | CONTROL_PROTOCOL_ERROR_CASES
        else DropAckParser(ack_predicate, ack_count)
    )
    serial.host_parser = parser
    client = object.__new__(SerialFileTransferClient)
    client.serial = serial
    client.timeout = 5.0
    client.hash_timeout = 30.0
    client.parser = parser
    client.request_id = 1
    client.capabilities = frozenset()
    return client, serial


def run_control_case(emu: wire.Emulator, fault: str) -> None:
    """Verify W=2 sender semantic handling without changing the wire format."""
    data = payload()
    client, serial = make_client(emu, fault)
    started = time.monotonic()
    try:
        client.sync()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / f"e2-control-{fault}.bin"
            source.write_bytes(data)
            control_path = "/upload/e2-control.bin"
            try:
                metrics = client.upload(
                    control_path, source, replace=True,
                    encoding="raw", transport="windowed-gbn-v1", window_frames=2,
                )
            except RemoteError as error:
                if fault not in CONTROL_PROTOCOL_ERROR_CASES or error.code != "PROTOCOL_ERROR":
                    raise
                if serial.data_frames:
                    try:
                        client.request(
                            "binary.transfer.abort",
                            {"transfer_id": int(serial.data_frames[0]["transfer_id"])},
                        )
                    except RemoteError:
                        pass
                if len(client.parser.injections) != 1:
                    raise AssertionError(
                        f"W=2 protocol case did not inject one control: {fault}"
                    )
                expected_frontier_frames = 2 if fault in CONTROL_RAW_CASES else 3
                if len(serial.data_frames) != expected_frontier_frames:
                    raise AssertionError(
                        f"W=2 protocol case advanced beyond its bounded frontier: "
                        f"fault={fault} frames={len(serial.data_frames)} "
                        f"expected={expected_frontier_frames}"
                    )
                print(
                    "E2_W2_CONTROL_METRICS "
                    f"case={fault} result=PROTOCOL_ERROR "
                    f"data_frames={len(serial.data_frames)} "
                    f"elapsed={time.monotonic() - started:.3f}s"
                )
                return
            if fault not in CONTROL_SUCCESS_CASES:
                raise AssertionError(f"{fault} unexpectedly completed")
            if metrics.transport != "windowed-gbn-v1" or metrics.window_frames != 2:
                raise AssertionError(f"W=2 control case lost transport selection: {metrics}")
            if client.parser.injections and fault in {
                CONTROL_STALE_ACK, CONTROL_STALE_NACK,
            } and metrics.stale_control_frames == 0:
                raise AssertionError(f"ignored W=2 control was not counted stale: {fault}")
            if len(serial.data_frames) != DATA_FRAMES:
                raise AssertionError(f"ignored W=2 control changed DATA trace: {fault}")
        print(
            "E2_W2_CONTROL_METRICS "
            f"case={fault} result=COMPLETED data_frames={len(serial.data_frames)} "
            f"stale={metrics.stale_control_frames} "
            f"elapsed={time.monotonic() - started:.3f}s"
        )
    finally:
        client.close()


def assert_identity_replay(data_frames: list[dict]) -> None:
    first_by_identity: dict[tuple[int, int], dict] = {}
    for frame in data_frames:
        identity = (int(frame["sequence"]), int(frame["offset"]))
        first = first_by_identity.setdefault(identity, frame)
        for field in ("transfer_id", "sequence", "offset", "payload", "wire_crc"):
            if frame[field] != first[field]:
                raise AssertionError(
                    f"replayed DATA changed {field}: {frame[field]!r} != {first[field]!r}"
                )


def run_case(emu: wire.Emulator, fault: str) -> None:
    data = payload()
    expected_sha = hashlib.sha256(data).hexdigest()
    client, serial = make_client(emu, fault)
    started = time.monotonic()
    try:
        client.sync()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / f"e2-{fault}.bin"
            source.write_bytes(data)
            metrics = client.upload(
                f"/upload/e2-{fault}.bin", source, replace=True,
                encoding="raw", transport="windowed-gbn-v1", window_frames=2,
            )
        status = client.request("file.transfer.status", {
            "transfer_id": int(serial.data_frames[0]["transfer_id"]),
        })
        stat = client.stat(f"/upload/e2-{fault}.bin")
        hashed = client.sha256(f"/upload/e2-{fault}.bin")
        if (
            metrics.transport != "windowed-gbn-v1"
            or metrics.window_frames != DATA_WINDOW_FRAMES
            or metrics.max_outstanding_frames != 2
            or metrics.frames != DATA_FRAMES
            or metrics.actual_data_transmissions != len(serial.data_frames)
            or status.get("file_state") != "completed"
            or status.get("transferred_bytes") != PAYLOAD_BYTES
            or stat.get("size_bytes") != PAYLOAD_BYTES
            or hashed.get("sha256") != expected_sha
        ):
            raise AssertionError(
                f"W=2 result mismatch fault={fault} metrics={metrics} "
                f"status={status} stat={stat} hash={hashed} "
                f"frames={len(serial.data_frames)}"
            )
        assert_identity_replay(serial.data_frames)
        if fault == FAULT_NORMAL:
            if serial.data_send_parser_queue[:2] != [0, 0]:
                raise AssertionError(
                    "the second DATA was not sent before host processed an ACK: "
                    f"queue={serial.data_send_parser_queue[:2]}"
                )
            if len(serial.data_frames) != DATA_FRAMES:
                raise AssertionError("normal W=2 unexpectedly retransmitted DATA")
        elif fault == FAULT_ACK0_LOSS:
            if len(client.parser.dropped_frames) != 1 or len(serial.data_frames) != DATA_FRAMES:
                raise AssertionError("cumulative ACK did not retire the first window")
        elif fault == FAULT_WINDOW_ACK_LOSS:
            if len(client.parser.dropped_frames) != 2:
                raise AssertionError("full-window ACK loss was not injected")
            if len(serial.data_frames) != DATA_FRAMES + DATA_WINDOW_FRAMES:
                raise AssertionError("full-window timeout did not replay both DATA frames")
            if metrics.retransmitted_data_frames != DATA_WINDOW_FRAMES:
                raise AssertionError("full-window timeout did not replay two DATA frames")
        elif fault == FAULT_DATA0_LOSS:
            if len(serial.dropped_data) != 1 or len(serial.data_frames) != DATA_FRAMES + DATA_WINDOW_FRAMES:
                raise AssertionError("DATA gap did not trigger go-back-N replay")
            if metrics.nacks == 0:
                raise AssertionError("DATA gap did not produce a NACK")
            if metrics.retransmitted_data_frames != DATA_WINDOW_FRAMES:
                raise AssertionError("DATA gap did not replay the full outstanding window")
        elif fault == FAULT_DATA1_CORRUPT:
            if len(serial.corrupted_data) != 1 or len(serial.data_frames) != DATA_FRAMES + 1:
                raise AssertionError(
                    "corrupt second DATA did not retry from DATA1 frontier: "
                    f"corrupted={len(serial.corrupted_data)} frames={len(serial.data_frames)} "
                    f"trace={[(f.get('sequence'), f.get('offset')) for f in serial.data_frames]} "
                    f"metrics={metrics}"
                )
            if metrics.nacks == 0:
                raise AssertionError("corrupt DATA did not produce a NACK")
            if metrics.retransmitted_data_frames != 1:
                raise AssertionError("corrupt DATA did not retry from DATA1 only")
        elif fault == FAULT_FINAL_ACK_LOSS:
            if len(client.parser.dropped_frames) != 2:
                raise AssertionError("final-window ACK loss was not injected")
            final_replays = [
                frame for frame in serial.data_frames
                if frame.get("sequence") in FINAL_WINDOW_SEQUENCES
            ]
            if len(final_replays) != DATA_WINDOW_FRAMES * 2:
                raise AssertionError("final window was not replayed in full")
            if metrics.protocol_timeouts != 1:
                raise AssertionError("final-window replay did not use one RTO")
            if metrics.retransmitted_data_frames != DATA_WINDOW_FRAMES:
                raise AssertionError("final-window timeout did not replay two DATA frames")
        print(
            "E2_W2_GBN_METRICS "
            f"case={fault} logical_frames={metrics.frames} "
            f"actual_transmissions={metrics.actual_data_transmissions} "
            f"max_outstanding={metrics.max_outstanding_frames} "
            f"cumulative_acks={metrics.cumulative_acks} nacks={metrics.nacks} "
            f"retransmitted_data={metrics.retransmitted_data_frames} "
            f"retransmission_rounds={metrics.retransmission_rounds} "
            f"timeouts={metrics.protocol_timeouts} stale={metrics.stale_control_frames} "
            f"elapsed={time.monotonic() - started:.3f}s sha256={expected_sha}"
        )
    finally:
        client.close()


def main() -> int:
    args = parse_args()
    run_host_negotiation_unit_cases()
    emu = wire.Emulator(args)
    error: BaseException | None = None
    exit_status: int | None = None
    try:
        emu.start()
        emu.wait_line(wire.READY_MARKER, timeout=10.0)
        run_negotiation_cases(emu)
        for fault in (
            FAULT_NORMAL,
            FAULT_ACK0_LOSS,
            FAULT_WINDOW_ACK_LOSS,
            FAULT_DATA0_LOSS,
            FAULT_DATA1_CORRUPT,
            FAULT_FINAL_ACK_LOSS,
        ):
            run_case(emu, fault)
        for fault in (
            CONTROL_STALE_ACK,
            CONTROL_STALE_NACK,
            CONTROL_FUTURE_ACK,
            CONTROL_MISMATCHED_ACK,
            CONTROL_MISMATCHED_NACK,
            CONTROL_WRONG_TRANSFER_ACK,
            CONTROL_CORRUPT_ACK,
            CONTROL_RAW_ACK_PAYLOAD,
            CONTROL_RAW_ACK_STATUS,
            CONTROL_RAW_NACK_PAYLOAD,
            CONTROL_RAW_NACK_ZERO_STATUS,
            CONTROL_RAW_NACK_INVALID_STATUS,
        ):
            run_control_case(emu, fault)
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
            "UART bytes (hex):\n" + " ".join(f"{byte:02x}" for byte in emu.raw)
            + "\n\nesp-emu output:\n"
            + process_output.decode("utf-8", errors="replace"),
            encoding="utf-8",
        )
    if error is not None:
        traceback.print_exception(error)
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    if exit_status not in (0, -15, -9):
        print(f"ERROR: esp-emu exit status was {exit_status}", file=sys.stderr)
        return 1
    print("PASS: FILE TRANSFER OPT-IN W=2 RAW WINDOWED GBN")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
