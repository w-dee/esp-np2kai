#!/usr/bin/env python3
"""W=1 raw upload regression with one deterministic non-final ACK loss."""

from __future__ import annotations

import argparse
from collections.abc import Callable
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import time
import traceback

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from np2_fixture_cache import SerialFileTransferClient, _SerialParser
import uart_binary_data_plane_test as wire


PAYLOAD_BYTES = 4097
TARGET_SEQUENCE = 2
TARGET_OFFSET = 2048
TARGET_DATA_SEQUENCE = TARGET_SEQUENCE - 1
TARGET_DATA_OFFSET = TARGET_OFFSET - 1024


class DropOnceAckParser(_SerialParser):
    """Drop exactly one parsed frame matching a caller-supplied predicate."""

    def __init__(self, predicate: Callable[[dict], bool]) -> None:
        super().__init__()
        self.predicate = predicate
        self.dropped_frames: list[dict] = []

    def feed(self, data: bytes) -> None:
        super().feed(data)
        if self.dropped_frames:
            return
        for index, frame in enumerate(self.frames):
            if (
                frame.get("type") == wire.ACK
                and self.predicate(frame)
            ):
                del self.frames[index]
                self.dropped_frames.append(frame)
                print(
                    "FAULT_INJECT "
                    "action=drop-once direction=firmware-to-host type=ACK "
                    f"transfer_id={frame['transfer_id']} "
                    f"sequence={frame['sequence']} offset={frame['offset']} "
                    f"wire_crc=0x{frame['wire_crc']:08x}"
                )
                return


class RecordingEmulatorSerial:
    """pyserial-compatible adapter that records DATA sent by the host."""

    def __init__(self, emu: wire.Emulator) -> None:
        if emu.sock is None:
            raise AssertionError("emu UART socket is not connected")
        self.sock = emu.sock
        self.buffer = bytearray()
        self.tx_parser = wire.StreamParser()
        self.data_frames: list[dict] = []

    def _fill(self) -> None:
        if self.buffer:
            return
        try:
            data = self.sock.recv(65536)
        except BlockingIOError:
            return
        if not data:
            raise AssertionError("esp-emu closed the UART-TCP socket")
        self.buffer.extend(data)

    @property
    def in_waiting(self) -> int:
        self._fill()
        return len(self.buffer)

    def read(self, size: int) -> bytes:
        self._fill()
        data = bytes(self.buffer[:size])
        del self.buffer[:size]
        return data

    def write(self, data: bytes) -> int:
        self.tx_parser.feed(data)
        while self.tx_parser.frames:
            frame = self.tx_parser.frames.popleft()
            if frame.get("type") == wire.DATA:
                self.data_frames.append(frame)
        self.sock.sendall(data)
        return len(data)

    def close(self) -> None:
        pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--uart-log", required=True, type=Path)
    parser.add_argument(
        "--esp-emu",
        default=os.environ.get("ESP_EMU", str(Path.home() / ".local/bin/esp-emu")),
    )
    parser.add_argument(
        "--controlled-data-ack-timeout",
        type=float,
        default=None,
        help="cap client frame waits for pre-fix evidence; normal runs use production timing",
    )
    args = parser.parse_args()
    args.emulator_timeout = "90s"
    args.process_timeout = 100.0
    return args


def payload() -> bytes:
    return bytes(
        (index * 73 + (index >> 8) * 19 + 0x41) & 0xFF
        for index in range(PAYLOAD_BYTES)
    )


