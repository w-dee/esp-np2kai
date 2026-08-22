#!/usr/bin/env python3
"""W=1 sender validation regressions for stale and malformed ACK/NACK frames."""

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

from file_transfer_ack_loss_test import RecordingEmulatorSerial
from np2_fixture_cache import RemoteError, SerialFileTransferClient, _SerialParser
import uart_binary_data_plane_test as wire


PAYLOAD_BYTES = 4097
NACK_BAD_CRC = 1

CASE_DUPLICATE_ACK = "duplicate-ack"
CASE_STALE_ACK = "stale-ack"
CASE_FUTURE_ACK = "future-ack"
CASE_STALE_NACK = "stale-nack"
CASE_MISMATCHED_NACK = "mismatched-nack"
CASE_WRONG_TRANSFER_ACK = "wrong-transfer-ack"
CASE_CORRUPT_ACK = "corrupt-ack"
CASE_INVALID_ACK = "invalid-ack"
CASE_RAW_ACK_PAYLOAD = "raw-ack-payload"
CASE_RAW_ACK_STATUS = "raw-ack-status"
CASE_RAW_NACK_PAYLOAD = "raw-nack-payload"
CASE_RAW_NACK_ZERO_STATUS = "raw-nack-zero-status"
CASE_RAW_NACK_INVALID_STATUS = "raw-nack-invalid-status"

SUCCESS_CASES = {
    CASE_DUPLICATE_ACK,
    CASE_STALE_ACK,
    CASE_STALE_NACK,
    CASE_WRONG_TRANSFER_ACK,
    CASE_CORRUPT_ACK,
}
PROTOCOL_ERROR_CASES = {
    CASE_FUTURE_ACK,
    CASE_MISMATCHED_NACK,
    CASE_INVALID_ACK,
    CASE_RAW_ACK_PAYLOAD,
    CASE_RAW_ACK_STATUS,
    CASE_RAW_NACK_PAYLOAD,
    CASE_RAW_NACK_ZERO_STATUS,
    CASE_RAW_NACK_INVALID_STATUS,
}

RAW_SEMANTIC_CASES = {
    CASE_RAW_ACK_PAYLOAD,
    CASE_RAW_ACK_STATUS,
    CASE_RAW_NACK_PAYLOAD,
    CASE_RAW_NACK_ZERO_STATUS,
    CASE_RAW_NACK_INVALID_STATUS,
}


def parsed_control_frame(frame_type: int, transfer_id: int, sequence: int,
                         offset: int, status: int = 0) -> dict:
    encoded = wire.build_frame(
        frame_type, transfer_id, sequence, offset, status=status
    )
    return wire.parse_frame(wire.cobs_decode(encoded[2:-1]))


class InjectingAckParser(_SerialParser):
    """Append one delayed firmware response after a real ACK is parsed."""

    def __init__(self, fault: str) -> None:
        super().__init__()
        self.fault = fault
        self.injections: list[dict] = []

    def _injection(self, frame: dict) -> dict | None:
        transfer_id = int(frame["transfer_id"])
        sequence = int(frame["sequence"])
        offset = int(frame["offset"])
        if frame.get("type") != wire.ACK:
            return None
        if self.injections:
            return None

        if self.fault == CASE_DUPLICATE_ACK and (sequence, offset) == (1, 1024):
            return dict(frame)
        if self.fault == CASE_STALE_ACK and (sequence, offset) == (2, 2048):
            return parsed_control_frame(wire.ACK, transfer_id, 1, 1024)
        if self.fault == CASE_FUTURE_ACK and (sequence, offset) == (1, 1024):
            return parsed_control_frame(wire.ACK, transfer_id, 3, 3072)
        if self.fault == CASE_STALE_NACK and (sequence, offset) == (2, 2048):
            return parsed_control_frame(
                wire.NACK, transfer_id, 0, 0, status=NACK_BAD_CRC
            )
        if self.fault == CASE_MISMATCHED_NACK and (sequence, offset) == (1, 1024):
            return parsed_control_frame(
                wire.NACK, transfer_id, 1, 1025, status=NACK_BAD_CRC
            )
        if self.fault == CASE_WRONG_TRANSFER_ACK and (sequence, offset) == (1, 1024):
            return parsed_control_frame(wire.ACK, transfer_id + 1, 2, 2048)
        if self.fault == CASE_CORRUPT_ACK and (sequence, offset) == (1, 1024):
            corrupt = dict(parsed_control_frame(wire.ACK, transfer_id, 2, 2048))
            corrupt["crc_valid"] = False
            return corrupt
        if self.fault == CASE_INVALID_ACK and (sequence, offset) == (1, 1024):
            invalid = dict(frame)
            invalid["status"] = 1
            invalid["crc_valid"] = True
            return invalid
        return None

    def feed(self, data: bytes) -> None:
        before = len(self.frames)
        super().feed(data)
        for frame in list(self.frames)[before:]:
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


