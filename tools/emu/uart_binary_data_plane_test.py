#!/usr/bin/env python3
"""Bidirectional UART Binary Data Plane v1 test over esp-emu UART-TCP."""

from __future__ import annotations

import argparse
import binascii
from collections import deque
import json
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import time


FRAME_PREFIX = "@ESP-NP2 "
READY_MARKER = "ESP-NP2KAI UART CONTROL READY"
MAGIC = b"NB"
VERSION = 1
HEADER = struct.Struct("<2sBBHHIIQHH")
HEADER_BYTES = HEADER.size
CRC_BYTES = 4
MAX_PAYLOAD = 1024
MAX_DECODED = HEADER_BYTES + MAX_PAYLOAD + CRC_BYTES
MAX_ENCODED = 1061
TRANSFER_BYTES = 65536
ACK_TIMEOUT = 2.0
CONNECT_TIMEOUT = 5.0
PROCESS_TIMEOUT = 70.0

DATA = 0x01
ACK = 0x02
NACK = 0x03
BAD_CRC = 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--uart-log", required=True, type=Path)
    parser.add_argument(
        "--esp-emu",
        default=os.environ.get("ESP_EMU", os.path.expanduser("~/.local/bin/esp-emu")),
    )
    return parser.parse_args()


def cobs_encode(data: bytes) -> bytes:
    output = bytearray([0])
    code = 1
    code_index = 0
    for byte in data:
        if byte == 0:
            output[code_index] = code
            code_index = len(output)
            output.append(0)
            code = 1
        else:
            output.append(byte)
            code += 1
            if code == 0xFF:
                output[code_index] = code
                code_index = len(output)
                output.append(0)
                code = 1
    output[code_index] = code
    return bytes(output)


def cobs_decode(data: bytes) -> bytes:
    if not data:
        raise ValueError("empty COBS body")
    output = bytearray()
    index = 0
    while index < len(data):
        code = data[index]
        index += 1
        if code == 0 or index + code - 1 > len(data):
            raise ValueError("invalid COBS body")
        output.extend(data[index : index + code - 1])
        index += code - 1
        if code != 0xFF and index < len(data):
            output.append(0)
    if len(output) > MAX_DECODED:
        raise ValueError("decoded frame is too large")
    return bytes(output)


def crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def pattern(offset: int) -> int:
    block_offset = offset % 1024
    prefix = b"@ESP-NP2 "
    log_text = b"I (123) ESP-IDF log text\n"
    if 256 <= block_offset < 256 + len(prefix):
        return prefix[block_offset - 256]
    if 320 <= block_offset < 320 + len(log_text):
        return log_text[block_offset - 320]
    special = {384: 0x00, 385: 0x0A, 386: 0x0D, 387: 0xFF, 388: 0x00}
    return special.get(block_offset, block_offset & 0xFF)


def pattern_chunk(offset: int, length: int) -> bytes:
    return bytes(pattern(offset + index) for index in range(length))


def expected_crc_for_size(size: int) -> int:
    value = 0
    for offset in range(0, size, MAX_PAYLOAD):
        value = binascii.crc32(
            pattern_chunk(offset, min(MAX_PAYLOAD, size - offset)), value
        )
    return value & 0xFFFFFFFF


def expected_crc() -> int:
    return expected_crc_for_size(TRANSFER_BYTES)


def build_frame(
    frame_type: int,
    transfer_id: int,
    sequence: int,
    offset: int,
    status: int = 0,
    payload: bytes = b"",
) -> bytes:
    if not 0 <= len(payload) <= MAX_PAYLOAD:
        raise ValueError("payload length exceeds v1 limit")
    if frame_type == DATA and (not payload or status != 0):
        raise ValueError("DATA frame fields are invalid")
    if frame_type == ACK and (payload or status != 0):
        raise ValueError("ACK frame fields are invalid")
    if frame_type == NACK and (payload or status == 0):
        raise ValueError("NACK frame fields are invalid")
    header = HEADER.pack(
        MAGIC,
        VERSION,
        frame_type,
        0,
        HEADER_BYTES,
        transfer_id,
        sequence,
        offset,
        len(payload),
        status,
    )
    decoded = header + payload
    decoded += struct.pack("<I", crc32(decoded))
    encoded = cobs_encode(decoded)
    if len(decoded) > MAX_DECODED or len(encoded) > MAX_ENCODED:
        raise ValueError("encoded frame exceeds v1 bounds")
    return b"\x00\x00" + encoded + b"\x00"


