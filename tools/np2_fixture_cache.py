#!/usr/bin/env python3
"""Provision and verify a content-addressed NP2 fixture over UART File Transfer."""

from __future__ import annotations

import argparse
from collections import deque
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import time
from typing import Callable, Iterator, Protocol

try:
    import serial
except ImportError:  # pragma: no cover - only needed by the physical CLI
    serial = None

from emu import uart_binary_data_plane_test as wire


FRAME_PREFIX = b"@ESP-NP2 "
TRANSPORT_SYNC = b"\x00\x00\x00\x00"
SHA256_HEX = re.compile(r"^[0-9a-f]{64}$")
SAFE_STEM = re.compile(r"^[A-Za-z0-9._-]+$")
MAX_COMPONENT_BYTES = 128
MAX_PATH_BYTES = 192
DEFAULT_CONTROL_TIMEOUT = 5.0
DEFAULT_HASH_TIMEOUT = 30.0
SERIAL_READ_MAX_BYTES = 4096
SERIAL_POLL_INTERVAL_SECONDS = 0.001
ENCODING_RAW = "raw"
ENCODING_ZERO_RLE_V1 = "zero-rle-v1"
ENCODING_AUTO = "auto"
ZERO_RLE_SCAN_BYTES = 64 * 1024
ZERO_RLE_RECORD_OVERHEAD = 5
ZERO_RLE_MAX_RECORD_BYTES = 0xFFFFFFFF


class RemoteError(RuntimeError):
    def __init__(self, code: str, message: str, response: dict | None = None) -> None:
        super().__init__(f"remote File Transfer error {code}: {message}")
        self.code = code
        self.message = message
        self.response = response


class UploadModeError(ValueError):
    """The requested upload mode cannot be used with the peer."""


class SourceChangedError(IOError):
    """The source file changed between analysis and streaming."""


@dataclass(frozen=True)
class LocalFile:
    path: Path
    size_bytes: int
    sha256: str


@dataclass(frozen=True)
class UploadPlan:
    local: LocalFile
    encoding: str
    wire_size_bytes: int
    source_signature: tuple[int, int]
    zero_run_count: int = 0
    literal_record_count: int = 0
    literal_bytes: int = 0


@dataclass
class UploadMetrics:
    # bytes_sent is the established transport-payload metric. For a
    # compressed upload it is the encoded wire byte count, not logical bytes.
    bytes_sent: int = 0
    frames: int = 0
    retries: int = 0
    nacks: int = 0
    duplicate_frames: int = 0
    protocol_timeouts: int = 0
    elapsed: float = 0.0
    encoding: str = ENCODING_RAW
    logical_size_bytes: int = 0
    wire_size_bytes: int = 0
    logical_bytes_sent: int = 0
    wire_bytes_sent: int = 0


@dataclass
class ProvisionResult:
    cache_path: str
    size_bytes: int
    sha256: str
    cache_hit: bool
    miss_reason: str | None
    fixture_upload_bytes: int
    preflight_seconds: float
    upload_seconds: float
    verify_seconds: float
    total_seconds: float
    upload: UploadMetrics | None
    encoding: str = "none"
    wire_size_bytes: int = 0


class FixtureClient(Protocol):
    def stat(self, path: str) -> dict: ...

    def sha256(self, path: str) -> dict: ...

    def upload(self, path: str, local_path: Path, replace: bool,
               encoding: str = ENCODING_RAW,
               plan: UploadPlan | None = None) -> UploadMetrics: ...


def hash_file(path: Path) -> LocalFile:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        while True:
            chunk = source.read(64 * 1024)
            if not chunk:
                break
            digest.update(chunk)
            size += len(chunk)
    return LocalFile(path, size, digest.hexdigest())


def _source_signature(path: Path) -> tuple[int, int]:
    stat = path.stat()
    return stat.st_size, stat.st_mtime_ns


def _record_wire_size(kind_is_literal: bool, length: int) -> int:
    if length <= 0 or length > ZERO_RLE_MAX_RECORD_BYTES:
        raise ValueError("zero-rle-v1 record length is out of range")
    return ZERO_RLE_RECORD_OVERHEAD + (length if kind_is_literal else 0)


