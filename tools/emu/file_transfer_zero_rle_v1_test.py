#!/usr/bin/env python3
"""Bounded RAM-backed zero-rle-v1 File Transfer integration test."""

from __future__ import annotations

import argparse
import binascii
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import traceback

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from np2_fixture_cache import (
    CAPABILITY_ZERO_RLE_V1,
    ENCODING_ZERO_RLE_V1,
    SerialFileTransferClient,
    _SerialParser,
    _ZeroRleProducer,
    analyze_zero_rle,
)
import uart_binary_data_plane_test as wire


LOGICAL_SIZE = 80 * 1024
ACK_TIMEOUT = 2.0


class EmulatorSerial:
    """Small pyserial-compatible adapter for the production host client."""

    def __init__(self, emu: wire.Emulator) -> None:
        if emu.sock is None:
            raise AssertionError("emu UART socket is not connected")
        self.sock = emu.sock
        self.buffer = bytearray()

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
    args = parser.parse_args()
    args.emulator_timeout = "90s"
    args.process_timeout = 100.0
    return args


def request(emu: wire.Emulator, request_id: int, command: str,
            params: dict | None = None) -> dict:
    emu.send_json(request_id, command, params)
    return emu.wait_response(request_id, timeout=30.0)


def response(emu: wire.Emulator, request_id: int, command: str,
             params: dict | None = None) -> dict:
    result = wire.require_response(request(emu, request_id, command, params), request_id)
    return result


def require_error(emu: wire.Emulator, request_id: int, command: str,
                  code: str, params: dict | None = None) -> dict:
    result = request(emu, request_id, command, params)
    if result.get("ok") is not False or result.get("error", {}).get("code") != code:
        raise AssertionError(f"expected {code}, received: {result}")
    return result


