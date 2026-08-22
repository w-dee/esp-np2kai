#!/usr/bin/env python3
"""Unit tests for the content-addressed NP2 fixture cache helper."""

from __future__ import annotations

import hashlib
from pathlib import Path
import tempfile
from unittest.mock import patch

import np2_fixture_cache as fixture_cache
from emu import uart_binary_data_plane_test as wire
from np2_fixture_cache import (
    DEFAULT_CONTROL_TIMEOUT,
    DEFAULT_HASH_TIMEOUT,
    FRAME_PREFIX,
    RemoteError,
    SerialFileTransferClient,
    _SerialParser,
    UploadMetrics,
    derive_cache_path,
    hash_file,
    provision_fixture,
)


class FakeSerialPump:
    def __init__(self, data: bytes = b"") -> None:
        self.buffer = bytearray(data)
        self.read_calls: list[int] = []

    @property
    def in_waiting(self) -> int:
        return len(self.buffer)

    def read(self, size: int) -> bytes:
        self.read_calls.append(size)
        if not self.buffer:
            raise AssertionError("blocking read was attempted with no available data")
        data = bytes(self.buffer[:size])
        del self.buffer[:size]
        return data

    def close(self) -> None:
        pass


def make_pump_probe(data: bytes = b"") -> SerialFileTransferClient:
    client = object.__new__(SerialFileTransferClient)
    client.serial = FakeSerialPump(data)
    client.parser = _SerialParser()
    return client


def test_serial_constructor_and_pump() -> None:
    factory = type(
        "SerialFactory",
        (),
        {
            "kwargs": None,
            "Serial": lambda self, *args, **kwargs: (
                setattr(self, "kwargs", kwargs) or FakeSerialPump()
            ),
        },
    )()
    with patch.object(fixture_cache, "serial", factory):
        client = SerialFileTransferClient(
            "/dev/fake", baud=1500000, timeout=12.5, hash_timeout=45.0
        )
        assert factory.kwargs == {
            "baudrate": 1500000,
            "timeout": 0.0,
            "write_timeout": 12.5,
        }
        client.close()

    empty_client = make_pump_probe()
    with patch.object(fixture_cache.time, "sleep") as sleep:
        empty_client._pump()
    assert empty_client.serial.read_calls == []
    sleep.assert_called_once_with(fixture_cache.SERIAL_POLL_INTERVAL_SECONDS)

    available_client = make_pump_probe(b"available-now")
    with patch.object(fixture_cache.time, "sleep") as sleep:
        available_client._pump()
    assert available_client.serial.read_calls == [len(b"available-now")]
    sleep.assert_not_called()

    bounded_client = make_pump_probe(bytes(range(256)) * 20)
    bounded_client._pump()
    bounded_client._pump()
    assert bounded_client.serial.read_calls == [
        fixture_cache.SERIAL_READ_MAX_BYTES,
        1024,
    ]


def test_pump_preserves_parser_byte_stream() -> None:
    control = FRAME_PREFIX + b'{"type":"response","id":7,"ok":true,"result":{"pong":true}}\n'
    frame = wire.build_frame(wire.ACK, 42, 3, 1024)
    stream = control + frame
    client = make_pump_probe(stream)
    expected = _SerialParser()
    expected.feed(stream)

    while client.serial.in_waiting:
        client._pump()

    assert list(client.parser.lines) == list(expected.lines)
    assert list(client.parser.frames) == list(expected.frames)


def test_request_deadline_semantics() -> None:
    client = make_pump_probe()
    response = {"id": 7, "ok": True}

    def deliver_response() -> None:
        client.parser.lines.append(response)

    client._pump = deliver_response
    with patch.object(fixture_cache.time, "monotonic", side_effect=[100.0, 100.5, 100.6]):
        assert client.wait_response(7, timeout=1.0) == response

    expired_client = make_pump_probe()
    pump_calls: list[bool] = []
    expired_client._pump = lambda: pump_calls.append(True)
    with patch.object(fixture_cache.time, "monotonic", side_effect=[200.0, 201.1]):
        try:
            expired_client.wait_response(7, timeout=1.0)
        except TimeoutError:
            pass
        else:
            raise AssertionError("expired request deadline did not time out")
    assert pump_calls == []


