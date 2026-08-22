#!/usr/bin/env python3
"""Final DATA replay regressions for wire loss and firmware output failure."""

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

from file_transfer_ack_loss_test import DropOnceAckParser, RecordingEmulatorSerial
from np2_fixture_cache import SerialFileTransferClient, _SerialParser
import uart_binary_data_plane_test as wire


PAYLOAD_BYTES = 4097
FINAL_DATA_SEQUENCE = 4
FINAL_DATA_OFFSET = 4096
FINAL_ACK_SEQUENCE = 5
FINAL_ACK_OFFSET = PAYLOAD_BYTES
FAULT_OUTPUT = "output-failure"
FAULT_WIRE = "wire-loss"


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
    parser = (
        DropOnceAckParser(
            lambda frame: (
                frame.get("sequence") == FINAL_ACK_SEQUENCE
                and frame.get("offset") == FINAL_ACK_OFFSET
            )
        )
        if fault == FAULT_WIRE else _SerialParser()
    )
    client = object.__new__(SerialFileTransferClient)
    client.serial = serial
    client.timeout = 5.0
    client.hash_timeout = 30.0
    client.parser = parser
    client.request_id = 1
    client.capabilities = frozenset()
    return client, serial


def run_case(emu: wire.Emulator, fault: str) -> RecordingEmulatorSerial:
    data = payload()
    expected_sha = hashlib.sha256(data).hexdigest()
    client, serial = make_client(emu, fault)
    transfer_ids: list[int] = []
    fault_armed = False
    original_request = client.request

    def recording_request(command: str, params: dict | None = None,
                          timeout: float | None = None) -> dict:
        nonlocal fault_armed
        result = original_request(command, params, timeout)
        if command == "file.write.begin":
            transfer_ids.append(int(result["transfer_id"]))
            if fault == FAULT_OUTPUT:
                armed = original_request("binary.test.final-ack-output-failure-once")
                if armed.get("armed") is not True:
                    raise AssertionError(f"final ACK output fault was not armed: {armed}")
                fault_armed = True
        return result

    client.request = recording_request
    started = time.monotonic()
    try:
        client.sync()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "e1b-final-ack.bin"
            source.write_bytes(data)
            metrics = client.upload(
                "/upload/e1b-final-ack.bin",
                source,
                replace=True,
                encoding="raw",
            )

        if len(transfer_ids) != 1:
            raise AssertionError(f"unexpected transfer IDs: {transfer_ids}")
        if fault == FAULT_OUTPUT and not fault_armed:
            raise AssertionError("final ACK output fault was not armed")
        dropped = getattr(client.parser, "dropped_frames", [])
        if fault == FAULT_WIRE and len(dropped) != 1:
            raise AssertionError(f"expected one final ACK drop: {dropped}")
        if fault == FAULT_OUTPUT and dropped:
            raise AssertionError(f"output-failure case unexpectedly dropped ACK: {dropped}")

        final_frames = [
            frame for frame in serial.data_frames
            if frame.get("sequence") == FINAL_DATA_SEQUENCE
            and frame.get("offset") == FINAL_DATA_OFFSET
        ]
        if len(serial.data_frames) != 6 or len(final_frames) != 2:
            raise AssertionError(
                "unexpected DATA send trace: "
                f"frames={len(serial.data_frames)} final_replays={len(final_frames)} "
                f"trace={[ (frame.get('sequence'), frame.get('offset')) for frame in serial.data_frames ]}"
            )
        first, replay = final_frames
        for field in ("transfer_id", "sequence", "offset", "payload", "wire_crc"):
            if first[field] != replay[field]:
                raise AssertionError(
                    f"final DATA retry changed {field}: {first[field]!r} != {replay[field]!r}"
                )
        if not first["crc_valid"] or not replay["crc_valid"]:
            raise AssertionError("final DATA retry CRC was not valid")

        binary = client.request("binary.transfer.status", {"transfer_id": transfer_ids[0]})
        file_status = client.request("file.transfer.status", {"transfer_id": transfer_ids[0]})
        stat = client.stat("/upload/e1b-final-ack.bin")
        hashed = client.sha256("/upload/e1b-final-ack.bin")
        if (
            metrics.frames != 5
            or metrics.retries != 1
            or metrics.protocol_timeouts != 1
            or metrics.nacks != 0
            or binary.get("state") != "completed"
            or binary.get("transferred_bytes") != PAYLOAD_BYTES
            or file_status.get("transport_state") != "completed"
            or file_status.get("file_state") != "completed"
            or file_status.get("transferred_bytes") != PAYLOAD_BYTES
            or stat.get("size_bytes") != PAYLOAD_BYTES
            or hashed.get("size_bytes") != PAYLOAD_BYTES
            or hashed.get("sha256") != expected_sha
        ):
            raise AssertionError(
                "final ACK replay result mismatch: "
                f"metrics={metrics} binary={binary} file={file_status} "
                f"stat={stat} sha={hashed}"
            )

        if fault == FAULT_OUTPUT and b"BINARY_TEST_FAULT action=fail-final-ack-once" not in serial.received_bytes:
            raise AssertionError("firmware final ACK output failure marker was not observed")

        print(
            "E1B_FINAL_ACK_METRICS "
            f"fault={fault} payload_bytes={PAYLOAD_BYTES} data_frames={metrics.frames} "
            f"final_data_sequence={FINAL_DATA_SEQUENCE} final_data_offset={FINAL_DATA_OFFSET} "
            f"dropped_ack={len(dropped)} failed_ack={1 if fault == FAULT_OUTPUT else 0} "
            f"sender_timeouts={metrics.protocol_timeouts} retransmitted_final_data=1 "
            f"nacks={metrics.nacks} final_size={stat['size_bytes']} "
            f"sha256={hashed['sha256']} elapsed={time.monotonic() - started:.3f}s"
        )
    finally:
        client.close()
    return serial