class EncodedSemanticValidationParser(_SerialParser):
    """Inject one malformed encoded control frame through the production parser."""

    def __init__(self, fault: str) -> None:
        super().__init__()
        self.fault = fault
        self.injections: list[dict] = []

    def _raw_frame(self, frame: dict) -> bytes:
        transfer_id = int(frame["transfer_id"])
        if self.fault == CASE_RAW_ACK_PAYLOAD:
            return wire.build_unchecked_control_frame(
                wire.ACK, transfer_id, 1, 1024, payload=b"unexpected-ack-payload"
            )
        if self.fault == CASE_RAW_ACK_STATUS:
            return wire.build_unchecked_control_frame(
                wire.ACK, transfer_id, 1, 1024, status=1
            )
        if self.fault == CASE_RAW_NACK_PAYLOAD:
            return wire.build_unchecked_control_frame(
                wire.NACK, transfer_id, 0, 0, status=NACK_BAD_CRC,
                payload=b"unexpected-nack-payload",
            )
        if self.fault == CASE_RAW_NACK_ZERO_STATUS:
            return wire.build_unchecked_control_frame(
                wire.NACK, transfer_id, 0, 0, status=0
            )
        if self.fault == CASE_RAW_NACK_INVALID_STATUS:
            return wire.build_unchecked_control_frame(
                wire.NACK, transfer_id, 0, 0, status=0xFFFF
            )
        raise AssertionError(f"unsupported encoded semantic case: {self.fault}")

    def feed(self, data: bytes) -> None:
        before = len(self.frames)
        super().feed(data)
        if self.injections:
            return
        for index in range(before, len(self.frames)):
            frame = self.frames[index]
            if (
                frame.get("type") == wire.ACK
                and frame.get("sequence") == 2
                and frame.get("offset") == 2048
            ):
                real_ack = self.frames[index]
                raw = self._raw_frame(real_ack)
                try:
                    wire.parse_frame(wire.cobs_decode(raw[2:-1]))
                except (AssertionError, ValueError):
                    pass
                else:
                    if self.fault != CASE_RAW_NACK_INVALID_STATUS:
                        raise AssertionError(
                            "canonical parser unexpectedly accepted malformed control frame"
                        )
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
                    f"crc_valid={injected.get('crc_valid')}"
                )
                return


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
    args.emulator_timeout = "90s"
    args.process_timeout = 100.0
    return args


def payload() -> bytes:
    return bytes(
        (index * 73 + (index >> 8) * 19 + 0x41) & 0xFF
        for index in range(PAYLOAD_BYTES)
    )


def make_client(emu: wire.Emulator, fault: str) -> tuple[
    SerialFileTransferClient, RecordingEmulatorSerial
]:
    serial = RecordingEmulatorSerial(emu)
    client = object.__new__(SerialFileTransferClient)
    client.serial = serial
    client.timeout = 5.0
    client.hash_timeout = 30.0
    client.parser = (
        EncodedSemanticValidationParser(fault)
        if fault in RAW_SEMANTIC_CASES else InjectingAckParser(fault)
    )
    client.request_id = 1
    client.capabilities = frozenset()
    return client, serial


