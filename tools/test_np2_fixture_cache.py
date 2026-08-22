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
    ENCODING_AUTO,
    ENCODING_RAW,
    ENCODING_ZERO_RLE_V1,
    CAPABILITY_ZERO_RLE_V1,
    FRAME_PREFIX,
    RemoteError,
    SerialFileTransferClient,
    SourceChangedError,
    UploadModeError,
    _SerialParser,
    _ZeroRleProducer,
    UploadMetrics,
    analyze_zero_rle,
    derive_cache_path,
    hash_file,
    provision_fixture,
    select_upload_plan,
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


def collect_zero_rle(plan) -> bytes:
    producer = _ZeroRleProducer(plan)
    output = bytearray()
    try:
        while True:
            chunk = producer.read(wire.MAX_PAYLOAD)
            if not chunk:
                break
            assert len(chunk) <= wire.MAX_PAYLOAD
            output.extend(chunk)
        producer.finish()
    finally:
        producer.close()
    return bytes(output)


def decode_zero_rle(encoded: bytes, logical_size: int) -> bytes:
    output = bytearray()
    offset = 0
    while offset < len(encoded):
        assert offset + 5 <= len(encoded)
        tag = encoded[offset]
        length = int.from_bytes(encoded[offset + 1 : offset + 5], "little")
        assert length > 0
        offset += 5
        if tag == 0:
            output.extend(b"\x00" * length)
        elif tag == 1:
            assert offset + length <= len(encoded)
            output.extend(encoded[offset : offset + length])
            assert b"\x00" not in encoded[offset : offset + length]
            offset += length
        else:
            raise AssertionError(f"unknown zero-rle tag {tag}")
    assert len(output) == logical_size
    return bytes(output)


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


def test_zero_rle_encoder() -> None:
    cases = [
        b"",
        b"\x00",
        b"\x01",
        b"\x00" * 4096,
        bytes(range(1, 256)) * 8,
        bytes((0, 1, 0, 2, 0, 3)) * 300,
        b"\x00" * 65537 + b"\x01" * 2049 + b"\x00" * 1025,
        b"\x01" * 1020 + b"\x00" * 7 + b"\x02" * 1025,
    ]
    with tempfile.TemporaryDirectory() as directory:
        for index, data in enumerate(cases):
            path = Path(directory) / f"case-{index}.bin"
            path.write_bytes(data)
            plan = analyze_zero_rle(path)
            encoded = collect_zero_rle(plan)
            assert plan.local.size_bytes == len(data)
            assert plan.local.sha256 == hashlib.sha256(data).hexdigest()
            assert plan.wire_size_bytes == len(encoded)
            assert decode_zero_rle(encoded, len(data)) == data
            assert encoded == collect_zero_rle(plan)


def test_np2_fixture_canonical_shape() -> None:
    fixture = Path(__file__).resolve().parents[1] / \
        "tests/guest/np2test/golden/np2test-fd1232.image"
    plan = analyze_zero_rle(fixture)
    encoded = collect_zero_rle(plan)
    assert plan.local.size_bytes == 1261568
    assert plan.local.sha256 == "3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3"
    assert plan.zero_run_count == 96
    assert plan.literal_record_count == 96
    assert plan.literal_bytes == 852
    assert plan.wire_size_bytes == 1812
    assert len(encoded) == 1812
    assert (len(encoded) + wire.MAX_PAYLOAD - 1) // wire.MAX_PAYLOAD == 2
    assert decode_zero_rle(encoded, 1261568) == fixture.read_bytes()