def build_unchecked_control_frame(
    frame_type: int, transfer_id: int, sequence: int, offset: int,
    status: int = 0, payload: bytes = b"",
) -> bytes:
    """Build a CRC-valid noncanonical ACK/NACK for receiver validation tests."""
    header = HEADER.pack(
        MAGIC,
        VERSION,
        frame_type,
        0,
        HEADER_BYTES,
        transfer_id,
        sequence,
        offset,
        len(payload),
        status,
    )
    decoded = header + payload
    decoded += struct.pack("<I", crc32(decoded))
    encoded = cobs_encode(decoded)
    return b"\x00\x00" + encoded + b"\x00"


def parse_frame(decoded: bytes) -> dict:
    if len(decoded) < HEADER_BYTES + CRC_BYTES:
        raise AssertionError("decoded binary frame is too short")
    (
        magic,
        version,
        frame_type,
        flags,
        header_length,
        transfer_id,
        sequence,
        offset,
        payload_length,
        status,
    ) = HEADER.unpack(decoded[:HEADER_BYTES])
    if magic != MAGIC or version != VERSION or flags != 0 or header_length != HEADER_BYTES:
        raise AssertionError("binary frame header is invalid")
    if frame_type not in (DATA, ACK, NACK):
        raise AssertionError("unknown binary frame type")
    if payload_length > MAX_PAYLOAD or len(decoded) != HEADER_BYTES + payload_length + CRC_BYTES:
        raise AssertionError("binary frame payload length is invalid")
    if frame_type == DATA and (payload_length == 0 or status != 0):
        raise AssertionError("DATA frame fields are invalid")
    if frame_type == ACK and (payload_length != 0 or status != 0):
        raise AssertionError("ACK frame fields are invalid")
    if frame_type == NACK and (payload_length != 0 or status == 0):
        raise AssertionError("NACK frame fields are invalid")
    payload = decoded[HEADER_BYTES : HEADER_BYTES + payload_length]
    wire_crc = struct.unpack("<I", decoded[-CRC_BYTES:])[0]
    return {
        "type": frame_type,
        "transfer_id": transfer_id,
        "sequence": sequence,
        "offset": offset,
        "status": status,
        "payload": payload,
        "wire_crc": wire_crc,
        "crc_valid": crc32(decoded[:-CRC_BYTES]) == wire_crc,
    }


class StreamParser:
    """The host-side equivalent of the firmware TEXT/binary multiplexer."""

    def __init__(self) -> None:
        self.state = "text"
        self.text = bytearray()
        self.encoded = bytearray()
        self.lines: deque[str] = deque()
        self.frames: deque[dict] = deque()

    def _text_byte(self, byte: int) -> None:
        self.text.append(byte)
        if len(self.text) > 4096:
            self.text.clear()
            return
        if byte == 0x0A:
            line = bytes(self.text[:-1]).rstrip(b"\r").decode("utf-8", errors="replace")
            self.lines.append(line)
            self.text.clear()

    def feed(self, data: bytes) -> None:
        for byte in data:
            if self.state == "text":
                if byte == 0:
                    self.text.clear()
                    self.state = "start_zero"
                else:
                    self._text_byte(byte)
            elif self.state == "start_zero":
                if byte == 0:
                    self.encoded.clear()
                    self.state = "collect"
                else:
                    self.state = "text"
                    self._text_byte(byte)
            elif self.state == "collect":
                if byte == 0:
                    if self.encoded:
                        try:
                            self.frames.append(parse_frame(cobs_decode(bytes(self.encoded))))
                        except (AssertionError, ValueError):
                            pass
                    self.encoded.clear()
                    self.state = "text"
                elif len(self.encoded) < MAX_ENCODED:
                    self.encoded.append(byte)
                else:
                    self.state = "discard"
            else:
                if byte == 0:
                    self.state = "text"
                    self.encoded.clear()