def case_log_path(path: Path, fault: str) -> Path:
    return path.with_name(f"{path.stem}-{fault}{path.suffix}")


def case_uart_path(path: Path, fault: str) -> Path:
    return path.with_name(f"{path.stem}-{fault}{path.suffix}")


def execute_case(args: argparse.Namespace, fault: str) -> None:
    emu = wire.Emulator(args)
    serial: RecordingEmulatorSerial | None = None
    error: BaseException | None = None
    exit_status: int | None = None
    try:
        emu.start()
        emu.wait_line(wire.READY_MARKER, timeout=10.0)
        serial = run_case(emu, fault)
        exit_status = emu.finish()
    except BaseException as exc:
        error = exc
    finally:
        if emu.process is not None and emu.process.poll() is None:
            emu.finish()
        process_output = emu.diagnostics()
        raw = bytes(emu.raw)
        if serial is not None:
            raw += bytes(serial.received_bytes)
        uart_path = case_uart_path(args.uart_log, fault)
        log_path = case_log_path(args.log, fault)
        uart_path.parent.mkdir(parents=True, exist_ok=True)
        uart_path.write_bytes(raw)
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.write_text(
            "UART bytes (hex):\n" + " ".join(f"{byte:02x}" for byte in raw)
            + "\n\nesp-emu output:\n"
            + process_output.decode("utf-8", errors="replace"),
            encoding="utf-8",
        )
    if error is not None:
        raise error
    if exit_status not in (0, -15, -9):
        raise AssertionError(f"esp-emu exit status was {exit_status}")


def main() -> int:
    args = parse_args()
    try:
        for fault in (FAULT_WIRE, FAULT_OUTPUT):
            execute_case(args, fault)
    except BaseException as error:
        traceback.print_exception(error)
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print("PASS: FILE TRANSFER FINAL ACK REPLAY / OUTPUT FAILURE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
