#!/usr/bin/env python3
"""Unit tests for the content-addressed NP2 fixture cache helper."""

from __future__ import annotations

import hashlib
from pathlib import Path
import tempfile

from np2_fixture_cache import (
    RemoteError,
    UploadMetrics,
    derive_cache_path,
    hash_file,
    provision_fixture,
)


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

    print("PASS: NP2 fixture cache helper tests")


if __name__ == "__main__":
    run()