class Emulator:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.process: subprocess.Popen[bytes] | None = None
        self.sock: socket.socket | None = None
        self.parser = StreamParser()
        self.raw = bytearray()
        self.port = 0
        self.fallback_cleanup = False
        self.deadline = 0.0

    def start(self) -> None:
        if not self.args.firmware.is_file():
            raise AssertionError(f"firmware image not found: {self.args.firmware}")
        if not os.path.isfile(self.args.esp_emu) or not os.access(self.args.esp_emu, os.X_OK):
            raise AssertionError(f"esp-emu executable not found: {self.args.esp_emu}")

        reservation = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            reservation.bind(("127.0.0.1", 0))
            self.port = reservation.getsockname()[1]
        finally:
            reservation.close()

        command = [
            self.args.esp_emu,
            "--chip",
            "esp32p4",
            "--firmware",
            str(self.args.firmware),
            "--uart-tcp",
            f"127.0.0.1:{self.port}",
            "--timeout",
            getattr(self.args, "emulator_timeout", "60s"),
            "--log-color",
            "never",
        ]
        command.extend(getattr(self.args, "extra_emu_args", []))
        self.process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env={**os.environ, "NO_COLOR": "1"},
        )
        self.deadline = time.monotonic() + getattr(
            self.args, "process_timeout", PROCESS_TIMEOUT
        )

        deadline = time.monotonic() + CONNECT_TIMEOUT
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise AssertionError(
                    f"esp-emu exited before UART-TCP connection: {self.process.returncode}"
                )
            try:
                self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=0.25)
                self.sock.setblocking(False)
                return
            except OSError:
                time.sleep(0.05)
        raise AssertionError("timed out connecting to esp-emu UART-TCP")

    def pump(self, timeout: float) -> None:
        if self.sock is None:
            raise AssertionError("UART-TCP socket is not connected")
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise AssertionError("UART binary data-plane test exceeded its outer deadline")
        readable, _, _ = select.select([self.sock], [], [], max(0.0, min(timeout, remaining)))
        if not readable:
            return
        data = self.sock.recv(65536)
        if not data:
            raise AssertionError("esp-emu closed the UART-TCP socket")
        self.raw.extend(data)
        self.parser.feed(data)

    def send(self, data: bytes) -> None:
        if self.sock is None:
            raise AssertionError("UART-TCP socket is not connected")
        self.sock.sendall(data)

    def send_json(self, request_id: int, command: str, params: dict | None = None) -> None:
        request: dict[str, object] = {"v": 1, "id": request_id, "cmd": command}
        if params is not None:
            request["params"] = params
        payload = json.dumps(request, separators=(",", ":"))
        self.send((FRAME_PREFIX + payload + "\n").encode("ascii"))

    def wait_line(self, marker: str, timeout: float = ACK_TIMEOUT) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for line in list(self.parser.lines):
                if marker in line:
                    return
            self.pump(min(0.1, deadline - time.monotonic()))
        raise AssertionError(f"UART marker was not observed: {marker}")

    def wait_response(self, request_id: int, timeout: float = ACK_TIMEOUT) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for index, line in enumerate(self.parser.lines):
                if not line.startswith(FRAME_PREFIX):
                    continue
                try:
                    response = json.loads(line[len(FRAME_PREFIX) :])
                except json.JSONDecodeError as exc:
                    raise AssertionError(f"malformed JSON response: {line!r}") from exc
                if response.get("id") == request_id:
                    del self.parser.lines[index]
                    return response
            self.pump(min(0.1, deadline - time.monotonic()))
        raise AssertionError(f"JSON response timeout for request id {request_id}")

    def wait_frame(self, timeout: float = ACK_TIMEOUT) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.parser.frames:
                return self.parser.frames.popleft()
            self.pump(min(0.1, deadline - time.monotonic()))
        raise AssertionError("binary frame timeout")

    def finish(self) -> int | None:
        if self.sock is not None:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.sock.close()
            self.sock = None
        if self.process is None:
            return None
        try:
            return self.process.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            self.fallback_cleanup = True
            self.process.terminate()
            try:
                return self.process.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                self.process.kill()
                return self.process.wait(timeout=3.0)

    def diagnostics(self) -> bytes:
        if self.process is None:
            return b""
        try:
            output, _ = self.process.communicate(timeout=1.0)
        except subprocess.TimeoutExpired:
            self.process.kill()
            output, _ = self.process.communicate()
        return output or b""


