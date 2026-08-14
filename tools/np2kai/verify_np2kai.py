#!/usr/bin/env python3
"""Verify an imported NP2kai snapshot without network or firmware tools."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import tempfile
from typing import Any


TOOLS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR))

import import_np2kai as importer  # noqa: E402

from import_np2kai import (  # noqa: E402
    ImportError as ImportFailure,
    blob_bytes,
    import_snapshot,
    normalize_url,
    tree_mode,
    validate_license_documents,
    validate_manifest,
    validate_snapshot_tree,
    validate_source,
)


EXPECTED_URL = "https://github.com/AZO234/NP2kai"
EXPECTED_COMMIT = "e2dc9046aa5c786fcfbfb87e883457e421026e31"
EXPECTED_VERSION = "0.86.0.22"
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
    try:
        validate_license_documents(manifest)
    except ImportFailure as exc:
        raise VerificationError(str(exc)) from exc


def verify_snapshot(root: Path, source: Path | None) -> None:
    if not root.is_dir():
        raise VerificationError(f"vendor root is not a directory: {root}")
    manifest, _manifest_bytes = load_manifest(root)
    verify_identity(manifest)
    verify_license_evidence(manifest)
    try:
        validate_snapshot_tree(root, manifest, root.joinpath("import-manifest.json").read_bytes())
    except ImportFailure as exc:
        raise VerificationError(str(exc)) from exc

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


def snapshot_signature(root: Path) -> dict[str, tuple[bytes, int]]:
    signature: dict[str, tuple[bytes, int]] = {}
    for path in root.rglob("*"):
        if path.is_symlink():
            raise VerificationError(f"unexpected symlink in test snapshot: {path}")
        if path.is_file():
            signature[path.relative_to(root).as_posix()] = (
                path.read_bytes(),
                path.stat().st_mode & 0o777,
            )
    return signature


def run_replacement_safety_checks(root: Path, source: Path | None) -> None:
    if source is None:
        raise VerificationError("--replacement-safety requires --source")
    manifest_path = root / "import-manifest.json"
    with tempfile.TemporaryDirectory(prefix="np2kai-replace-tests-") as temp_name:
        parent = Path(temp_name)
        managed = parent / "managed-output"
        import_snapshot(source, manifest_path, managed, replace=False)

        unmanaged = parent / "unmanaged" / "np2kai"
        unmanaged.mkdir(parents=True)
        sentinel = unmanaged / "keep-me.txt"
        sentinel.write_bytes(b"unmanaged sentinel\n")
        try:
            import_snapshot(source, manifest_path, unmanaged, replace=True)
        except ImportFailure:
            pass
        else:
            raise VerificationError("--replace accepted an unmanaged basename-only target")
        if sentinel.read_bytes() != b"unmanaged sentinel\n":
            raise VerificationError("unmanaged target contents changed after rejection")
        print("replacement unmanaged-target rejection: ok")

        symlink_target = parent / "symlink-output"
        symlink_before = snapshot_signature(managed)
        symlink_target.symlink_to(managed, target_is_directory=True)
        try:
            import_snapshot(source, manifest_path, symlink_target, replace=True)
        except ImportFailure:
            pass
        else:
            raise VerificationError("--replace followed a symlink output target")
        if snapshot_signature(managed) != symlink_before:
            raise VerificationError("symlink rejection changed the managed snapshot")
        symlink_target.unlink()
        print("replacement symlink-target rejection: ok")

        before = snapshot_signature(managed)
        try:
            import_snapshot(parent / "missing-source", manifest_path, managed, replace=True)
        except ImportFailure:
            pass
        else:
            raise VerificationError("replacement unexpectedly accepted an invalid source")
        if snapshot_signature(managed) != before:
            raise VerificationError("failed replacement changed the managed snapshot")
        print("replacement failed-at-validation preservation: ok")

        before = snapshot_signature(managed)
        original_replace = importer.os.replace

        def fail_final_swap(source_path: str | bytes, destination_path: str | bytes) -> None:
            if Path(source_path).name.startswith(".np2kai-import-") and Path(
                destination_path
            ) == managed:
                raise OSError("forced final swap failure")
            original_replace(source_path, destination_path)

        importer.os.replace = fail_final_swap
        try:
            try:
                import_snapshot(source, manifest_path, managed, replace=True)
            except OSError:
                pass
            else:
                raise VerificationError("forced final swap failure was not observed")
        finally:
            importer.os.replace = original_replace
        if snapshot_signature(managed) != before:
            raise VerificationError("rollback after final swap failure changed the snapshot")
        verify_snapshot(managed, source)
        print("replacement rollback after move: ok")

        import_snapshot(source, manifest_path, managed, replace=True)
        verify_snapshot(managed, source)
        print("replacement managed-target success: ok")

        reference = parent / "reference-output"
        import_snapshot(source, manifest_path, reference, replace=False)
        if snapshot_signature(managed) != snapshot_signature(reference):
            raise VerificationError("successful replacement was not deterministic")
        leftovers = [
            path.name
            for path in parent.iterdir()
            if path.name.startswith(".np2kai-import-")
            or path.name.startswith(".np2kai-backup-")
        ]
        if leftovers:
            raise VerificationError(f"replacement left temporary artifacts: {leftovers}")
        print("replacement staging/backup cleanup: ok")


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
    parser.add_argument(
        "--replacement-safety",
        action="store_true",
        help="run temporary-directory --replace safety checks (requires --source)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        vendor_root = args.vendor_root.resolve()
        source = args.source.resolve() if args.source else None
        verify_snapshot(vendor_root, source)
        if args.replacement_safety:
            run_replacement_safety_checks(vendor_root, source)
    except (VerificationError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