def make_serial_probe(files: dict[str, bytes] | None = None) -> tuple[SerialFileTransferClient, list[tuple[str, float | None]]]:
    client = object.__new__(SerialFileTransferClient)
    client.timeout = DEFAULT_CONTROL_TIMEOUT
    client.hash_timeout = DEFAULT_HASH_TIMEOUT
    client.files = dict(files or {})
    calls: list[tuple[str, float | None]] = []

    def request(command: str, params: dict | None = None, timeout: float | None = None) -> dict:
        calls.append((command, timeout))
        path = str((params or {}).get("path", ""))
        if command == "system.ping":
            return {"pong": True}
        if command == "file.stat":
            if path not in client.files:
                raise RemoteError("NOT_FOUND", "missing")
            return {"path": path, "type": "file", "size_bytes": len(client.files[path])}
        if command == "file.sha256":
            data = client.files[path]
            return {"path": path, "size_bytes": len(data),
                    "sha256": hashlib.sha256(data).hexdigest()}
        raise AssertionError(f"unexpected command: {command}")

    def upload(path: str, local_path: Path, replace: bool) -> UploadMetrics:
        if not replace and path in client.files:
            raise RemoteError("ALREADY_EXISTS", "already present")
        data = local_path.read_bytes()
        client.files[path] = data
        return UploadMetrics(bytes_sent=len(data), frames=(len(data) + 1023) // 1024)

    client.request = request
    client.upload = upload
    return client, calls


class FakeClient:
    def __init__(self, files: dict[str, bytes] | None = None) -> None:
        self.files = dict(files or {})
        self.upload_calls: list[tuple[str, int, bool]] = []

    def stat(self, path: str) -> dict:
        if path not in self.files:
            raise RemoteError("NOT_FOUND", "missing")
        return {"path": path, "type": "file", "size_bytes": len(self.files[path])}

    def sha256(self, path: str) -> dict:
        data = self.files[path]
        return {"path": path, "size_bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}

    def upload(self, path: str, local_path: Path, replace: bool) -> UploadMetrics:
        if not replace and path in self.files:
            raise RemoteError("ALREADY_EXISTS", "already present")
        data = local_path.read_bytes()
        self.files[path] = data
        self.upload_calls.append((path, len(data), replace))
        return UploadMetrics(bytes_sent=len(data), frames=(len(data) + 1023) // 1024)


def run() -> None:
    test_serial_constructor_and_pump()
    test_pump_preserves_parser_byte_stream()
    test_request_deadline_semantics()

    payload = bytes((index * 17 + 3) & 0xFF for index in range(1261568))
    with tempfile.TemporaryDirectory() as directory:
        fixture = Path(directory) / "np2test-fd1232.hdm"
        fixture.write_bytes(payload)
        local = hash_file(fixture)
        assert local.size_bytes == 1261568
        assert local.sha256 == hashlib.sha256(payload).hexdigest()
        path = derive_cache_path("np2test-fd1232", local.sha256)
        assert len(path.rsplit("/", 1)[1].encode("utf-8")) <= 128

        first_client = FakeClient()
        first_markers: list[str] = []
        first = provision_fixture(first_client, fixture, emit=first_markers.append)
        assert not first.cache_hit and first.miss_reason == "not_found"
        assert first.fixture_upload_bytes == 1261568
        assert first_client.upload_calls == [(path, 1261568, False)]
        assert any(marker.startswith("UPLOAD_COMPLETE ") for marker in first_markers)

        hit_markers: list[str] = []
        hit = provision_fixture(first_client, fixture, emit=hit_markers.append)
        assert hit.cache_hit and hit.fixture_upload_bytes == 0
        assert len(first_client.upload_calls) == 1
        assert hit_markers[0].startswith("NP2_FIXTURE_CACHE=HIT ")
        assert "fixture_upload_bytes=0" in hit_markers[0]

        corrupt_client = FakeClient({path: bytes(len(payload))})
        corrupt = provision_fixture(corrupt_client, fixture, emit=lambda _: None)
        assert not corrupt.cache_hit and corrupt.miss_reason == "sha256_mismatch"
        assert corrupt.fixture_upload_bytes == 1261568
        assert corrupt_client.upload_calls == [(path, 1261568, True)]

        wrong_size_path = path
        wrong_size_client = FakeClient({wrong_size_path: b"short"})
        wrong_size = provision_fixture(wrong_size_client, fixture, emit=lambda _: None)
        assert wrong_size.miss_reason == "size_mismatch"
        assert wrong_size_client.upload_calls == [(path, 1261568, True)]

        assert derive_cache_path("np2test-fd1232", local.sha256) == path

        timeout_client, timeout_calls = make_serial_probe({path: payload})
        timeout_client.ping()
        timeout_client.sha256(path)
        assert timeout_client.timeout == DEFAULT_CONTROL_TIMEOUT
        assert timeout_client.hash_timeout == DEFAULT_HASH_TIMEOUT
        assert timeout_calls == [
            ("system.ping", None),
            ("file.sha256", DEFAULT_HASH_TIMEOUT),
        ]

        hit_client, hit_calls = make_serial_probe({path: payload})
        hit = provision_fixture(hit_client, fixture, emit=lambda _: None)
        assert hit.cache_hit and hit.fixture_upload_bytes == 0
        assert hit_calls == [
            ("file.stat", None),
            ("file.sha256", DEFAULT_HASH_TIMEOUT),
        ]

        verify_client, verify_calls = make_serial_probe()
        replaced = provision_fixture(verify_client, fixture, emit=lambda _: None)
        assert not replaced.cache_hit and replaced.fixture_upload_bytes == 1261568
        assert verify_calls[-2:] == [
            ("file.stat", None),
            ("file.sha256", DEFAULT_HASH_TIMEOUT),
        ]

    print("PASS: NP2 fixture cache helper tests")


if __name__ == "__main__":
    run()