def require_response(response: dict, request_id: int) -> dict:
    if response.get("id") != request_id or response.get("ok") is not True:
        raise AssertionError(f"request {request_id} failed: {response}")
    return response.get("result", {})


def require_ack(frame: dict, transfer_id: int, sequence: int, offset: int) -> None:
    if (
        frame["type"] != ACK
        or frame["transfer_id"] != transfer_id
        or frame["sequence"] != sequence
        or frame["offset"] != offset
        or frame["status"] != 0
        or frame["payload"]
        or not frame["crc_valid"]
    ):
        raise AssertionError(f"unexpected ACK: {frame}")


def require_nack(frame: dict, transfer_id: int, sequence: int, offset: int,
                 reason: int) -> None:
    if (
        frame["type"] != NACK
        or frame["transfer_id"] != transfer_id
        or frame["sequence"] != sequence
        or frame["offset"] != offset
        or frame["status"] != reason
        or frame["payload"]
        or not frame["crc_valid"]
    ):
        raise AssertionError(f"unexpected NACK: {frame}")


def begin_rx(emulator: Emulator, request_id: int, size: int) -> int:
    emulator.send_json(request_id, "binary.test.rx.begin", {"size_bytes": size})
    result = require_response(emulator.wait_response(request_id), request_id)
    transfer_id = int(result["transfer_id"])
    if result.get("direction") != "host_to_device" or result.get("size_bytes") != size:
        raise AssertionError(f"unexpected RX begin result: {result}")
    return transfer_id


def complete_rx(emulator: Emulator, request_id: int, transfer_id: int,
                size: int) -> None:
    emulator.send_json(request_id, "binary.transfer.status", {"transfer_id": transfer_id})
    status = require_response(emulator.wait_response(request_id), request_id)
    if (
        status.get("state") != "completed"
        or status.get("transferred_bytes") != size
        or status.get("crc32") != expected_crc_for_size(size)
    ):
        raise AssertionError(f"unexpected small RX status: {status}")


def send_data(emulator: Emulator, transfer_id: int, sequence: int,
              offset: int, payload: bytes) -> None:
    emulator.send(build_frame(DATA, transfer_id, sequence, offset, payload=payload))


def corrupt_crc(wire: bytes) -> bytes:
    decoded = bytearray(cobs_decode(wire[2:-1]))
    decoded[-1] ^= 0x01
    return b"\x00\x00" + cobs_encode(bytes(decoded)) + b"\x00"


def finish_1812_case(emulator: Emulator, transfer_id: int) -> None:
    final = pattern_chunk(1024, 788)
    send_data(emulator, transfer_id, 1, 1024, final)
    require_ack(emulator.wait_frame(), transfer_id, 2, 1812)


def run_active_duplicate_cases(emulator: Emulator, request_id: int) -> None:
    # CASE A: deliberately consume/drop the first ACK, replay the exact 1024-byte
    # DATA frame, then accept the 788-byte final frame.  This is the original
    # 1812-byte failure shape.
    transfer_id = begin_rx(emulator, request_id, 1812)
    first = pattern_chunk(0, 1024)
    send_data(emulator, transfer_id, 0, 0, first)
    require_ack(emulator.wait_frame(), transfer_id, 1, 1024)  # dropped by host logic
    send_data(emulator, transfer_id, 0, 0, first)
    require_ack(emulator.wait_frame(), transfer_id, 1, 1024)
    finish_1812_case(emulator, transfer_id)
    complete_rx(emulator, request_id + 1, transfer_id, 1812)

    # CASE H: the same 1024 + 788 shape without a replay remains unchanged.
    transfer_id = begin_rx(emulator, request_id + 10, 1812)
    send_data(emulator, transfer_id, 0, 0, first)
    require_ack(emulator.wait_frame(), transfer_id, 1, 1024)
    finish_1812_case(emulator, transfer_id)
    complete_rx(emulator, request_id + 11, transfer_id, 1812)

    # The extreme 1024 + 1 shape is covered as well.
    transfer_id = begin_rx(emulator, request_id + 20, 1025)
    send_data(emulator, transfer_id, 0, 0, first)
    require_ack(emulator.wait_frame(), transfer_id, 1, 1024)
    send_data(emulator, transfer_id, 0, 0, first)
    require_ack(emulator.wait_frame(), transfer_id, 1, 1024)
    send_data(emulator, transfer_id, 1, 1024, pattern_chunk(1024, 1))
    require_ack(emulator.wait_frame(), transfer_id, 2, 1025)
    complete_rx(emulator, request_id + 21, transfer_id, 1025)