def run_case(emu: wire.Emulator, fault: str) -> None:
    data = payload()
    expected_sha = hashlib.sha256(data).hexdigest()
    client, serial = make_client(emu, fault)
    transfer_ids: list[int] = []
    original_request = client.request

    def recording_request(command: str, params: dict | None = None,
                          timeout: float | None = None) -> dict:
        result = original_request(command, params, timeout)
        if command == "file.write.begin":
            transfer_ids.append(int(result["transfer_id"]))
        return result

    client.request = recording_request
    path = f"/upload/e1c-{fault}.bin"
    started = time.monotonic()
    try:
        client.sync()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "e1c-ack-validation.bin"
            source.write_bytes(data)
            try:
                metrics = client.upload(path, source, replace=True, encoding="raw")
            except RemoteError as error:
                if fault not in PROTOCOL_ERROR_CASES or error.code != "PROTOCOL_ERROR":
                    raise
                if len(transfer_ids) != 1:
                    raise AssertionError(f"unexpected transfer IDs: {transfer_ids}")
                if len(serial.data_frames) != 2:
                    raise AssertionError(
                        "protocol error caused unexpected DATA trace: "
                        f"frames={len(serial.data_frames)} trace="
                        f"{[(f.get('sequence'), f.get('offset')) for f in serial.data_frames]}"
                    )
                client.request(
                    "binary.transfer.abort", {"transfer_id": transfer_ids[0]}
                )
                print(
                    "E1C_ACK_VALIDATION_METRICS "
                    f"case={fault} result=PROTOCOL_ERROR data_frames={len(serial.data_frames)} "
                    f"injected={len(client.parser.injections)} "
                    f"elapsed={time.monotonic() - started:.3f}s"
                )
                return
            if fault not in SUCCESS_CASES:
                raise AssertionError(f"{fault} unexpectedly completed")

        if len(transfer_ids) != 1:
            raise AssertionError(f"unexpected transfer IDs: {transfer_ids}")
        binary = client.request("binary.transfer.status", {"transfer_id": transfer_ids[0]})
        file_status = client.request("file.transfer.status", {"transfer_id": transfer_ids[0]})
        stat = client.stat(path)
        hashed = client.sha256(path)
        if (
            metrics.frames != 5
            or metrics.retries != 0
            or metrics.nacks != 0
            or metrics.protocol_timeouts != 0
            or len(serial.data_frames) != 5
            or binary.get("state") != "completed"
            or binary.get("transferred_bytes") != PAYLOAD_BYTES
            or file_status.get("file_state") != "completed"
            or file_status.get("transferred_bytes") != PAYLOAD_BYTES
            or stat.get("size_bytes") != PAYLOAD_BYTES
            or hashed.get("size_bytes") != PAYLOAD_BYTES
            or hashed.get("sha256") != expected_sha
        ):
            raise AssertionError(
                "ACK validation result mismatch: "
                f"metrics={metrics} data_frames={len(serial.data_frames)} "
                f"binary={binary} file={file_status} stat={stat} sha={hashed}"
            )
        print(
            "E1C_ACK_VALIDATION_METRICS "
            f"case={fault} result=COMPLETED data_frames={len(serial.data_frames)} "
            f"duplicate_frames={metrics.duplicate_frames} injected={len(client.parser.injections)} "
            f"elapsed={time.monotonic() - started:.3f}s final_size={stat['size_bytes']} "
            f"sha256={hashed['sha256']}"
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
        for fault in (
            CASE_DUPLICATE_ACK,
            CASE_STALE_ACK,
            CASE_FUTURE_ACK,
            CASE_STALE_NACK,
            CASE_MISMATCHED_NACK,
            CASE_WRONG_TRANSFER_ACK,
            CASE_CORRUPT_ACK,
            CASE_INVALID_ACK,
            CASE_RAW_ACK_PAYLOAD,
            CASE_RAW_ACK_STATUS,
            CASE_RAW_NACK_PAYLOAD,
            CASE_RAW_NACK_ZERO_STATUS,
            CASE_RAW_NACK_INVALID_STATUS,
        ):
            run_case(emu, fault)
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
    print("PASS: FILE TRANSFER W=1 ACK/NACK VALIDATION")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