def logical_payload() -> bytes:
    data = bytearray(LOGICAL_SIZE)
    for offset in range(0, LOGICAL_SIZE, 512):
        literal = bytes(((offset // 512 + index + 1) & 0xFF) for index in range(8))
        data[offset : offset + len(literal)] = literal
    return bytes(data)


def run_production_host_client(emu: wire.Emulator) -> None:
    """Exercise the real host client against the emulator's actual hello."""

    logical = logical_payload()
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "c2-host-source.bin"
        source.write_bytes(logical)
        client = object.__new__(SerialFileTransferClient)
        client.serial = EmulatorSerial(emu)
        client.timeout = 5.0
        client.hash_timeout = 30.0
        client.parser = _SerialParser()
        client.request_id = 1
        client.capabilities = frozenset()
        try:
            hello = client.sync()
            if CAPABILITY_ZERO_RLE_V1 not in client.capabilities:
                raise AssertionError(f"production host did not recognize capability: {hello}")
            metrics = client.upload(
                "/upload/c2-host-client.bin",
                source,
                replace=False,
                encoding=ENCODING_ZERO_RLE_V1,
            )
            if (metrics.encoding != ENCODING_ZERO_RLE_V1 or
                    metrics.logical_size_bytes != len(logical) or
                    metrics.logical_bytes_sent != len(logical) or
                    metrics.wire_bytes_sent != metrics.wire_size_bytes or
                    metrics.frames == 0):
                raise AssertionError(f"production host compressed metrics mismatch: {metrics}")
            stat = client.stat("/upload/c2-host-client.bin")
            hashed = client.sha256("/upload/c2-host-client.bin")
            if (stat.get("type") != "file" or stat.get("size_bytes") != len(logical) or
                    hashed.get("size_bytes") != len(logical) or
                    hashed.get("sha256") != hashlib.sha256(logical).hexdigest()):
                raise AssertionError(f"production host logical verification failed: {stat} {hashed}")
            if client.ping() != {"pong": True}:
                raise AssertionError("production host final ping failed")
        finally:
            client.close()


def send_compressed(emu: wire.Emulator, request_id: int, path: str,
                    plan, replace: bool = False) -> tuple[int, int]:
    begin = response(emu, request_id, "file.write.begin", {
        "path": path,
        "size_bytes": plan.local.size_bytes,
        "encoding": ENCODING_ZERO_RLE_V1,
        "wire_size_bytes": plan.wire_size_bytes,
        "replace": replace,
    })
    if (begin.get("encoding") != ENCODING_ZERO_RLE_V1 or
            begin.get("size_bytes") != plan.local.size_bytes or
            begin.get("wire_size_bytes") != plan.wire_size_bytes or
            begin.get("expected_offset") != 0):
        raise AssertionError(f"compressed begin schema mismatch: {begin}")
    transfer_id = int(begin["transfer_id"])

    producer = _ZeroRleProducer(plan)
    offset = 0
    crc = 0
    try:
        sequence = 0
        while offset < plan.wire_size_bytes:
            chunk = producer.read(min(wire.MAX_PAYLOAD, plan.wire_size_bytes - offset))
            if not chunk:
                raise AssertionError("production encoder ended before the wire stream")
            frame = wire.build_frame(wire.DATA, transfer_id, sequence, offset, payload=chunk)
            emu.send(frame)
            wire.require_ack(
                emu.wait_frame(ACK_TIMEOUT), transfer_id, sequence + 1,
                offset + len(chunk)
            )
            if sequence == 0:
                # C0 duplicate DATA replay: the decoder must not consume the
                # already accepted encoded byte twice.
                emu.send(frame)
                wire.require_ack(
                    emu.wait_frame(ACK_TIMEOUT), transfer_id, sequence + 1,
                    offset + len(chunk)
                )
            crc = binascii.crc32(chunk, crc)
            offset += len(chunk)
            sequence += 1
        producer.finish()
    finally:
        producer.close()
    if offset != plan.wire_size_bytes:
        raise AssertionError("compressed frame plan did not consume the wire stream")
    return transfer_id, crc & 0xFFFFFFFF


def require_completed(emu: wire.Emulator, request_id: int, transfer_id: int,
                      plan, wire_crc: int) -> None:
    binary = response(emu, request_id, "binary.transfer.status", {
        "transfer_id": transfer_id,
    })
    if (binary.get("state") != "completed" or
            binary.get("transferred_bytes") != plan.wire_size_bytes or
            binary.get("crc32") != wire_crc):
        raise AssertionError(f"compressed binary status mismatch: {binary}")
    file_status = response(emu, request_id + 1, "file.transfer.status", {
        "transfer_id": transfer_id,
    })
    if (file_status.get("transport_state") != "completed" or
            file_status.get("file_state") != "completed" or
            file_status.get("size_bytes") != plan.local.size_bytes or
            file_status.get("transferred_bytes") != plan.local.size_bytes or
            file_status.get("wire_size_bytes") != plan.wire_size_bytes or
            file_status.get("wire_transferred_bytes") != plan.wire_size_bytes or
            file_status.get("encoding") != ENCODING_ZERO_RLE_V1):
        raise AssertionError(f"compressed file status mismatch: {file_status}")
    hashed = response(emu, request_id + 2, "file.sha256", {"path": "/upload/c1-valid.bin"})
    if (hashed.get("size_bytes") != plan.local.size_bytes or
            hashed.get("sha256") != plan.local.sha256):
        raise AssertionError(f"compressed file SHA mismatch: {hashed}")


def run(emu: wire.Emulator) -> None:
    hello = response(emu, 1, "protocol.hello")
    capabilities = hello.get("capabilities", [])
    if "file-transfer.v1" not in capabilities or CAPABILITY_ZERO_RLE_V1 not in capabilities:
        raise AssertionError(f"zero-rle capability missing: {hello}")

    run_production_host_client(emu)

    logical = logical_payload()
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "c1-source.bin"
        source.write_bytes(logical)
        plan = analyze_zero_rle(source)
        transfer_id, wire_crc = send_compressed(emu, 10, "/upload/c1-valid.bin", plan)
        require_completed(emu, 20, transfer_id, plan, wire_crc)

        abort_producer = _ZeroRleProducer(plan)
        try:
            first = abort_producer.read(wire.MAX_PAYLOAD)
        finally:
            abort_producer.close()

    zero = response(emu, 30, "file.write.begin", {
        "path": "/upload/c1-empty.bin",
        "size_bytes": 0,
        "encoding": ENCODING_ZERO_RLE_V1,
        "wire_size_bytes": 0,
    })
    if (zero.get("state") != "completed" or zero.get("transfer_id") is not None or
            zero.get("size_bytes") != 0 or zero.get("encoding") != ENCODING_ZERO_RLE_V1 or
            zero.get("wire_size_bytes") != 0):
        raise AssertionError(f"compressed zero-byte response mismatch: {zero}")

    require_error(emu, 31, "file.write.begin", "UNSUPPORTED", {
        "path": "/upload/c1-unsupported.bin",
        "size_bytes": 1,
        "encoding": "made-up-v2",
        "wire_size_bytes": 1,
    })
    require_error(emu, 32, "file.write.begin", "INVALID_PARAMS", {
        "path": "/upload/c1-missing-wire.bin",
        "size_bytes": 1,
        "encoding": ENCODING_ZERO_RLE_V1,
    })

    bad_path = "/upload/c1-malformed.bin"
    bad_begin = response(emu, 40, "file.write.begin", {
        "path": bad_path,
        "size_bytes": 1,
        "encoding": ENCODING_ZERO_RLE_V1,
        "wire_size_bytes": 5,
    })
    bad_id = int(bad_begin["transfer_id"])
    emu.send(wire.build_frame(wire.DATA, bad_id, 0, 0,
                              payload=b"\x7f\x00\x00\x00\x01"))
    try:
        emu.wait_frame(0.25)
    except AssertionError:
        pass
    bad_binary = response(emu, 41, "binary.transfer.status", {"transfer_id": bad_id})
    bad_file = response(emu, 42, "file.transfer.status", {"transfer_id": bad_id})
    if bad_binary.get("state") != "aborted" or bad_file.get("file_state") != "failed" or \
            bad_file.get("error", {}).get("code") != "MALFORMED_ENCODING":
        raise AssertionError(f"malformed encoding status mismatch: {bad_binary} {bad_file}")
    require_error(emu, 43, "file.stat", "NOT_FOUND", {"path": bad_path})

    preserved_path = "/upload/c1-preserved.bin"
    original = bytes(range(37))
    raw_begin = response(emu, 50, "file.write.begin", {
        "path": preserved_path, "size_bytes": len(original), "replace": False,
    })
    raw_id = int(raw_begin["transfer_id"])
    emu.send(wire.build_frame(wire.DATA, raw_id, 0, 0, payload=original))
    wire.require_ack(emu.wait_frame(), raw_id, 1, len(original))
    raw_status = response(emu, 51, "file.transfer.status", {"transfer_id": raw_id})
    if raw_status.get("file_state") != "completed":
        raise AssertionError(f"raw seed did not complete: {raw_status}")
    bad_replace = response(emu, 52, "file.write.begin", {
        "path": preserved_path,
        "size_bytes": 1,
        "encoding": ENCODING_ZERO_RLE_V1,
        "wire_size_bytes": 5,
        "replace": True,
    })
    bad_replace_id = int(bad_replace["transfer_id"])
    emu.send(wire.build_frame(wire.DATA, bad_replace_id, 0, 0,
                              payload=b"\x7f\x00\x00\x00\x01"))
    try:
        emu.wait_frame(0.25)
    except AssertionError:
        pass
    replacement_status = response(emu, 53, "file.transfer.status", {
        "transfer_id": bad_replace_id,
    })
    if replacement_status.get("error", {}).get("code") != "MALFORMED_ENCODING":
        raise AssertionError(f"malformed replacement status mismatch: {replacement_status}")
    preserved_hash = response(emu, 54, "file.sha256", {"path": preserved_path})
    if preserved_hash.get("sha256") != hashlib.sha256(original).hexdigest():
        raise AssertionError(f"malformed replacement changed old target: {preserved_hash}")

    aborted = response(emu, 60, "file.write.begin", {
        "path": "/upload/c1-aborted.bin",
        "size_bytes": plan.local.size_bytes,
        "encoding": ENCODING_ZERO_RLE_V1,
        "wire_size_bytes": plan.wire_size_bytes,
    })
    aborted_id = int(aborted["transfer_id"])
    emu.send(wire.build_frame(wire.DATA, aborted_id, 0, 0, payload=first))
    wire.require_ack(emu.wait_frame(), aborted_id, 1, len(first))
    response(emu, 61, "binary.transfer.abort", {"transfer_id": aborted_id})
    aborted_status = response(emu, 62, "file.transfer.status", {"transfer_id": aborted_id})
    if (aborted_status.get("file_state") != "aborted" or
            aborted_status.get("error", {}).get("code") != "TRANSFER_ABORTED"):
        raise AssertionError(f"compressed abort status mismatch: {aborted_status}")
    require_error(emu, 63, "file.stat", "NOT_FOUND", {"path": "/upload/c1-aborted.bin"})

    raw_data = b"raw-after-zero-rle-v1"
    raw_id = int(response(emu, 70, "file.write.begin", {
        "path": "/upload/c1-raw-after.bin", "size_bytes": len(raw_data),
    })["transfer_id"])
    emu.send(wire.build_frame(wire.DATA, raw_id, 0, 0, payload=raw_data))
    wire.require_ack(emu.wait_frame(), raw_id, 1, len(raw_data))
    raw_after = response(emu, 71, "file.transfer.status", {"transfer_id": raw_id})
    if raw_after.get("file_state") != "completed" or raw_after.get("transferred_bytes") != len(raw_data):
        raise AssertionError(f"raw-after compressed status mismatch: {raw_after}")
    if "encoding" in response(emu, 72, "file.write.begin", {
            "path": "/upload/c1-raw-second.bin", "size_bytes": 0,
    }):
        raise AssertionError("raw legacy begin unexpectedly added encoding")
    if response(emu, 73, "system.ping") != {"pong": True}:
        raise AssertionError("final ping failed")


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
    print("PASS: FILE TRANSFER ZERO-RLE-V1 EMU INTEGRATION")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