def run_duplicate_validation_cases(emulator: Emulator, request_id: int) -> None:
    first = pattern_chunk(0, 1024)

    # CASE B: changed payload with an invalid CRC is rejected before duplicate
    # recognition and does not change the accepted transfer state.
    transfer_id = begin_rx(emulator, request_id, 1812)
    send_data(emulator, transfer_id, 0, 0, first)
    require_ack(emulator.wait_frame(), transfer_id, 1, 1024)
    changed = bytearray(first)
    changed[0] ^= 0x01
    emulator.send(corrupt_crc(build_frame(DATA, transfer_id, 0, 0, payload=bytes(changed))))
    require_nack(emulator.wait_frame(), transfer_id, 1, 1024, BAD_CRC)
    send_data(emulator, transfer_id, 0, 0, first)
    require_ack(emulator.wait_frame(), transfer_id, 1, 1024)
    finish_1812_case(emulator, transfer_id)
    complete_rx(emulator, request_id + 1, transfer_id, 1812)

    # CASE C: changed payload with a valid CRC is not a duplicate.  Use an even
    # 1024 + 1024 transfer so ordinary length validation can reach BAD_SEQUENCE.
    transfer_id = begin_rx(emulator, request_id + 10, 2048)
    send_data(emulator, transfer_id, 0, 0, first)
    require_ack(emulator.wait_frame(), transfer_id, 1, 1024)
    changed = bytearray(first)
    changed[0] ^= 0x01
    emulator.send(build_frame(DATA, transfer_id, 0, 0, payload=bytes(changed)))
    require_nack(emulator.wait_frame(), transfer_id, 1, 1024, 2)  # BAD_SEQUENCE
    send_data(emulator, transfer_id, 0, 0, first)
    require_ack(emulator.wait_frame(), transfer_id, 1, 1024)
    send_data(emulator, transfer_id, 1, 1024, pattern_chunk(1024, 1024))
    require_ack(emulator.wait_frame(), transfer_id, 2, 2048)
    complete_rx(emulator, request_id + 11, transfer_id, 2048)

    # CASE D: wrong sequence is ordinary BAD_SEQUENCE.
    transfer_id = begin_rx(emulator, request_id + 20, 2048)
    send_data(emulator, transfer_id, 0, 0, first)
    require_ack(emulator.wait_frame(), transfer_id, 1, 1024)
    emulator.send(build_frame(DATA, transfer_id, 7, 0, payload=first))
    require_nack(emulator.wait_frame(), transfer_id, 1, 1024, 2)  # BAD_SEQUENCE
    send_data(emulator, transfer_id, 0, 0, first)
    require_ack(emulator.wait_frame(), transfer_id, 1, 1024)
    send_data(emulator, transfer_id, 1, 1024, pattern_chunk(1024, 1024))
    require_ack(emulator.wait_frame(), transfer_id, 2, 2048)
    complete_rx(emulator, request_id + 21, transfer_id, 2048)

    # CASE E: wrong offset is ordinary BAD_OFFSET.
    transfer_id = begin_rx(emulator, request_id + 30, 2048)
    send_data(emulator, transfer_id, 0, 0, first)
    require_ack(emulator.wait_frame(), transfer_id, 1, 1024)
    emulator.send(build_frame(DATA, transfer_id, 1, 1, payload=first))
    require_nack(emulator.wait_frame(), transfer_id, 1, 1024, 3)  # BAD_OFFSET
    send_data(emulator, transfer_id, 0, 0, first)
    require_ack(emulator.wait_frame(), transfer_id, 1, 1024)
    send_data(emulator, transfer_id, 1, 1024, pattern_chunk(1024, 1024))
    require_ack(emulator.wait_frame(), transfer_id, 2, 2048)
    complete_rx(emulator, request_id + 31, transfer_id, 2048)

    # CASE F: an oversized new frame is still INVALID_LENGTH.  The exact
    # previous frame remains replayable afterwards.
    transfer_id = begin_rx(emulator, request_id + 40, 1812)
    send_data(emulator, transfer_id, 0, 0, first)
    require_ack(emulator.wait_frame(), transfer_id, 1, 1024)
    send_data(emulator, transfer_id, 1, 1024, first)
    require_nack(emulator.wait_frame(), transfer_id, 1, 1024, 4)  # INVALID_LENGTH
    send_data(emulator, transfer_id, 0, 0, first)
    require_ack(emulator.wait_frame(), transfer_id, 1, 1024)
    finish_1812_case(emulator, transfer_id)
    complete_rx(emulator, request_id + 41, transfer_id, 1812)


