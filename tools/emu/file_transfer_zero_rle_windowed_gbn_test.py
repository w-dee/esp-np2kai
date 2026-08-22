#!/usr/bin/env python3
"""Opt-in zero-rle-v1 upload over the bounded W=2 go-back-N transport."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import time
import traceback

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from file_transfer_windowed_gbn_test import (
    DropAckParser,
    FAULT_ACK0_LOSS,
    FAULT_DATA0_LOSS,
    FAULT_DATA1_CORRUPT,
    FAULT_FINAL_ACK_LOSS,
    FAULT_NORMAL,
    FAULT_WINDOW_ACK_LOSS,
    WindowFaultSerial,
    assert_identity_replay,
)
from np2_fixture_cache import (
    CAPABILITY_WINDOWED_GBN_V1,
    CAPABILITY_ZERO_RLE_V1,
    DATA_WINDOW_FRAMES,
    ENCODING_AUTO,
    ENCODING_ZERO_RLE_V1,
    SerialFileTransferClient,
    SourceChangedError,
    _ZeroRleProducer,
    analyze_zero_rle,
)
import uart_binary_data_plane_test as wire


LOGICAL_BYTES = 32 * 1024
ACK_TIMEOUT = 5.0


class MutatingWindowSerial(WindowFaultSerial):
    """Change the source after the final retained encoded DATA is sent."""

    def __init__(self, emu: wire.Emulator, source: Path, replacement: bytes,
                 mutate_after_frames: int) -> None:
        super().__init__(emu, FAULT_NORMAL)
        self.source = source
        self.replacement = replacement
        self.mutate_after_frames = mutate_after_frames
        self.mutated = False

    def write(self, data: bytes) -> int:
        result = super().write(data)
        if len(self.data_frames) >= self.mutate_after_frames and not self.mutated:
            self.source.write_bytes(self.replacement)
            self.mutated = True
            print(
                "FAULT_INJECT action=mutate-source "
                f"after-data-count={self.mutate_after_frames}"
            )
        return result


class RecordingDropAckParser(DropAckParser):
    """Keep NACK frontiers visible while retaining E2 ACK-loss behavior."""

    def __init__(self, predicate, count: int) -> None:
        super().__init__(predicate, count)
        self.nack_frames: list[dict] = []
        self._seen_nack_ids: set[int] = set()

    def feed(self, data: bytes) -> None:
        super().feed(data)
        for frame in self.frames:
            if frame.get("type") != wire.NACK or id(frame) in self._seen_nack_ids:
                continue
            self._seen_nack_ids.add(id(frame))
            self.nack_frames.append(frame)


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
    args.process_timeout = 200.0
    return args


def logical_payload() -> bytes:
    block = bytes(((index % 251) + 1) for index in range(180)) + b"\x00" * 332
    return block * (LOGICAL_BYTES // len(block))


def request(emu: wire.Emulator, request_id: int, command: str,
            params: dict | None = None) -> dict:
    emu.send_json(request_id, command, params)
    return emu.wait_response(request_id, timeout=30.0)


def require_ok(response: dict, request_id: int) -> dict:
    if response.get("id") != request_id or response.get("ok") is not True:
        raise AssertionError(f"request {request_id} failed: {response}")
    result = response.get("result")
    if not isinstance(result, dict):
        raise AssertionError(f"request {request_id} result is invalid: {response}")
    return result


def make_client(emu: wire.Emulator, fault: str, data_frames: int,
                source: Path | None = None, replacement: bytes | None = None) -> tuple[
                    SerialFileTransferClient, WindowFaultSerial
                ]:
    if source is not None:
        if replacement is None:
            raise AssertionError("source mutation replacement is required")
        serial: WindowFaultSerial = MutatingWindowSerial(
            emu, source, replacement, data_frames
        )
        parser = RecordingDropAckParser(lambda _frame: False, 0)
    else:
        ack_predicate = lambda _frame: False
        ack_count = 0
        if fault == FAULT_ACK0_LOSS:
            ack_predicate = lambda frame: frame.get("sequence") == 1
            ack_count = 1
        elif fault == FAULT_WINDOW_ACK_LOSS:
            ack_predicate = lambda frame: frame.get("sequence") in {1, 2}
            ack_count = 2
        elif fault == FAULT_DATA1_CORRUPT:
            ack_predicate = lambda frame: frame.get("sequence") == 1
            ack_count = 1
        elif fault == FAULT_FINAL_ACK_LOSS:
            ack_predicate = lambda frame: frame.get("sequence") in {
                data_frames - 1, data_frames,
            }
            ack_count = 2
        serial = WindowFaultSerial(emu, fault)
        parser = RecordingDropAckParser(ack_predicate, ack_count)
    serial.host_parser = parser
    client = object.__new__(SerialFileTransferClient)
    client.serial = serial
    client.timeout = 5.0
    client.hash_timeout = 30.0
    client.parser = parser
    client.request_id = 1
    client.capabilities = frozenset()
    return client, serial


def assert_transport_offsets(frames: list[dict], wire_size: int) -> None:
    first_by_sequence: dict[int, dict] = {}
    for frame in frames:
        sequence = int(frame["sequence"])
        first_by_sequence.setdefault(sequence, frame)
    expected_offset = 0
    for sequence in sorted(first_by_sequence):
        frame = first_by_sequence[sequence]
        if frame["offset"] != expected_offset:
            raise AssertionError(
                f"encoded transport offset gap at sequence={sequence}: {frame}"
            )
        expected_offset += len(frame["payload"])
    if expected_offset != wire_size:
        raise AssertionError(
            f"encoded transport bytes mismatch: {expected_offset} != {wire_size}"
        )


def run_case(emu: wire.Emulator, logical: bytes, fault: str,
             encoding: str = ENCODING_ZERO_RLE_V1,
             label: str | None = None) -> None:
    expected_sha = hashlib.sha256(logical).hexdigest()
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / f"e4-{fault}.bin"
        source.write_bytes(logical)
        plan = analyze_zero_rle(source)
        data_frames = (plan.wire_size_bytes + wire.MAX_PAYLOAD - 1) // wire.MAX_PAYLOAD
        if data_frames < 6:
            raise AssertionError(f"combined fixture has too few encoded frames: {plan}")
        client, serial = make_client(emu, fault, data_frames)
        started = time.monotonic()
        try:
            client.sync()
            case_label = label or fault
            metrics = client.upload(
                "/upload/e4-zero-rle-w2.bin", source, replace=True,
                encoding=encoding,
                transport="windowed-gbn-v1", window_frames=DATA_WINDOW_FRAMES,
            )
            transfer_id = int(serial.data_frames[0]["transfer_id"])
            status = client.request("file.transfer.status", {"transfer_id": transfer_id})
            stat = client.stat("/upload/e4-zero-rle-w2.bin")
            hashed = client.sha256("/upload/e4-zero-rle-w2.bin")
            if (
                metrics.encoding != ENCODING_ZERO_RLE_V1
                or metrics.transport != "windowed-gbn-v1"
                or metrics.window_frames != DATA_WINDOW_FRAMES
                or metrics.logical_size_bytes != len(logical)
                or metrics.logical_bytes_sent != len(logical)
                or metrics.wire_size_bytes != plan.wire_size_bytes
                or metrics.wire_bytes_sent != plan.wire_size_bytes
                or metrics.frames != data_frames
                or metrics.actual_data_transmissions != len(serial.data_frames)
                or metrics.max_outstanding_frames != DATA_WINDOW_FRAMES
                or status.get("file_state") != "completed"
                or status.get("size_bytes") != len(logical)
                or status.get("transferred_bytes") != len(logical)
                or status.get("wire_size_bytes") != plan.wire_size_bytes
                or status.get("wire_transferred_bytes") != plan.wire_size_bytes
                or stat.get("size_bytes") != len(logical)
                or hashed.get("sha256") != expected_sha
            ):
                raise AssertionError(
                    f"W=2 zero-rle result mismatch fault={fault} metrics={metrics} "
                    f"status={status} stat={stat} hash={hashed} "
                    f"frames={len(serial.data_frames)}"
                )
            assert_identity_replay(serial.data_frames)
            assert_transport_offsets(serial.data_frames, plan.wire_size_bytes)
            if fault == FAULT_NORMAL:
                if len(serial.data_frames) != data_frames:
                    raise AssertionError("normal compressed W=2 retransmitted DATA")
            elif fault == FAULT_ACK0_LOSS:
                if len(client.parser.dropped_frames) != 1:
                    raise AssertionError("first compressed ACK was not dropped")
                if len(serial.data_frames) != data_frames:
                    raise AssertionError("cumulative compressed ACK did not retire window")
            elif fault == FAULT_WINDOW_ACK_LOSS:
                if len(client.parser.dropped_frames) != 2:
                    raise AssertionError("full compressed window ACK loss was not injected")
                if metrics.retransmitted_data_frames != DATA_WINDOW_FRAMES:
                    raise AssertionError("compressed full-window timeout did not replay W=2")
            elif fault == FAULT_DATA0_LOSS:
                if len(serial.dropped_data) != 1 or metrics.nacks == 0:
                    raise AssertionError("first encoded DATA loss did not produce NACK/retry")
                if not client.parser.nack_frames or (
                        client.parser.nack_frames[0].get("sequence"),
                        client.parser.nack_frames[0].get("offset"),
                ) != (0, 0):
                    raise AssertionError(
                        f"encoded DATA0 gap NACK frontier was not (0, 0): "
                        f"{client.parser.nack_frames}"
                    )
                if metrics.retransmitted_data_frames != DATA_WINDOW_FRAMES:
                    raise AssertionError("encoded DATA0 loss did not replay full window")
            elif fault == FAULT_DATA1_CORRUPT:
                if len(serial.corrupted_data) != 1 or metrics.nacks == 0:
                    raise AssertionError("second encoded DATA corruption did not NACK")
                if metrics.retransmitted_data_frames != 1:
                    raise AssertionError("encoded DATA1 corruption did not use NACK frontier")
                if not client.parser.nack_frames or (
                        client.parser.nack_frames[0].get("sequence"),
                        client.parser.nack_frames[0].get("offset"),
                ) != (1, wire.MAX_PAYLOAD):
                    raise AssertionError(
                        "encoded DATA1 NACK did not identify the encoded transport frontier: "
                        f"{client.parser.nack_frames}"
                    )
                if sum(frame["sequence"] == 1 for frame in serial.data_frames) != 2:
                    raise AssertionError("encoded DATA1 corruption retried wrong frontier")
            elif fault == FAULT_FINAL_ACK_LOSS:
                if len(client.parser.dropped_frames) != 2:
                    raise AssertionError("final compressed ACK loss was not injected")
                if metrics.retransmitted_data_frames != DATA_WINDOW_FRAMES:
                    raise AssertionError("final compressed window was not replayed")
                if metrics.protocol_timeouts != 1:
                    raise AssertionError("final compressed replay used unexpected RTO count")
            print(
                "E4_ZERO_RLE_W2_METRICS "
                f"case={case_label} encoding={encoding} logical_bytes={len(logical)} "
                f"wire_bytes={plan.wire_size_bytes} encoded_frames={data_frames} "
                f"actual_transmissions={metrics.actual_data_transmissions} "
                f"retries={metrics.retries} nacks={metrics.nacks} "
                f"retransmitted_data={metrics.retransmitted_data_frames} "
                f"timeouts={metrics.protocol_timeouts} "
                f"cumulative_acks={metrics.cumulative_acks} "
                f"stale_control_frames={metrics.stale_control_frames} "
                f"elapsed={time.monotonic() - started:.3f}s sha256={expected_sha}"
            )
        finally:
            client.close()


def run_source_mutation_case(emu: wire.Emulator, logical: bytes) -> None:
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "e4-source-mutation.bin"
        replacement = bytes((byte ^ 0x01) if byte else 0x02 for byte in logical)
        source.write_bytes(logical)
        plan = analyze_zero_rle(source)
        data_frames = (plan.wire_size_bytes + wire.MAX_PAYLOAD - 1) // wire.MAX_PAYLOAD
        client, serial = make_client(
            emu, FAULT_NORMAL, data_frames, source=source, replacement=replacement
        )
        try:
            client.sync()
            try:
                client.upload(
                    "/upload/e4-zero-rle-w2.bin", source, replace=True,
                    encoding=ENCODING_ZERO_RLE_V1,
                    transport="windowed-gbn-v1", window_frames=DATA_WINDOW_FRAMES,
                )
            except SourceChangedError:
                pass
            else:
                raise AssertionError("W=2 zero-rle source mutation was accepted")
            if not serial.mutated:
                raise AssertionError("source mutation fault was not armed")
            print("E4_ZERO_RLE_W2_SOURCE_MUTATION result=REJECTED")
        finally:
            client.close()


def run_mid_transfer_dual_offset_case(emu: wire.Emulator, logical: bytes) -> None:
    """Observe distinct encoded transport and logical decoder frontiers."""
    path = "/upload/e4-zero-rle-w2.bin"
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "e4-mid-transfer.bin"
        source.write_bytes(logical)
        plan = analyze_zero_rle(source)
        producer = _ZeroRleProducer(plan)
        try:
            begin = require_ok(
                request(emu, 600, "file.write.begin", {
                    "path": path,
                    "size_bytes": plan.local.size_bytes,
                    "encoding": ENCODING_ZERO_RLE_V1,
                    "wire_size_bytes": plan.wire_size_bytes,
                    "replace": True,
                    "transport": "windowed-gbn-v1",
                    "window_frames": 2,
                }),
                600,
            )
            transfer_id = int(begin["transfer_id"])
            first = producer.read(wire.MAX_PAYLOAD)
            emu.send(wire.build_frame(
                wire.DATA, transfer_id, 0, 0, payload=first
            ))
            first_ack = emu.wait_frame(ACK_TIMEOUT)
            wire.require_ack(first_ack, transfer_id, 1, len(first))
            binary = require_ok(request(emu, 601, "binary.transfer.status", {
                "transfer_id": transfer_id,
            }), 601)
            file_status = require_ok(request(emu, 602, "file.transfer.status", {
                "transfer_id": transfer_id,
            }), 602)
            if (binary.get("transferred_bytes") != len(first) or
                    file_status.get("wire_transferred_bytes") != len(first) or
                    file_status.get("transferred_bytes") == len(first) or
                    file_status.get("transferred_bytes", 0) <= len(first)):
                raise AssertionError(
                    f"dual offset frontier was not distinct: binary={binary} "
                    f"file={file_status} first_wire={len(first)}"
                )
            offset = len(first)
            sequence = 1
            while offset < plan.wire_size_bytes:
                chunk = producer.read(
                    min(wire.MAX_PAYLOAD, plan.wire_size_bytes - offset)
                )
                if not chunk:
                    raise AssertionError("mid-transfer producer ended early")
                emu.send(wire.build_frame(
                    wire.DATA, transfer_id, sequence, offset, payload=chunk
                ))
                wire.require_ack(
                    emu.wait_frame(ACK_TIMEOUT), transfer_id,
                    sequence + 1, offset + len(chunk)
                )
                offset += len(chunk)
                sequence += 1
            producer.finish()
            completed = require_ok(request(emu, 603, "file.transfer.status", {
                "transfer_id": transfer_id,
            }), 603)
            hashed = require_ok(request(emu, 604, "file.sha256", {"path": path}), 604)
            if (completed.get("file_state") != "completed" or
                    completed.get("transferred_bytes") != len(logical) or
                    completed.get("wire_transferred_bytes") != plan.wire_size_bytes or
                    hashed.get("sha256") != hashlib.sha256(logical).hexdigest()):
                raise AssertionError(f"dual offset completion mismatch: {completed} {hashed}")
            print(
                "E4_ZERO_RLE_W2_DUAL_OFFSET "
                f"wire_frontier={len(first)} "
                f"logical_frontier={file_status.get('transferred_bytes')} "
                f"wire_size={plan.wire_size_bytes} logical_size={len(logical)}"
            )
        finally:
            producer.close()


def run_malformed_case(emu: wire.Emulator) -> None:
    path = "/upload/e4-malformed-zero-rle.bin"
    begin = require_ok(
        request(emu, 500, "file.write.begin", {
            "path": path,
            "size_bytes": 1,
            "encoding": ENCODING_ZERO_RLE_V1,
            "wire_size_bytes": 5,
            "transport": "windowed-gbn-v1",
            "window_frames": 2,
        }),
        500,
    )
    if (begin.get("encoding") != ENCODING_ZERO_RLE_V1 or
            begin.get("transport") != "windowed-gbn-v1" or
            begin.get("window_frames") != 2):
        raise AssertionError(f"malformed W=2 begin did not echo all fields: {begin}")
    transfer_id = int(begin["transfer_id"])
    emu.send(wire.build_frame(
        wire.DATA, transfer_id, 0, 0, payload=b"\x02\x01\x00\x00\x00"
    ))
    try:
        emu.wait_frame(0.5)
    except AssertionError:
        pass
    binary = require_ok(request(emu, 501, "binary.transfer.status", {
        "transfer_id": transfer_id,
    }), 501)
    file_status = require_ok(request(emu, 502, "file.transfer.status", {
        "transfer_id": transfer_id,
    }), 502)
    if binary.get("state") != "aborted" or file_status.get("file_state") != "failed":
        raise AssertionError(f"malformed W=2 stream did not abort: {binary} {file_status}")
    if file_status.get("error", {}).get("code") != "MALFORMED_ENCODING":
        raise AssertionError(f"malformed W=2 stream error mismatch: {file_status}")
    missing = request(emu, 503, "file.stat", {"path": path})
    if missing.get("ok") is not False or missing.get("error", {}).get("code") != "NOT_FOUND":
        raise AssertionError(f"malformed W=2 target was committed: {missing}")
    print("E4_ZERO_RLE_W2_MALFORMED result=ABORTED/MALFORMED_ENCODING")


def run(emu: wire.Emulator) -> None:
    hello = require_ok(request(emu, 1, "protocol.hello"), 1)
    capabilities = hello.get("capabilities", [])
    required = {CAPABILITY_ZERO_RLE_V1, CAPABILITY_WINDOWED_GBN_V1}
    if not required.issubset(capabilities):
        raise AssertionError(f"combined capability negotiation failed: {hello}")
    logical = logical_payload()
    with tempfile.TemporaryDirectory() as directory:
        probe = Path(directory) / "e4-probe.bin"
        probe.write_bytes(logical)
        plan = analyze_zero_rle(probe)
    if plan.wire_size_bytes <= 6 * wire.MAX_PAYLOAD:
        raise AssertionError(f"combined fixture is not a multi-window stream: {plan}")
    print(
        f"E4_ZERO_RLE_W2_PLAN logical_bytes={plan.local.size_bytes} "
        f"wire_bytes={plan.wire_size_bytes} "
        f"encoded_frames={(plan.wire_size_bytes + wire.MAX_PAYLOAD - 1) // wire.MAX_PAYLOAD}"
    )
    for fault in (
        FAULT_NORMAL,
        FAULT_ACK0_LOSS,
        FAULT_WINDOW_ACK_LOSS,
        FAULT_DATA0_LOSS,
        FAULT_DATA1_CORRUPT,
        FAULT_FINAL_ACK_LOSS,
    ):
        run_case(emu, logical, fault)
    run_case(emu, logical, FAULT_NORMAL, encoding=ENCODING_AUTO, label="auto")
    run_mid_transfer_dual_offset_case(emu, logical)
    run_source_mutation_case(emu, logical)
    run_malformed_case(emu)


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
    print("PASS: FILE TRANSFER ZERO-RLE-V1 W=2 WINDOWED GBN")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