def analyze_zero_rle(path: Path) -> UploadPlan:
    """Pass 1: hash and count a canonical zero-rle-v1 stream."""

    digest = hashlib.sha256()
    size = 0
    wire_size = 0
    zero_runs = 0
    literal_records = 0
    literal_bytes = 0
    current_literal: bool | None = None
    current_length = 0

    def finish_record() -> None:
        nonlocal wire_size, zero_runs, literal_records, literal_bytes
        if current_literal is None:
            return
        wire_size += _record_wire_size(current_literal, current_length)
        if current_literal:
            literal_records += 1
            literal_bytes += current_length
        else:
            zero_runs += 1

    with path.open("rb") as source:
        while True:
            chunk = source.read(ZERO_RLE_SCAN_BYTES)
            if not chunk:
                break
            digest.update(chunk)
            size += len(chunk)
            for byte in chunk:
                literal = byte != 0
                if current_literal is None:
                    current_literal = literal
                    current_length = 1
                elif literal == current_literal:
                    current_length += 1
                else:
                    finish_record()
                    current_literal = literal
                    current_length = 1
    finish_record()
    local = LocalFile(path, size, digest.hexdigest())
    return UploadPlan(
        local=local,
        encoding=ENCODING_ZERO_RLE_V1,
        wire_size_bytes=wire_size,
        source_signature=_source_signature(path),
        zero_run_count=zero_runs,
        literal_record_count=literal_records,
        literal_bytes=literal_bytes,
    )


def _raw_upload_plan(path: Path, local: LocalFile | None = None) -> UploadPlan:
    if local is None:
        local = hash_file(path)
    return UploadPlan(
        local=local,
        encoding=ENCODING_RAW,
        wire_size_bytes=local.size_bytes,
        source_signature=_source_signature(path),
    )


def select_upload_plan(path: Path, encoding: str,
                       capabilities: set[str] | frozenset[str] = frozenset()) -> UploadPlan:
    """Select raw, explicit zero-rle-v1, or the safe AUTO policy."""

    if encoding not in {ENCODING_RAW, ENCODING_ZERO_RLE_V1, ENCODING_AUTO}:
        raise UploadModeError(f"unsupported upload encoding mode: {encoding}")
    supports_zero_rle = ENCODING_ZERO_RLE_V1 in capabilities
    if encoding == ENCODING_ZERO_RLE_V1 and not supports_zero_rle:
        raise UploadModeError(
            "zero-rle-v1 upload requires protocol.hello capability "
            "file-transfer.zero-rle-v1"
        )
    if encoding == ENCODING_RAW or (encoding == ENCODING_AUTO and not supports_zero_rle):
        return _raw_upload_plan(path)

    compressed = analyze_zero_rle(path)
    if encoding == ENCODING_ZERO_RLE_V1 or compressed.wire_size_bytes < compressed.local.size_bytes:
        return compressed
    return _raw_upload_plan(path, compressed.local)


def _validate_supplied_upload_plan(encoding: str, plan: UploadPlan,
                                   capabilities: set[str] | frozenset[str]) -> None:
    """Validate a caller-supplied plan without reselecting or recomputing it."""

    if plan.encoding not in {ENCODING_RAW, ENCODING_ZERO_RLE_V1}:
        raise UploadModeError(f"unsupported upload plan encoding: {plan.encoding}")
    if encoding == ENCODING_RAW and plan.encoding != ENCODING_RAW:
        raise UploadModeError("raw upload cannot use a compressed upload plan")
    if encoding == ENCODING_ZERO_RLE_V1 and plan.encoding != ENCODING_ZERO_RLE_V1:
        raise UploadModeError("zero-rle-v1 upload requires a compressed upload plan")
    if (plan.encoding == ENCODING_ZERO_RLE_V1 and
            ENCODING_ZERO_RLE_V1 not in capabilities):
        raise UploadModeError(
            "zero-rle-v1 upload requires protocol.hello capability "
            "file-transfer.zero-rle-v1"
        )


def _scan_run(source, start: int) -> tuple[bool, int]:
    """Return (literal, length) for the run beginning at start."""

    source.seek(start)
    first = source.read(1)
    if not first:
        raise SourceChangedError("source ended while scanning a zero-rle run")
    literal = first[0] != 0
    length = 1
    while True:
        chunk = source.read(ZERO_RLE_SCAN_BYTES)
        if not chunk:
            return literal, length
        for index, byte in enumerate(chunk):
            if (byte != 0) != literal:
                return literal, length + index
        length += len(chunk)
        if length > ZERO_RLE_MAX_RECORD_BYTES:
            raise ValueError("zero-rle-v1 record length exceeds uint32")