def test_upload_policy_and_source_mutation() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "fixture.bin"
        path.write_bytes(b"\x00" * 4096)
        assert ENCODING_ZERO_RLE_V1 == "zero-rle-v1"
        assert CAPABILITY_ZERO_RLE_V1 == "file-transfer.zero-rle-v1"
        compressed = select_upload_plan(path, ENCODING_AUTO, {CAPABILITY_ZERO_RLE_V1})
        assert compressed.encoding == ENCODING_ZERO_RLE_V1
        assert compressed.wire_size_bytes < compressed.local.size_bytes
        assert select_upload_plan(path, ENCODING_AUTO, set()).encoding == ENCODING_RAW
        assert select_upload_plan(
            path, ENCODING_AUTO, {ENCODING_ZERO_RLE_V1}
        ).encoding == ENCODING_RAW
        assert select_upload_plan(path, ENCODING_RAW, {CAPABILITY_ZERO_RLE_V1}).encoding == ENCODING_RAW
        try:
            select_upload_plan(path, ENCODING_ZERO_RLE_V1, set())
        except UploadModeError:
            pass
        else:
            raise AssertionError("explicit compressed mode accepted absent capability")
        try:
            select_upload_plan(path, ENCODING_ZERO_RLE_V1, {ENCODING_ZERO_RLE_V1})
        except UploadModeError:
            pass
        else:
            raise AssertionError("short encoding name was accepted as a capability")

        path.write_bytes(b"\x00" * 4096 + b"\x01")
        plan = analyze_zero_rle(path)
        producer = _ZeroRleProducer(plan)
        path.write_bytes(b"\x00" * 4096 + b"\x02")
        try:
            while producer.read(wire.MAX_PAYLOAD):
                pass
            try:
                producer.finish()
            except SourceChangedError:
                pass
            else:
                raise AssertionError("source mutation was not rejected")
        finally:
            producer.close()


def test_supplied_upload_plan_consistency() -> None:
    with tempfile.TemporaryDirectory() as directory:
        raw_path = Path(directory) / "raw.bin"
        raw_path.write_bytes(b"R")
        raw_plan = select_upload_plan(raw_path, ENCODING_RAW, set())

        compressed_path = Path(directory) / "compressed.bin"
        compressed_path.write_bytes(b"\x00" * 4096)
        compressed_plan = select_upload_plan(
            compressed_path, ENCODING_ZERO_RLE_V1, {CAPABILITY_ZERO_RLE_V1}
        )
        compressed_raw_plan = select_upload_plan(
            compressed_path, ENCODING_RAW, set()
        )

        def upload_with_plan(path: Path, mode: str, plan, capabilities: set[str]):
            client = object.__new__(SerialFileTransferClient)
            client.capabilities = frozenset(capabilities)

            def request(command: str, params: dict | None = None,
                        timeout: float | None = None) -> dict:
                if command == "file.write.begin":
                    result = {
                        "transfer_id": 23,
                        "size_bytes": plan.local.size_bytes,
                    }
                    if plan.encoding == ENCODING_ZERO_RLE_V1:
                        result.update({
                            "encoding": ENCODING_ZERO_RLE_V1,
                            "wire_size_bytes": plan.wire_size_bytes,
                        })
                    return result
                if command == "file.transfer.status":
                    result = {
                        "file_state": "completed",
                        "size_bytes": plan.local.size_bytes,
                        "transferred_bytes": plan.local.size_bytes,
                    }
                    if plan.encoding == ENCODING_ZERO_RLE_V1:
                        result.update({
                            "encoding": ENCODING_ZERO_RLE_V1,
                            "wire_size_bytes": plan.wire_size_bytes,
                            "wire_transferred_bytes": plan.wire_size_bytes,
                        })
                    return result
                raise AssertionError(f"unexpected command {command}")

            client.request = request
            client.send_raw = lambda data: len(data)
            client.wait_frame = lambda timeout: {
                "type": wire.ACK,
                "transfer_id": 23,
                "sequence": 1,
                "offset": plan.wire_size_bytes,
                "crc_valid": True,
            }
            with patch.object(
                    fixture_cache, "select_upload_plan",
                    side_effect=AssertionError("supplied plan was recomputed")):
                return client.upload(
                    "/plan-check.bin", path, False, encoding=mode, plan=plan
                )

        def assert_rejected(path: Path, mode: str, plan,
                            capabilities: set[str]) -> None:
            try:
                upload_with_plan(path, mode, plan, capabilities)
            except UploadModeError:
                return
            raise AssertionError(f"accepted invalid supplied plan for mode {mode}")

        # raw + raw plan: accepted.
        assert upload_with_plan(raw_path, ENCODING_RAW, raw_plan, set()).encoding == ENCODING_RAW
        # raw + compressed plan: rejected.
        assert_rejected(compressed_path, ENCODING_RAW, compressed_plan,
                        {CAPABILITY_ZERO_RLE_V1})
        # zero-rle-v1 + compressed plan + capability: accepted.
        assert upload_with_plan(
            compressed_path, ENCODING_ZERO_RLE_V1, compressed_plan,
            {CAPABILITY_ZERO_RLE_V1}
        ).encoding == ENCODING_ZERO_RLE_V1
        # supplied compressed plan + short encoding name only: rejected.
        assert_rejected(compressed_path, ENCODING_ZERO_RLE_V1,
                        compressed_plan, {ENCODING_ZERO_RLE_V1})
        # zero-rle-v1 + raw plan: rejected.
        assert_rejected(compressed_path, ENCODING_ZERO_RLE_V1,
                        compressed_raw_plan, {CAPABILITY_ZERO_RLE_V1})
        # zero-rle-v1 + compressed plan without capability: rejected.
        assert_rejected(compressed_path, ENCODING_ZERO_RLE_V1,
                        compressed_plan, set())
        # auto + raw plan: accepted.
        assert upload_with_plan(raw_path, ENCODING_AUTO, raw_plan, set()).encoding == ENCODING_RAW
        # auto + compressed plan + capability: accepted.
        assert upload_with_plan(
            compressed_path, ENCODING_AUTO, compressed_plan,
            {CAPABILITY_ZERO_RLE_V1}
        ).encoding == ENCODING_ZERO_RLE_V1
        # auto + compressed plan without capability: rejected.
        assert_rejected(compressed_path, ENCODING_AUTO, compressed_plan, set())


