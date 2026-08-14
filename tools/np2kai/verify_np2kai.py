#!/usr/bin/env python3
"""Verify an imported NP2kai snapshot without network or firmware tools."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import stat
import sys
from typing import Any


TOOLS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR))

from import_np2kai import (  # noqa: E402
    EXPECTED_GENERATED,
    ImportError as ImportFailure,
    blob_bytes,
    render_license_map,
    render_readme,
    render_hashes,
    normalize_url,
    tree_mode,
    validate_manifest,
    validate_source,
)


EXPECTED_URL = "https://github.com/AZO234/NP2kai"
EXPECTED_COMMIT = "e2dc9046aa5c786fcfbfb87e883457e421026e31"
EXPECTED_VERSION = "0.86.0.22"
HASH_LINE = re.compile(r"^([0-9a-f]{64})  (.+)$")


class VerificationError(RuntimeError):
    pass


def load_manifest(root: Path) -> tuple[dict[str, Any], bytes]:
    path = root / "import-manifest.json"
    try:
        data = path.read_bytes()
        manifest = json.loads(data.decode("utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise VerificationError(f"cannot read manifest: {exc}") from exc
    if not isinstance(manifest, dict):
        raise VerificationError("manifest root must be an object")
    try:
        validate_manifest(manifest)
    except ImportFailure as exc:
        raise VerificationError(f"invalid manifest: {exc}") from exc
    return manifest, data


def actual_files(root: Path) -> set[str]:
    files: set[str] = set()
    for path in root.rglob("*"):
        if path.is_symlink():
            raise VerificationError(f"symlink is not allowed in vendor snapshot: {path}")
        if path.is_file():
            files.add(path.relative_to(root).as_posix())
    return files


def verify_identity(manifest: dict[str, Any]) -> None:
    upstream = manifest["upstream"]
    if normalize_url(upstream.get("url", "")) != EXPECTED_URL:
        raise VerificationError("manifest upstream URL is not the approved NP2kai source")
    if upstream.get("commit") != EXPECTED_COMMIT:
        raise VerificationError("manifest does not pin the approved NP2kai commit")
    if upstream.get("project_version") != EXPECTED_VERSION:
        raise VerificationError("manifest project version changed unexpectedly")
    baseline = manifest["baseline"]
    expected = {
        "cpu": "i286",
        "frontend": "none",
        "guest_video_core": "included",
        "host_display": "none",
        "guest_sound_state": "source-inspection-selected-minimum",
        "sound_generation": "disabled",
        "host_audio": "none",
        "network": False,
        "threading": "minimum-single-thread",
    }
    if baseline != expected:
        raise VerificationError("manifest baseline does not match the approved Step 3 baseline")


def verify_license_evidence(manifest: dict[str, Any]) -> None:
    by_origin = {entry["upstream_path"]: entry for entry in manifest["files"]}
    for entry in manifest["files"]:
        evidence = entry["license_evidence"]
        if evidence["review_status"] != "reviewed":
            raise VerificationError(
                f"license evidence is not reviewed: {entry['upstream_path']}"
            )
        if not evidence["documents"]:
            raise VerificationError(f"license evidence has no document: {entry['upstream_path']}")
        for document in evidence["documents"]:
            referenced = by_origin.get(document)
            if referenced is None or referenced["role"] != "license":
                raise VerificationError(
                    f"license evidence document is not an imported license: {document}"
                )


def verify_hashes(root: Path, manifest: dict[str, Any]) -> None:
    path = root / "SHA256SUMS"
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as exc:
        raise VerificationError(f"cannot read SHA256SUMS: {exc}") from exc
    expected = {entry["destination_path"] for entry in manifest["files"]}
    seen: dict[str, str] = {}
    for line in lines:
        match = HASH_LINE.fullmatch(line)
        if match is None:
            raise VerificationError(f"invalid SHA256SUMS line: {line!r}")
        digest, destination = match.groups()
        if destination in seen:
            raise VerificationError(f"duplicate SHA256SUMS entry: {destination}")
        seen[destination] = digest
    if set(seen) != expected:
        raise VerificationError("SHA256SUMS does not cover exactly the manifest destinations")
    for destination, expected_digest in seen.items():
        digest = hashlib.sha256((root / destination).read_bytes()).hexdigest()
        if digest != expected_digest:
            raise VerificationError(f"SHA256SUMS mismatch: {destination}")


def verify_generated_metadata(root: Path, manifest: dict[str, Any]) -> None:
    blobs = {
        entry["destination_path"]: (root / entry["destination_path"]).read_bytes()
        for entry in manifest["files"]
    }
    expected_readme = render_readme(manifest).encode("utf-8")
    expected_map = render_license_map(manifest, blobs).encode("utf-8")
    expected_hashes = render_hashes(manifest, blobs).encode("utf-8")
    for name, expected in {
        "README.md": expected_readme,
        "LICENSE-MAP.md": expected_map,
        "SHA256SUMS": expected_hashes,
    }.items():
        if (root / name).read_bytes() != expected:
            raise VerificationError(f"generated metadata is stale or modified: {name}")


def verify_snapshot(root: Path, source: Path | None) -> None:
    if not root.is_dir():
        raise VerificationError(f"vendor root is not a directory: {root}")
    manifest, _manifest_bytes = load_manifest(root)
    verify_identity(manifest)
    verify_license_evidence(manifest)

    expected = (
        {entry["destination_path"] for entry in manifest["files"]}
        | {"import-manifest.json"}
        | EXPECTED_GENERATED
    )
    actual = actual_files(root)
    if actual != expected:
        missing = sorted(expected - actual)
        unexpected = sorted(actual - expected)
        raise VerificationError(
            f"snapshot file set mismatch; missing={missing}, unexpected={unexpected}"
        )
    for entry in manifest["files"]:
        path = root / entry["destination_path"]
        mode = path.stat().st_mode
        if not stat.S_ISREG(mode):
            raise VerificationError(f"snapshot entry is not a regular file: {path}")
    verify_hashes(root, manifest)
    verify_generated_metadata(root, manifest)

    if source is not None:
        try:
            validate_source(source, manifest)
        except ImportFailure as exc:
            raise VerificationError(f"source validation failed: {exc}") from exc
        commit = manifest["upstream"]["commit"]
        for entry in manifest["files"]:
            destination = root / entry["destination_path"]
            expected_bytes = blob_bytes(source, commit, entry["upstream_path"])
            if destination.read_bytes() != expected_bytes:
                raise VerificationError(
                    f"snapshot bytes differ from pinned Git blob: {entry['upstream_path']}"
                )
            expected_mode = tree_mode(source, commit, entry["upstream_path"])
            actual_mode = destination.stat().st_mode & 0o777
            if actual_mode != expected_mode:
                raise VerificationError(
                    f"snapshot mode differs from pinned Git blob: {entry['upstream_path']}"
                )
    print(f"verified {len(manifest['files'])} allowlisted files in {root}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    repo_root = Path(__file__).resolve().parents[2]
    parser.add_argument(
        "--vendor-root",
        type=Path,
        default=repo_root / "third_party/np2kai",
        help="imported vendor directory",
    )
    parser.add_argument(
        "--source",
        type=Path,
        help="optional local Git checkout used for pinned blob comparison",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        verify_snapshot(args.vendor_root.resolve(), args.source.resolve() if args.source else None)
    except VerificationError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
