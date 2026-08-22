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
from typing import Callable, Protocol

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


class RemoteError(RuntimeError):
    def __init__(self, code: str, message: str, response: dict | None = None) -> None:
        super().__init__(f"remote File Transfer error {code}: {message}")
        self.code = code
        self.message = message
        self.response = response


@dataclass(frozen=True)
class LocalFile:
    path: Path
    size_bytes: int
    sha256: str


@dataclass
class UploadMetrics:
    bytes_sent: int = 0
    frames: int = 0
    retries: int = 0
    nacks: int = 0
    duplicate_frames: int = 0
    protocol_timeouts: int = 0
    elapsed: float = 0.0


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


class FixtureClient(Protocol):
    def stat(self, path: str) -> dict: ...

    def sha256(self, path: str) -> dict: ...

    def upload(self, path: str, local_path: Path, replace: bool) -> UploadMetrics: ...


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
    emit: Callable[[str], None] = print,
) -> ProvisionResult:
    started = time.monotonic()
    local = hash_file(local_path)
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
            f"sha256={local.sha256} fixture_upload_bytes=0"
        )
        emit(f"VERIFIED path={cache_path} size_bytes={local.size_bytes} sha256={local.sha256}")
        return ProvisionResult(
            cache_path, local.size_bytes, local.sha256, True, None, 0,
            preflight_seconds, 0.0, 0.0, total_seconds, None,
        )

    emit(f"NP2_FIXTURE_CACHE=MISS reason={miss_reason} path={cache_path}")
    upload_started = time.monotonic()
    upload = client.upload(cache_path, local.path, replace=miss_reason != "not_found")
    upload_seconds = time.monotonic() - upload_started
    emit(
        f"UPLOAD_COMPLETE path={cache_path} bytes={upload.bytes_sent} "
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
        total_seconds, upload,
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
        return hello

    def ping(self) -> dict:
        return self.request("system.ping")

    def stat(self, path: str) -> dict:
        return self.request("file.stat", {"path": path})

    def sha256(self, path: str) -> dict:
        return self.request("file.sha256", {"path": path}, timeout=self.hash_timeout)

    def upload(self, path: str, local_path: Path, replace: bool) -> UploadMetrics:
        local = hash_file(local_path)
        begin = self.request("file.write.begin", {
            "path": path, "size_bytes": local.size_bytes, "replace": replace,
        }, timeout=120.0)
        if local.size_bytes == 0:
            return UploadMetrics()
        transfer_id = int(begin["transfer_id"])
        metrics = UploadMetrics()
        started = time.monotonic()
        with local_path.open("rb") as source:
            sequence = 0
            offset = 0
            while offset < local.size_bytes:
                payload = source.read(min(wire.MAX_PAYLOAD, local.size_bytes - offset))
                if not payload:
                    raise IOError("fixture changed while uploading")
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
        status = self.request("file.transfer.status", {"transfer_id": transfer_id})
        if status.get("file_state") != "completed" or status.get("transferred_bytes") != local.size_bytes:
            raise RemoteError("TRANSFER_FAILED", f"upload did not complete: {status}")
        metrics.elapsed = time.monotonic() - started
        return metrics


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--stem", default="np2test-fd1232")
    parser.add_argument("--extension", default=".hdm")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=DEFAULT_CONTROL_TIMEOUT)
    parser.add_argument("--hash-timeout", type=float, default=DEFAULT_HASH_TIMEOUT)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    with SerialFileTransferClient(args.port, args.baud, args.timeout, args.hash_timeout) as client:
        client.sync()
        result = provision_fixture(client, args.fixture, args.stem, args.extension)
        if client.ping() != {"pong": True}:
            raise AssertionError("post-provision system.ping failed")
    print(
        f"NP2_FIXTURE_PROVISION mode={'HIT' if result.cache_hit else 'MISS'} "
        f"path={result.cache_path} fixture_upload_bytes={result.fixture_upload_bytes} "
        f"preflight={result.preflight_seconds:.3f}s upload={result.upload_seconds:.3f}s "
        f"verify={result.verify_seconds:.3f}s total={result.total_seconds:.3f}s "
        f"timeout={args.timeout:.3f}s hash_timeout={args.hash_timeout:.3f}s"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