def run_host_to_device(emulator: Emulator, transfer_id: int) -> None:
    def send_and_ack(wire: bytes, expected_sequence: int, expected_offset: int) -> None:
        emulator.send(wire)
        require_ack(emulator.wait_frame(), transfer_id, expected_sequence, expected_offset)

    first = build_frame(DATA, transfer_id, 0, 0, payload=pattern_chunk(0, MAX_PAYLOAD))
    send_and_ack(first, 1, MAX_PAYLOAD)

    # The receiver must acknowledge an exact retransmission without applying it again.
    send_and_ack(first, 1, MAX_PAYLOAD)

    second = build_frame(DATA, transfer_id, 1, MAX_PAYLOAD, payload=pattern_chunk(MAX_PAYLOAD, MAX_PAYLOAD))
    corrupted_decoded = bytearray(cobs_decode(second[2:-1]))
    corrupted_decoded[-1] ^= 0x01
    corrupted = b"\x00\x00" + cobs_encode(bytes(corrupted_decoded)) + b"\x00"
    emulator.send(corrupted)
    nack = emulator.wait_frame()
    if (
        nack["type"] != NACK
        or nack["transfer_id"] != transfer_id
        or nack["sequence"] != 1
        or nack["offset"] != MAX_PAYLOAD
        or nack["status"] != BAD_CRC
        or not nack["crc_valid"]
    ):
        raise AssertionError(f"unexpected BAD_CRC NACK: {nack}")

    send_and_ack(second, 2, 2 * MAX_PAYLOAD)
    final_wire = b""
    for sequence in range(2, TRANSFER_BYTES // MAX_PAYLOAD):
        offset = sequence * MAX_PAYLOAD
        wire = build_frame(
            DATA,
            transfer_id,
            sequence,
            offset,
            payload=pattern_chunk(offset, MAX_PAYLOAD),
        )
        send_and_ack(wire, sequence + 1, offset + MAX_PAYLOAD)
        final_wire = wire

    # The completed receiver must replay the final progress ACK without
    # reopening or reapplying its endpoint after a link-lost final ACK.
    emulator.send(final_wire)
    require_ack(emulator.wait_frame(), transfer_id,
                TRANSFER_BYTES // MAX_PAYLOAD, TRANSFER_BYTES)


def run_device_to_host(emulator: Emulator, transfer_id: int) -> None:
    for sequence in range(TRANSFER_BYTES // MAX_PAYLOAD):
        offset = sequence * MAX_PAYLOAD
        frame = emulator.wait_frame()
        if (
            frame["type"] != DATA
            or frame["transfer_id"] != transfer_id
            or frame["sequence"] != sequence
            or frame["offset"] != offset
            or frame["payload"] != pattern_chunk(offset, MAX_PAYLOAD)
            or not frame["crc_valid"]
        ):
            raise AssertionError(f"unexpected DATA frame at sequence {sequence}: {frame}")
        if sequence == 1:
            emulator.send(
                build_frame(
                    NACK,
                    transfer_id,
                    sequence,
                    offset,
                    status=BAD_CRC,
                )
            )
            retransmitted = emulator.wait_frame()
            if (
                retransmitted["type"] != DATA
                or retransmitted["transfer_id"] != frame["transfer_id"]
                or retransmitted["sequence"] != frame["sequence"]
                or retransmitted["offset"] != frame["offset"]
                or retransmitted["payload"] != frame["payload"]
                or retransmitted["wire_crc"] != frame["wire_crc"]
                or not retransmitted["crc_valid"]
            ):
                raise AssertionError(
                    f"NACK retransmission changed DATA frame: {retransmitted}"
                )
            # A delayed NACK for the already accepted DATA must not trigger a
            # second replay or rewind the device sender frontier.
            emulator.send(build_frame(NACK, transfer_id, 0, 0, status=BAD_CRC))
        elif sequence == 2:
            # Both an older ACK and the duplicate ACK for the previous
            # frontier are stale while this DATA frame is outstanding.
            emulator.send(build_frame(ACK, transfer_id, 1, MAX_PAYLOAD))
            emulator.send(build_frame(ACK, transfer_id, 2, 2 * MAX_PAYLOAD))
        emulator.send(
            build_frame(
                ACK,
                transfer_id,
                sequence + 1,
                offset + MAX_PAYLOAD,
            )
        )


def run_device_to_host_protocol_error_case(
    emulator: Emulator, begin_request_id: int, status_request_id: int,
    frame_type: int, sequence: int, offset: int, status: int = 0,
    payload: bytes = b"",
) -> None:
    emulator.send_json(
        begin_request_id, "binary.test.tx.begin", {"size_bytes": TRANSFER_BYTES}
    )
    result = require_response(
        emulator.wait_response(begin_request_id), begin_request_id
    )
    transfer_id = int(result["transfer_id"])
    if result.get("direction") != "device_to_host":
        raise AssertionError(f"wrong TX direction: {result}")
    first = emulator.wait_frame()
    if (
        first.get("type") != DATA
        or first.get("transfer_id") != transfer_id
        or first.get("sequence") != 0
        or first.get("offset") != 0
        or not first.get("crc_valid")
    ):
        raise AssertionError(f"unexpected protocol-error DATA frame: {first}")
    emulator.send(
        build_unchecked_control_frame(
            frame_type, transfer_id, sequence, offset, status=status,
            payload=payload,
        )
    )
    emulator.send_json(
        status_request_id, "binary.transfer.status", {"transfer_id": transfer_id}
    )
    transfer_status = require_response(
        emulator.wait_response(status_request_id), status_request_id
    )
    if (
        transfer_status.get("state") != "aborted"
        or transfer_status.get("transferred_bytes") != 0
    ):
        raise AssertionError(f"unexpected protocol-error TX status: {transfer_status}")


def run_device_to_host_validation_cases(emulator: Emulator) -> None:
    # Future ACK must not advance the sender frontier.
    run_device_to_host_protocol_error_case(
        emulator, 40, 41, ACK, sequence=2, offset=2 * MAX_PAYLOAD
    )
    # An offset-mismatched NACK must not cause a retransmission or a
    # sequence/offset rewind.
    run_device_to_host_protocol_error_case(
        emulator, 42, 43, NACK, sequence=0, offset=1, status=BAD_CRC
    )
    # CRC-valid control frames with payload are semantic protocol errors, not
    # silently ignored malformed responses.
    run_device_to_host_protocol_error_case(
        emulator, 44, 45, ACK, sequence=1, offset=MAX_PAYLOAD,
        payload=b"unexpected-ack-payload",
    )
    run_device_to_host_protocol_error_case(
        emulator, 46, 47, NACK, sequence=0, offset=0, status=BAD_CRC,
        payload=b"unexpected-nack-payload",
    )
    print(
        "E1C_FIRMWARE_SENDER_VALIDATION "
        "duplicate_ack=PASS stale_ack=PASS future_ack=PASS "
        "stale_nack=PASS mismatched_nack=PASS ack_payload=PASS nack_payload=PASS"
    )


def main() -> int:
    args = parse_args()
    emulator = Emulator(args)
    exit_status: int | None = None
    error: BaseException | None = None
    try:
        emulator.start()
        emulator.wait_line(READY_MARKER, timeout=10.0)

        run_active_duplicate_cases(emulator, 100)
        run_duplicate_validation_cases(emulator, 200)

        emulator.send_json(20, "binary.test.rx.begin", {"size_bytes": TRANSFER_BYTES})
        rx_result = require_response(emulator.wait_response(20), 20)
        rx_id = int(rx_result["transfer_id"])
        if rx_result.get("direction") != "host_to_device":
            raise AssertionError(f"wrong RX direction: {rx_result}")
        run_host_to_device(emulator, rx_id)

        emulator.send_json(21, "binary.transfer.status", {"transfer_id": rx_id})
        rx_status = require_response(emulator.wait_response(21), 21)
        if (
            rx_status.get("state") != "completed"
            or rx_status.get("transferred_bytes") != TRANSFER_BYTES
            or rx_status.get("crc32") != expected_crc()
        ):
            raise AssertionError(f"unexpected Host-to-ESP status: {rx_status}")

        emulator.send_json(30, "binary.test.tx.begin", {"size_bytes": TRANSFER_BYTES})
        tx_result = require_response(emulator.wait_response(30), 30)
        tx_id = int(tx_result["transfer_id"])
        if tx_result.get("direction") != "device_to_host":
            raise AssertionError(f"wrong TX direction: {tx_result}")
        run_device_to_host(emulator, tx_id)

        emulator.send_json(31, "binary.transfer.status", {"transfer_id": tx_id})
        tx_status = require_response(emulator.wait_response(31), 31)
        if (
            tx_status.get("state") != "completed"
            or tx_status.get("transferred_bytes") != TRANSFER_BYTES
            or tx_status.get("crc32") != expected_crc()
        ):
            raise AssertionError(f"unexpected ESP-to-Host status: {tx_status}")

        run_device_to_host_validation_cases(emulator)

        malformed_probe = build_frame(ACK, 0x12345678, 0, 0)
        encoded_probe = bytearray(malformed_probe[2:-1])
        corruption_index = len(encoded_probe) // 2
        if encoded_probe[corruption_index] == 0:
            raise AssertionError("COBS probe unexpectedly contains a zero byte")
        malformed_probe = (
            b"\x00\x00"
            + bytes(encoded_probe[:corruption_index])
            + b"\x00"
            + bytes(encoded_probe[corruption_index + 1 :])
            + b"\x00"
        )
        emulator.send(malformed_probe)
        emulator.send_json(902, "system.ping")
        resynchronized_ping = require_response(emulator.wait_response(902), 902)
        if resynchronized_ping != {"pong": True}:
            raise AssertionError(
                f"unexpected JSON response after delimiter corruption: "
                f"{resynchronized_ping}"
            )

        emulator.send_json(900, "system.ping")
        ping = require_response(emulator.wait_response(900), 900)
        if ping != {"pong": True}:
            raise AssertionError(f"unexpected final ping response: {ping}")
        exit_status = emulator.finish()
        if exit_status not in (0, -15, -9):
            raise AssertionError(f"esp-emu exited with status {exit_status}")
    except BaseException as exc:
        error = exc
    finally:
        if emulator.process is not None and emulator.process.poll() is None:
            emulator.finish()
        process_output = emulator.diagnostics()
        args.uart_log.parent.mkdir(parents=True, exist_ok=True)
        args.uart_log.write_bytes(bytes(emulator.raw))
        args.log.parent.mkdir(parents=True, exist_ok=True)
        escaped_uart = " ".join(f"{byte:02x}" for byte in emulator.raw)
        args.log.write_text(
            "UART bytes (hex):\n"
            + escaped_uart
            + "\n\nUART text lines:\n"
            + "\n".join(emulator.parser.lines)
            + "\n\nesp-emu output:\n"
            + process_output.decode("utf-8", errors="replace"),
            encoding="utf-8",
        )

    if error is not None:
        if isinstance(error, (AssertionError, OSError, ValueError)):
            print(f"ERROR: {error}", file=sys.stderr)
        else:
            print(f"ERROR: unexpected test failure: {error}", file=sys.stderr)
        return 1
    if emulator.fallback_cleanup:
        print("WARNING: final UART response was received; esp-emu required fallback cleanup")
    elif exit_status != 0:
        print(f"ERROR: esp-emu exit status was {exit_status}", file=sys.stderr)
        return 1
    print("PASS: UART BINARY DATA PLANE BIDIRECTIONAL TEST OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