def test_capability_discovery() -> None:
    client = object.__new__(SerialFileTransferClient)
    sent: list[bytes] = []

    def send_raw(data: bytes) -> int:
        sent.append(data)
        return len(data)

    def request(command: str, params: dict | None = None,
                timeout: float | None = None) -> dict:
        assert command == "protocol.hello"
        return {"protocol_version": 1,
                "capabilities": ["file-transfer.v1", CAPABILITY_ZERO_RLE_V1]}

    client.send_raw = send_raw
    client.request = request
    hello = client.sync()
    assert CAPABILITY_ZERO_RLE_V1 in client.capabilities
    assert ENCODING_ZERO_RLE_V1 not in client.capabilities
    assert hello["protocol_version"] == 1
    assert sent == [b"\x00\x00\x00\x00"]


def test_retry_retains_encoded_frame() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "retry.bin"
        path.write_bytes(b"\x00" * 1024)
        plan = analyze_zero_rle(path)
        client = object.__new__(SerialFileTransferClient)
        client.capabilities = frozenset({CAPABILITY_ZERO_RLE_V1})
        sent: list[bytes] = []
        waits = [TimeoutError("simulated lost ACK"), {
            "type": wire.ACK,
            "transfer_id": 17,
            "sequence": 1,
            "offset": plan.wire_size_bytes,
            "crc_valid": True,
        }]

        def request(command: str, params: dict | None = None,
                    timeout: float | None = None) -> dict:
            if command == "file.write.begin":
                assert params == {
                    "path": "/retry.bin",
                    "size_bytes": 1024,
                    "replace": False,
                    "encoding": ENCODING_ZERO_RLE_V1,
                    "wire_size_bytes": plan.wire_size_bytes,
                }
                return {"transfer_id": 17, "size_bytes": 1024,
                        "encoding": ENCODING_ZERO_RLE_V1,
                        "wire_size_bytes": plan.wire_size_bytes}
            if command == "file.transfer.status":
                return {"file_state": "completed", "size_bytes": 1024,
                        "transferred_bytes": 1024, "encoding": ENCODING_ZERO_RLE_V1,
                        "wire_size_bytes": plan.wire_size_bytes,
                        "wire_transferred_bytes": plan.wire_size_bytes}
            raise AssertionError(f"unexpected command {command}")

        def send_raw(data: bytes) -> int:
            sent.append(data)
            return len(data)

        def wait_frame(timeout: float) -> dict:
            result = waits.pop(0)
            if isinstance(result, BaseException):
                raise result
            return result

        client.request = request
        client.send_raw = send_raw
        client.wait_frame = wait_frame
        metrics = client.upload("/retry.bin", path, False,
                                encoding=ENCODING_ZERO_RLE_V1, plan=plan)
        assert metrics.retries == 1
        assert metrics.bytes_sent == plan.wire_size_bytes
        assert len(sent) == 2
        assert sent[0] == sent[1]