class _ByteProducer(Protocol):
    def read(self, maximum: int) -> bytes: ...
    def finish(self) -> None: ...
    def close(self) -> None: ...


class _RawProducer:
    def __init__(self, plan: UploadPlan) -> None:
        self.plan = plan
        if _source_signature(plan.local.path) != plan.source_signature:
            raise SourceChangedError("source metadata changed before raw upload")
        self.source = plan.local.path.open("rb")
        self.digest = hashlib.sha256()
        self.logical_read = 0
        self.done = plan.local.size_bytes == 0
        self.closed = False

    def read(self, maximum: int) -> bytes:
        if self.done:
            return b""
        data = self.source.read(maximum)
        if not data:
            raise SourceChangedError("source ended before the planned raw upload size")
        self.digest.update(data)
        self.logical_read += len(data)
        if self.logical_read == self.plan.local.size_bytes:
            self.done = True
        return data

    def finish(self) -> None:
        if not self.done or self.logical_read != self.plan.local.size_bytes:
            raise SourceChangedError("raw producer was not fully consumed")
        if (self.digest.hexdigest() != self.plan.local.sha256 or
                _source_signature(self.plan.local.path) != self.plan.source_signature):
            raise SourceChangedError("source changed during raw upload")
        self.close()

    def close(self) -> None:
        if not self.closed:
            self.source.close()
            self.closed = True


class _ZeroRleProducer:
    """Pass 2 bounded canonical encoder with one DATA-sized output buffer."""

    def __init__(self, plan: UploadPlan) -> None:
        self.plan = plan
        if plan.encoding != ENCODING_ZERO_RLE_V1:
            raise ValueError("zero-rle producer requires a compressed upload plan")
        if _source_signature(plan.local.path) != plan.source_signature:
            raise SourceChangedError("source metadata changed before zero-rle upload")
        self.source = plan.local.path.open("rb")
        self.digest = hashlib.sha256()
        self.logical_offset = 0
        self.logical_read = 0
        self.wire_returned = 0
        self.record_literal = False
        self.record_remaining = 0
        self.header = b""
        self.header_offset = 0
        self.done = plan.local.size_bytes == 0
        self.closed = False

    def _start_record(self) -> None:
        literal, length = _scan_run(self.source, self.logical_offset)
        self.source.seek(self.logical_offset)
        tag = b"\x01" if literal else b"\x00"
        self.header = tag + length.to_bytes(4, "little")
        self.header_offset = 0
        self.record_literal = literal
        self.record_remaining = length

    def _consume_zero_run(self) -> None:
        while self.record_remaining:
            count = min(self.record_remaining, ZERO_RLE_SCAN_BYTES)
            data = self.source.read(count)
            if len(data) != count:
                raise SourceChangedError("source ended during zero-rle zero run")
            self.digest.update(data)
            self.logical_read += count
            self.logical_offset += count
            self.record_remaining -= count
        if self.logical_offset >= self.plan.local.size_bytes:
            self.done = True

    def read(self, maximum: int) -> bytes:
        if maximum <= 0:
            raise ValueError("producer maximum must be positive")
        if self.done:
            return b""
        output = bytearray()
        while len(output) < maximum:
            if self.record_remaining == 0:
                if self.logical_offset >= self.plan.local.size_bytes:
                    self.done = True
                    break
                self._start_record()

            if self.header_offset < len(self.header):
                count = min(maximum - len(output), len(self.header) - self.header_offset)
                output.extend(self.header[self.header_offset:self.header_offset + count])
                self.header_offset += count
                if self.header_offset == len(self.header) and not self.record_literal:
                    self._consume_zero_run()
                continue

            if not self.record_literal:
                self._consume_zero_run()
                continue

            count = min(maximum - len(output), self.record_remaining)
            data = self.source.read(count)
            if len(data) != count:
                raise SourceChangedError("source ended during zero-rle encoding")
            self.digest.update(data)
            self.logical_read += count
            self.logical_offset += count
            self.record_remaining -= count
            output.extend(data)
            if self.record_remaining == 0 and self.logical_offset >= self.plan.local.size_bytes:
                self.done = True

        self.wire_returned += len(output)
        return bytes(output)

    def finish(self) -> None:
        if (not self.done or self.logical_read != self.plan.local.size_bytes or
                self.wire_returned != self.plan.wire_size_bytes):
            raise SourceChangedError("zero-rle producer was not fully consumed")
        if (self.digest.hexdigest() != self.plan.local.sha256 or
                _source_signature(self.plan.local.path) != self.plan.source_signature):
            raise SourceChangedError("source changed during zero-rle upload")
        self.close()

    def close(self) -> None:
        if not self.closed:
            self.source.close()
            self.closed = True