def run(emu: wire.Emulator, args: argparse.Namespace) -> None:
    data = payload()
    expected_sha = hashlib.sha256(data).hexdigest()
    serial = RecordingEmulatorSerial(emu)
    client = object.__new__(SerialFileTransferClient)
    client.serial = serial
    client.timeout = 5.0
    client.hash_timeout = 30.0
    client.parser = DropOnceAckParser(
        lambda frame: (
            frame.get("sequence") == TARGET_SEQUENCE
            and frame.get("offset") == TARGET_OFFSET
        )
    )
    client.request_id = 1
    client.capabilities = frozenset()
    transfer_ids: list[int] = []

    original_request = client.request

    def recording_request(command: str, params: dict | None = None,
                          timeout: float | None = None) -> dict:
        result = original_request(command, params, timeout)
        if command == "file.write.begin":
            transfer_ids.append(int(result["transfer_id"]))
        return result

    client.request = recording_request
    if args.controlled_data_ack_timeout is not None:
        original_wait_frame = client.wait_frame

        def controlled_wait_frame(timeout: float) -> dict:
            return original_wait_frame(
                min(timeout, args.controlled_data_ack_timeout)
            )

        client.wait_frame = controlled_wait_frame

    started = time.monotonic()
    try:
        client.sync()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "e1-ack-loss.bin"
            source.write_bytes(data)
            metrics = client.upload(
                "/upload/e1-ack-loss.bin",
                source,
                replace=False,
                encoding="raw",
            )

        if len(transfer_ids) != 1:
            raise AssertionError(f"unexpected transfer IDs: {transfer_ids}")
        if len(client.parser.dropped_frames) != 1:
            raise AssertionError(
                f"expected exactly one dropped ACK: {client.parser.dropped_frames}"
            )

        retries = [
            frame for frame in serial.data_frames
            if frame.get("sequence") == TARGET_DATA_SEQUENCE
        ]
        if len(serial.data_frames) != 6 or len(retries) != 2:
            raise AssertionError(
                "unexpected DATA send trace: "
                f"frames={len(serial.data_frames)} target_replays={len(retries)} "
                f"trace={[ (frame.get('sequence'), frame.get('offset')) for frame in serial.data_frames ]}"
            )
        first, replay = retries
        if (
            first["sequence"] != TARGET_DATA_SEQUENCE
            or first["offset"] != TARGET_DATA_OFFSET
        ):
            raise AssertionError(
                f"unexpected retry target: sequence={first['sequence']} "
                f"offset={first['offset']}"
            )
        for field in ("transfer_id", "sequence", "offset", "payload", "wire_crc"):
            if first[field] != replay[field]:
                raise AssertionError(
                    f"retry changed DATA field {field}: "
                    f"{first[field]!r} != {replay[field]!r}"
                )
        if not first["crc_valid"] or not replay["crc_valid"]:
            raise AssertionError("DATA retry CRC was not valid")

        binary = client.request(
            "binary.transfer.status", {"transfer_id": transfer_ids[0]}
        )
        file_status = client.request(
            "file.transfer.status", {"transfer_id": transfer_ids[0]}
        )
        stat = client.stat("/upload/e1-ack-loss.bin")
        hashed = client.sha256("/upload/e1-ack-loss.bin")
        if (
            metrics.frames != 5
            or metrics.retries != 1
            or metrics.protocol_timeouts != 1
            or metrics.nacks != 0
            or binary.get("state") != "completed"
            or binary.get("transferred_bytes") != PAYLOAD_BYTES
            or file_status.get("file_state") != "completed"
            or file_status.get("transferred_bytes") != PAYLOAD_BYTES
            or stat.get("size_bytes") != PAYLOAD_BYTES
            or hashed.get("size_bytes") != PAYLOAD_BYTES
            or hashed.get("sha256") != expected_sha
        ):
            raise AssertionError(
                "ACK-loss result mismatch: "
                f"metrics={metrics} binary={binary} file={file_status} "
                f"stat={stat} sha={hashed}"
            )
        print(
            "E1_ACK_LOSS_METRICS "
            f"payload_bytes={PAYLOAD_BYTES} data_frames={metrics.frames} "
            f"dropped_ack={len(client.parser.dropped_frames)} "
            f"retransmitted_data={len(retries) - 1} nacks={metrics.nacks} "
            f"protocol_timeouts={metrics.protocol_timeouts} "
            f"elapsed={time.monotonic() - started:.3f}s "
            f"final_size={stat['size_bytes']} sha256={hashed['sha256']}"
        )
    finally:
        client.close()


def main() -> int:
    args = parse_args()
    emu = wire.Emulator(args)
    error: BaseException | None = None
    exit_status: int | None = None
    try:
        emu.start()
        emu.wait_line(wire.READY_MARKER, timeout=10.0)
        run(emu, args)
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
    print("PASS: FILE TRANSFER W=1 NON-FINAL ACK-LOSS RELIABILITY")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