def make_serial_probe(files: dict[str, bytes] | None = None) -> tuple[SerialFileTransferClient, list[tuple[str, float | None]]]:
    client = object.__new__(SerialFileTransferClient)
    client.timeout = DEFAULT_CONTROL_TIMEOUT
    client.hash_timeout = DEFAULT_HASH_TIMEOUT
    client.capabilities = frozenset()
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

    def upload(path: str, local_path: Path, replace: bool,
               encoding: str = ENCODING_RAW, plan=None) -> UploadMetrics:
        if not replace and path in client.files:
            raise RemoteError("ALREADY_EXISTS", "already present")
        data = local_path.read_bytes()
        client.files[path] = data
        wire_size = plan.wire_size_bytes if plan is not None else len(data)
        return UploadMetrics(
            bytes_sent=wire_size,
            frames=(wire_size + 1023) // 1024,
            encoding=encoding,
            logical_size_bytes=len(data),
            wire_size_bytes=wire_size,
            logical_bytes_sent=len(data),
            wire_bytes_sent=wire_size,
        )

    client.request = request
    client.upload = upload
    return client, calls


class FakeClient:
    def __init__(self, files: dict[str, bytes] | None = None,
                 capabilities: set[str] | None = None) -> None:
        self.files = dict(files or {})
        self.capabilities = frozenset(capabilities or set())
        self.upload_calls: list[tuple[str, int, bool, str, int]] = []

    def stat(self, path: str) -> dict:
        if path not in self.files:
            raise RemoteError("NOT_FOUND", "missing")
        return {"path": path, "type": "file", "size_bytes": len(self.files[path])}

    def sha256(self, path: str) -> dict:
        data = self.files[path]
        return {"path": path, "size_bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}

    def upload(self, path: str, local_path: Path, replace: bool,
               encoding: str = ENCODING_RAW, plan=None) -> UploadMetrics:
        if not replace and path in self.files:
            raise RemoteError("ALREADY_EXISTS", "already present")
        data = local_path.read_bytes()
        self.files[path] = data
        wire_size = plan.wire_size_bytes if plan is not None else len(data)
        self.upload_calls.append((path, len(data), replace, encoding, wire_size))
        return UploadMetrics(
            bytes_sent=wire_size,
            frames=(wire_size + 1023) // 1024,
            encoding=encoding,
            logical_size_bytes=len(data),
            wire_size_bytes=wire_size,
            logical_bytes_sent=len(data),
            wire_bytes_sent=wire_size,
        )


def run() -> None:
    test_serial_constructor_and_pump()
    test_pump_preserves_parser_byte_stream()
    test_request_deadline_semantics()
    test_zero_rle_encoder()
    test_np2_fixture_canonical_shape()
    test_upload_policy_and_source_mutation()
    test_supplied_upload_plan_consistency()
    test_capability_discovery()
    test_retry_retains_encoded_frame()

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
        assert first_client.upload_calls == [
            (path, 1261568, False, ENCODING_RAW, 1261568)
        ]
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
        assert corrupt_client.upload_calls == [
            (path, 1261568, True, ENCODING_RAW, 1261568)
        ]

        wrong_size_path = path
        wrong_size_client = FakeClient({wrong_size_path: b"short"})
        wrong_size = provision_fixture(wrong_size_client, fixture, emit=lambda _: None)
        assert wrong_size.miss_reason == "size_mismatch"
        assert wrong_size_client.upload_calls == [
            (path, 1261568, True, ENCODING_RAW, 1261568)
        ]

        sparse = Path(directory) / "sparse.bin"
        sparse.write_bytes(b"\x00" * 4096)
        compressed_client = FakeClient(capabilities={CAPABILITY_ZERO_RLE_V1})
        compressed = provision_fixture(compressed_client, sparse,
                                        stem="sparse", emit=lambda _: None)
        assert not compressed.cache_hit
        assert compressed.encoding == ENCODING_ZERO_RLE_V1
        assert compressed.fixture_upload_bytes == 5
        assert compressed.wire_size_bytes == 5
        assert compressed_client.upload_calls[-1] == (
            compressed.cache_path, 4096, False, ENCODING_ZERO_RLE_V1, 5
        )

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