def derive_cache_path(stem: str, sha256: str, extension: str = ".hdm") -> str:
    if not SAFE_STEM.fullmatch(stem):
        raise ValueError("fixture stem contains unsupported characters")
    if not SHA256_HEX.fullmatch(sha256):
        raise ValueError("fixture SHA-256 must be 64 lowercase hexadecimal characters")
    if not extension.startswith(".") or not SAFE_STEM.fullmatch(extension[1:]):
        raise ValueError("fixture extension is invalid")
    name = f"{stem}-{sha256}{extension}"
    if len(name.encode("utf-8")) > MAX_COMPONENT_BYTES:
        raise ValueError("content-addressed fixture name is too long")
    path = f"/np2-fixtures/{name}"
    if len(path.encode("utf-8")) > MAX_PATH_BYTES:
        raise ValueError("content-addressed fixture path is too long")
    return path


def provision_fixture(
    client: FixtureClient,
    local_path: Path,
    stem: str = "np2test-fd1232",
    extension: str = ".hdm",
    encoding: str = ENCODING_AUTO,
    emit: Callable[[str], None] = print,
) -> ProvisionResult:
    started = time.monotonic()
    capabilities = set(getattr(client, "capabilities", ()))
    plan = select_upload_plan(local_path, encoding, capabilities)
    local = plan.local
    cache_path = derive_cache_path(stem, local.sha256, extension)
    miss_reason: str | None = None
    preflight_started = time.monotonic()
    try:
        remote_stat = client.stat(cache_path)
    except RemoteError as error:
        if error.code != "NOT_FOUND":
            raise
        miss_reason = "not_found"
    else:
        if remote_stat.get("type") != "file" or remote_stat.get("size_bytes") != local.size_bytes:
            miss_reason = "size_mismatch"
        else:
            remote_hash = client.sha256(cache_path)
            if (remote_hash.get("size_bytes") != local.size_bytes or
                    remote_hash.get("sha256") != local.sha256):
                miss_reason = "sha256_mismatch"

    preflight_seconds = time.monotonic() - preflight_started
    if miss_reason is None:
        total_seconds = time.monotonic() - started
        emit(
            f"NP2_FIXTURE_CACHE=HIT path={cache_path} size_bytes={local.size_bytes} "
            f"sha256={local.sha256} fixture_upload_bytes=0 encoding=none "
            f"wire_size_bytes=0 data_frames=0"
        )
        emit(f"VERIFIED path={cache_path} size_bytes={local.size_bytes} sha256={local.sha256}")
        return ProvisionResult(
            cache_path, local.size_bytes, local.sha256, True, None, 0,
            preflight_seconds, 0.0, 0.0, total_seconds, None,
            "none", 0,
        )

    emit(f"NP2_FIXTURE_CACHE=MISS reason={miss_reason} path={cache_path}")
    upload_started = time.monotonic()
    upload = client.upload(
        cache_path,
        local.path,
        replace=miss_reason != "not_found",
        encoding=plan.encoding,
        plan=plan,
    )
    upload_seconds = time.monotonic() - upload_started
    emit(
        f"UPLOAD_COMPLETE path={cache_path} bytes={upload.bytes_sent} "
        f"encoding={upload.encoding} logical_size_bytes={upload.logical_size_bytes} "
        f"wire_size_bytes={upload.wire_size_bytes} data_frames={upload.frames} "
        f"elapsed={upload_seconds:.3f}s retries={upload.retries} nacks={upload.nacks} "
        f"duplicates={upload.duplicate_frames} timeouts={upload.protocol_timeouts}"
    )

    verify_started = time.monotonic()
    verified_stat = client.stat(cache_path)
    verified_hash = client.sha256(cache_path)
    if (verified_stat.get("type") != "file" or
            verified_stat.get("size_bytes") != local.size_bytes or
            verified_hash.get("size_bytes") != local.size_bytes or
            verified_hash.get("sha256") != local.sha256):
        raise AssertionError(
            f"post-upload fixture verification failed path={cache_path} "
            f"stat={verified_stat} hash={verified_hash} expected_size={local.size_bytes} "
            f"expected_sha256={local.sha256}"
        )
    verify_seconds = time.monotonic() - verify_started
    total_seconds = time.monotonic() - started
    emit(f"VERIFIED path={cache_path} size_bytes={local.size_bytes} sha256={local.sha256}")
    return ProvisionResult(
        cache_path, local.size_bytes, local.sha256, False, miss_reason,
        upload.bytes_sent, preflight_seconds, upload_seconds, verify_seconds,
        total_seconds, upload, upload.encoding, upload.wire_size_bytes,
    )


class _SerialParser:
    """Small UART demultiplexer for newline control responses and COBS frames."""

    def __init__(self) -> None:
        self.mode = "text"
        self.text = bytearray()
        self.encoded = bytearray()
        self.lines: deque[dict] = deque()
        self.frames: deque[dict] = deque()

    def _finish_text_line(self) -> None:
        if not self.text:
            return
        line = bytes(self.text).rstrip(b"\r")
        self.text.clear()
        index = line.find(FRAME_PREFIX)
        if index < 0:
            return
        try:
            value = json.loads(line[index + len(FRAME_PREFIX):].decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return
        if isinstance(value, dict):
            self.lines.append(value)

    def feed(self, data: bytes) -> None:
        for byte in data:
            if self.mode == "text":
                if byte == 0:
                    self.mode = "zero"
                else:
                    self.text.append(byte)
                    if byte == 0x0A:
                        self._finish_text_line()
            elif self.mode == "zero":
                if byte == 0:
                    self.encoded.clear()
                    self.mode = "binary"
                else:
                    self.mode = "text"
                    self.text.append(byte)
                    if byte == 0x0A:
                        self._finish_text_line()
            elif self.mode == "binary":
                if byte == 0:
                    if self.encoded:
                        try:
                            self.frames.append(wire.parse_frame(wire.cobs_decode(bytes(self.encoded))))
                        except (AssertionError, ValueError):
                            pass
                    self.encoded.clear()
                    self.mode = "text"
                elif len(self.encoded) < wire.MAX_ENCODED:
                    self.encoded.append(byte)
                else:
                    self.mode = "discard"
            else:
                if byte == 0:
                    self.mode = "text"
                    self.encoded.clear()


class SerialFileTransferClient:
    def __init__(self, port: str, baud: int = 115200,
                 timeout: float = DEFAULT_CONTROL_TIMEOUT,
                 hash_timeout: float = DEFAULT_HASH_TIMEOUT) -> None:
        if serial is None:
            raise RuntimeError("pyserial is required for the physical cache CLI")
        self.serial = serial.Serial(port, baudrate=baud, timeout=0.0,
                                    write_timeout=timeout)
        self.timeout = timeout
        self.hash_timeout = hash_timeout
        self.parser = _SerialParser()
        self.request_id = 1
        self.capabilities: frozenset[str] = frozenset()

    def close(self) -> None:
        self.serial.close()

    def __enter__(self) -> "SerialFileTransferClient":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def send_raw(self, data: bytes) -> int:
        written = self.serial.write(data)
        if written != len(data):
            raise IOError(f"short UART write: {written}/{len(data)}")
        return written

    def _pump(self) -> None:
        available = self.serial.in_waiting
        if available == 0:
            time.sleep(SERIAL_POLL_INTERVAL_SECONDS)
            return
        data = self.serial.read(min(available, SERIAL_READ_MAX_BYTES))
        if data:
            self.parser.feed(data)

    def wait_response(self, request_id: int, timeout: float | None = None) -> dict:
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while time.monotonic() < deadline:
            for index, response in enumerate(self.parser.lines):
                if response.get("id") == request_id:
                    del self.parser.lines[index]
                    return response
            self._pump()
        raise TimeoutError(f"JSON response timeout for request id {request_id}")

    def wait_frame(self, timeout: float) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.parser.frames:
                return self.parser.frames.popleft()
            self._pump()
        raise TimeoutError("binary frame timeout")

    def request(self, command: str, params: dict | None = None, timeout: float | None = None) -> dict:
        request_id = self.request_id
        self.request_id += 1
        request: dict[str, object] = {"v": 1, "id": request_id, "cmd": command}
        if params is not None:
            request["params"] = params
        payload = (FRAME_PREFIX + json.dumps(request, separators=(",", ":")).encode("ascii") + b"\n")
        self.send_raw(payload)
        response = self.wait_response(request_id, timeout)
        if response.get("ok") is not True:
            error = response.get("error", {})
            raise RemoteError(str(error.get("code", "UNKNOWN")),
                              str(error.get("message", "request failed")), response)
        result = response.get("result")
        if not isinstance(result, dict):
            raise RemoteError("INTERNAL_ERROR", "response result is not an object", response)
        return result

    def sync(self) -> dict:
        self.send_raw(TRANSPORT_SYNC)
        hello = self.request("protocol.hello")
        if hello.get("protocol_version") != 1 or "file-transfer.v1" not in hello.get("capabilities", []):
            raise AssertionError(f"unexpected protocol.hello result: {hello}")
        capabilities = hello.get("capabilities", [])
        if not isinstance(capabilities, list):
            raise AssertionError(f"protocol.hello capabilities are invalid: {hello}")
        self.capabilities = frozenset(str(value) for value in capabilities)
        return hello

    def ping(self) -> dict:
        return self.request("system.ping")

    def stat(self, path: str) -> dict:
        return self.request("file.stat", {"path": path})

    def sha256(self, path: str) -> dict:
        return self.request("file.sha256", {"path": path}, timeout=self.hash_timeout)

    def _abort_upload(self, transfer_id: int) -> None:
        try:
            self.request("binary.transfer.abort", {"transfer_id": transfer_id})
        except Exception:
            # The receiver may already have committed the final frame. The
            # source mismatch is still reported to the caller below.
            pass

    def upload(self, path: str, local_path: Path, replace: bool,
               encoding: str = ENCODING_RAW,
               plan: UploadPlan | None = None) -> UploadMetrics:
        if encoding not in {ENCODING_RAW, ENCODING_ZERO_RLE_V1, ENCODING_AUTO}:
            raise UploadModeError(f"unsupported upload encoding mode: {encoding}")
        capabilities = set(self.capabilities)
        if plan is None:
            plan = select_upload_plan(local_path, encoding, capabilities)
        elif plan.local.path != local_path:
            raise ValueError("upload plan does not match local path")
        else:
            _validate_supplied_upload_plan(encoding, plan, capabilities)
        selected_encoding = plan.encoding

        producer: _ByteProducer
        if selected_encoding == ENCODING_ZERO_RLE_V1:
            producer = _ZeroRleProducer(plan)
        else:
            producer = _RawProducer(plan)

        begin_params: dict[str, object] = {
            "path": path,
            "size_bytes": plan.local.size_bytes,
            "replace": replace,
        }
        if selected_encoding == ENCODING_ZERO_RLE_V1:
            begin_params["encoding"] = ENCODING_ZERO_RLE_V1
            begin_params["wire_size_bytes"] = plan.wire_size_bytes

        transfer_id: int | None = None
        started = time.monotonic()
        try:
            begin = self.request("file.write.begin", begin_params, timeout=120.0)
            if selected_encoding == ENCODING_ZERO_RLE_V1:
                if (begin.get("encoding") != ENCODING_ZERO_RLE_V1 or
                        begin.get("size_bytes") != plan.local.size_bytes or
                        begin.get("wire_size_bytes") != plan.wire_size_bytes):
                    raise RemoteError("PROTOCOL_ERROR", f"compressed begin mismatch: {begin}")
            if plan.local.size_bytes == 0:
                producer.finish()
                metrics = UploadMetrics(
                    encoding=selected_encoding,
                    logical_size_bytes=plan.local.size_bytes,
                    wire_size_bytes=plan.wire_size_bytes,
                )
                metrics.logical_bytes_sent = plan.local.size_bytes
                metrics.wire_bytes_sent = 0
                return metrics

            transfer_id = int(begin["transfer_id"])
            metrics = UploadMetrics(
                encoding=selected_encoding,
                logical_size_bytes=plan.local.size_bytes,
                wire_size_bytes=plan.wire_size_bytes,
            )
            sequence = 0
            offset = 0
            while offset < plan.wire_size_bytes:
                payload = producer.read(min(wire.MAX_PAYLOAD, plan.wire_size_bytes - offset))
                if not payload:
                    raise SourceChangedError("producer ended before the planned wire size")
                frame = wire.build_frame(wire.DATA, transfer_id, sequence, offset, payload=payload)
                attempts = 0
                while True:
                    attempts += 1
                    self.send_raw(frame)
                    try:
                        response = self.wait_frame(timeout=30.0)
                    except TimeoutError:
                        metrics.protocol_timeouts += 1
                        if attempts >= 3:
                            raise
                        metrics.retries += 1
                        continue
                    if response["type"] == wire.NACK:
                        metrics.nacks += 1
                        if attempts >= 3:
                            raise RemoteError("NACK", f"upload frame rejected: {response}")
                        metrics.retries += 1
                        continue
                    if response["type"] != wire.ACK:
                        raise RemoteError("PROTOCOL_ERROR", f"unexpected upload response: {response}")
                    if (response["transfer_id"] == transfer_id and
                            response["sequence"] == sequence + 1 and
                            response["offset"] == offset + len(payload) and
                            response["crc_valid"]):
                        break
                    metrics.duplicate_frames += 1
                metrics.frames += 1
                metrics.bytes_sent += len(payload)
                offset += len(payload)
                sequence += 1

            producer.finish()
            if metrics.bytes_sent != plan.wire_size_bytes:
                raise SourceChangedError("emitted wire size differs from the upload plan")
            status = self.request("file.transfer.status", {"transfer_id": transfer_id})
            if (status.get("file_state") != "completed" or
                    status.get("size_bytes") != plan.local.size_bytes or
                    status.get("transferred_bytes") != plan.local.size_bytes):
                raise RemoteError("TRANSFER_FAILED", f"upload did not complete: {status}")
            if selected_encoding == ENCODING_ZERO_RLE_V1 and (
                    status.get("encoding") != ENCODING_ZERO_RLE_V1 or
                    status.get("wire_size_bytes") != plan.wire_size_bytes or
                    status.get("wire_transferred_bytes") != plan.wire_size_bytes):
                raise RemoteError("TRANSFER_FAILED", f"compressed status mismatch: {status}")
            metrics.logical_bytes_sent = plan.local.size_bytes
            metrics.wire_bytes_sent = metrics.bytes_sent
            metrics.elapsed = time.monotonic() - started
            return metrics
        except SourceChangedError:
            if transfer_id is not None:
                self._abort_upload(transfer_id)
            raise
        finally:
            producer.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--stem", default="np2test-fd1232")
    parser.add_argument("--extension", default=".hdm")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=DEFAULT_CONTROL_TIMEOUT)
    parser.add_argument("--hash-timeout", type=float, default=DEFAULT_HASH_TIMEOUT)
    parser.add_argument("--encoding", choices=(ENCODING_RAW, ENCODING_ZERO_RLE_V1, ENCODING_AUTO),
                        default=ENCODING_AUTO,
                        help="fixture upload encoding policy (default: auto)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    with SerialFileTransferClient(args.port, args.baud, args.timeout, args.hash_timeout) as client:
        client.sync()
        result = provision_fixture(client, args.fixture, args.stem, args.extension,
                                   encoding=args.encoding)
        if client.ping() != {"pong": True}:
            raise AssertionError("post-provision system.ping failed")
    print(
        f"NP2_FIXTURE_PROVISION mode={'HIT' if result.cache_hit else 'MISS'} "
        f"path={result.cache_path} fixture_upload_bytes={result.fixture_upload_bytes} "
        f"encoding={result.encoding} logical_size_bytes={result.size_bytes} "
        f"wire_size_bytes={result.wire_size_bytes} "
        f"data_frames={result.upload.frames if result.upload is not None else 0} "
        f"preflight={result.preflight_seconds:.3f}s upload={result.upload_seconds:.3f}s "
        f"verify={result.verify_seconds:.3f}s total={result.total_seconds:.3f}s "
        f"timeout={args.timeout:.3f}s hash_timeout={args.hash_timeout:.3f}s"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
