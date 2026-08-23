#!/usr/bin/env python3
"""Prepare deterministic, ignored Step 4 source derivatives."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from typing import Any


SCHEMA_VERSION = 1
HASH_RE = re.compile(r"[0-9a-f]{64}\Z")
GENERATED_NAMES = {
    "host-overlay",
    "patched-src",
    "patch-report.json",
    "source-map.json",
}


class PreparationError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise PreparationError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        fail(f"cannot read {label}: {exc}")
    if not isinstance(value, dict):
        fail(f"{label} must contain a JSON object")
    return value


def safe_relative(value: Any, label: str) -> Path:
    if not isinstance(value, str) or not value or "\x00" in value:
        fail(f"unsafe {label}: {value!r}")
    if "\\" in value:
        fail(f"backslash in {label}: {value!r}")
    path = Path(value)
    if path.is_absolute() or path.as_posix() != value:
        fail(f"unsafe {label}: {value!r}")
    if not path.parts or any(part in {"", ".", ".."} for part in path.parts):
        fail(f"unsafe {label}: {value!r}")
    return path


def reject_symlink_components(root: Path, candidate: Path, label: str) -> None:
    root = root.resolve()
    candidate = candidate.absolute()
    try:
        relative = candidate.relative_to(root)
    except ValueError:
        fail(f"{label} escapes its managed root")
    current = root
    if current.is_symlink():
        fail(f"unexpected symlink in {label} root: {root}")
    for part in relative.parts:
        current /= part
        if current.is_symlink():
            fail(f"unexpected symlink in {label}: {current}")


def reject_symlink_path(path: Path, label: str) -> None:
    absolute = path.absolute()
    current = Path(absolute.anchor)
    for part in absolute.parts[1:]:
        current /= part
        if current.is_symlink():
            fail(f"unexpected symlink in {label}: {current}")


def path_under(root: Path, relative: Path, label: str) -> Path:
    candidate = root / relative
    reject_symlink_components(root, candidate, label)
    resolved_root = root.resolve()
    resolved = candidate.resolve(strict=False)
    try:
        resolved.relative_to(resolved_root)
    except ValueError:
        fail(f"{label} escapes its managed root")
    return candidate


def validate_hash(value: Any, label: str) -> str:
    if not isinstance(value, str) or HASH_RE.fullmatch(value) is None:
        fail(f"{label} must be a lowercase SHA-256 hex string")
    return value


def load_inputs(
    vendor_root: Path, manifest_path: Path, patch_set_path: Path
) -> tuple[str, list[dict[str, Any]], Path]:
    if vendor_root.is_symlink() or not vendor_root.is_dir():
        fail(f"vendor root is not a real directory: {vendor_root}")
    manifest = read_json(manifest_path, "import manifest")
    patch_set = read_json(patch_set_path, "patch set")
    if patch_set.get("schema_version") != SCHEMA_VERSION:
        fail("unsupported patch-set schema_version")
    commit = patch_set.get("upstream_commit")
    manifest_upstream = manifest.get("upstream")
    manifest_commit = manifest_upstream.get("commit") if isinstance(manifest_upstream, dict) else None
    if not isinstance(commit, str) or commit != manifest_commit:
        fail("patch-set upstream_commit differs from import-manifest upstream.commit")
    entries = patch_set.get("entries")
    if not isinstance(entries, list):
        fail("patch-set entries must be a list")
    files = manifest.get("files")
    if not isinstance(files, list):
        fail("import manifest files must be a list")
    allowlisted: dict[str, dict[str, Any]] = {}
    for item in files:
        if not isinstance(item, dict):
            fail("import manifest file entry must be an object")
        destination = item.get("destination_path")
        if isinstance(destination, str):
            allowlisted[destination] = item

    patch_root = patch_set_path.parent.resolve()
    if patch_set_path.is_symlink() or not patch_set_path.is_file():
        fail(f"patch-set path is not a real file: {patch_set_path}")
    normalized: list[dict[str, Any]] = []
    seen_logical: set[str] = set()
    for index, item in enumerate(entries):
        if not isinstance(item, dict):
            fail(f"patch-set entry {index} must be an object")
        required = {
            "logical_source",
            "pristine_path",
            "pristine_sha256",
            "patch_path",
            "patch_sha256",
            "patched_sha256",
        }
        if set(item) != required:
            fail(f"patch-set entry {index} has unexpected or missing keys")
        logical = safe_relative(item["logical_source"], f"entry {index} logical_source")
        pristine = safe_relative(item["pristine_path"], f"entry {index} pristine_path")
        patch_relative = safe_relative(item["patch_path"], f"entry {index} patch_path")
        logical_text = logical.as_posix()
        pristine_text = pristine.as_posix()
        if logical_text in seen_logical:
            fail(f"duplicate logical source: {logical_text}")
        seen_logical.add(logical_text)
        if logical_text != pristine_text:
            fail("logical_source and pristine_path must identify the same source")
        destination = f"src/{pristine_text}"
        imported = allowlisted.get(destination)
        if imported is None or imported.get("role") not in {"source", "header"}:
            fail(f"pristine source/header is not an allowlisted imported file: {pristine_text}")
        source = path_under(vendor_root, Path("src") / pristine, "pristine source")
        if source.is_symlink() or not source.is_file():
            fail(f"pristine source does not exist as a regular file: {source}")
        patch_path = path_under(patch_root, patch_relative, "patch file")
        if patch_path.is_symlink() or not patch_path.is_file():
            fail(f"patch file does not exist as a regular file: {patch_path}")
        normalized.append(
            {
                "logical_source": logical_text,
                "pristine_path": pristine_text,
                "pristine_sha256": validate_hash(item["pristine_sha256"], f"entry {index} pristine_sha256"),
                "patch_path": patch_relative.as_posix(),
                "patch_sha256": validate_hash(item["patch_sha256"], f"entry {index} patch_sha256"),
                "patched_sha256": validate_hash(item["patched_sha256"], f"entry {index} patched_sha256"),
            }
        )
    normalized.sort(key=lambda item: item["logical_source"])
    return commit, normalized, patch_root


def apply_entry(
    entry: dict[str, Any], vendor_root: Path, patch_root: Path, work_root: Path, patched_root: Path
) -> dict[str, str]:
    pristine = path_under(
        vendor_root, Path("src") / Path(entry["pristine_path"]), "pristine source"
    )
    patch_path = path_under(patch_root, Path(entry["patch_path"]), "patch file")
    actual_pristine = sha256_file(pristine)
    if actual_pristine != entry["pristine_sha256"]:
        fail(f"pristine SHA-256 mismatch for {entry['logical_source']}")
    actual_patch = sha256_file(patch_path)
    if actual_patch != entry["patch_sha256"]:
        fail(f"patch SHA-256 mismatch for {entry['logical_source']}")

    entry_root = work_root / entry["logical_source"]
    entry_root.parent.mkdir(parents=True, exist_ok=True)
    entry_root.mkdir()
    staged_source = entry_root / entry["pristine_path"]
    staged_source.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(pristine, staged_source)
    for check in (True, False):
        command = ["git", "apply", "--check", str(patch_path)] if check else ["git", "apply", str(patch_path)]
        completed = subprocess.run(
            command,
            cwd=entry_root,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        if completed.returncode:
            phase = "check" if check else "apply"
            fail(f"git apply {phase} failed for {entry['logical_source']}: {completed.stdout.strip()}")
    files = [path for path in entry_root.rglob("*") if path.is_file()]
    if len(files) != 1 or files[0] != staged_source:
        fail(f"patch changed files outside {entry['logical_source']}")
    actual_patched = sha256_file(staged_source)
    if actual_patched != entry["patched_sha256"]:
        fail(f"patched SHA-256 mismatch for {entry['logical_source']}")
    generated = patched_root / entry["logical_source"]
    generated.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(staged_source, generated)
    return {
        "logical_source": entry["logical_source"],
        "pristine_path": entry["pristine_path"],
        "pristine_sha256": entry["pristine_sha256"],
        "patch_path": entry["patch_path"],
        "patch_sha256": entry["patch_sha256"],
        "patched_sha256": entry["patched_sha256"],
        "generated_path": f"patched-src/{entry['logical_source']}",
    }


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n", encoding="utf-8")


def validate_existing_output(output_root: Path) -> None:
    if not output_root.exists():
        return
    if output_root.is_symlink() or not output_root.is_dir():
        fail(f"output root is not a real directory: {output_root}")
    names = {item.name for item in output_root.iterdir()}
    unexpected = names - GENERATED_NAMES
    if unexpected:
        fail(f"output root contains unexpected entries: {sorted(unexpected)}")
    for item in output_root.rglob("*"):
        if item.is_symlink():
            fail(f"output root contains unexpected symlink: {item}")


def install_atomically(stage: Path, output_root: Path) -> None:
    validate_existing_output(output_root)
    backup: Path | None = None
    try:
        if output_root.exists():
            fd, name = tempfile.mkstemp(prefix=".step4-sources-backup-", dir=str(output_root.parent))
            os.close(fd)
            Path(name).unlink()
            backup = Path(name)
            os.replace(output_root, backup)
        os.replace(stage, output_root)
        if backup is not None:
            shutil.rmtree(backup)
            backup = None
    except Exception:
        if output_root.exists() and output_root != stage:
            shutil.rmtree(output_root, ignore_errors=True)
        if backup is not None and backup.exists() and not output_root.exists():
            os.replace(backup, output_root)
        raise


def prepare_i286_host_overlay(
    vendor_root: Path, patched_root: Path, overlay_root: Path
) -> None:
    """Copy I286/V30 units beside the patched shared header for host probes."""

    vendor_i286 = path_under(vendor_root, Path("src/i286c"), "I286 source overlay")
    if vendor_i286.is_symlink() or not vendor_i286.is_dir():
        fail(f"I286 source overlay directory is not real: {vendor_i286}")
    overlay_i286 = overlay_root / "i286c"
    overlay_i286.mkdir(parents=True, exist_ok=True)
    for source in sorted(vendor_i286.iterdir()):
        if source.is_symlink() or not source.is_file():
            fail(f"I286 source overlay contains a non-regular file: {source}")
        patched_source = patched_root / "i286c" / source.name
        if patched_source.is_symlink():
            fail(f"patched I286 overlay contains an unexpected symlink: {patched_source}")
        if patched_source.exists():
            if not patched_source.is_file():
                fail(f"patched I286 overlay contains a non-regular file: {patched_source}")
            source = patched_source
        shutil.copyfile(source, overlay_i286 / source.name)
    patched_header = patched_root / "i286c/i286c.h"
    if patched_header.is_symlink() or not patched_header.is_file():
        fail("patched I286 header is missing from the host overlay")
    shutil.copyfile(patched_header, overlay_i286 / "i286c.h")


def prepare(args: argparse.Namespace) -> None:
    vendor_argument = args.vendor_root.absolute()
    manifest_argument = args.import_manifest.absolute()
    patch_set_argument = args.patch_set.absolute()
    reject_symlink_path(vendor_argument, "vendor root")
    reject_symlink_path(manifest_argument, "import manifest")
    reject_symlink_path(patch_set_argument, "patch set")
    vendor_root = vendor_argument.resolve()
    manifest_path = manifest_argument.resolve()
    patch_set_path = patch_set_argument.resolve()
    output_root = args.output_root.absolute()
    reject_symlink_path(output_root, "output root")
    output_root.parent.mkdir(parents=True, exist_ok=True)
    reject_symlink_components(output_root.parent, output_root, "output root")
    commit, entries, patch_root = load_inputs(vendor_root, manifest_path, patch_set_path)
    validate_existing_output(output_root)

    stage = Path(tempfile.mkdtemp(prefix=".step4-sources-", dir=str(output_root.parent)))
    work_root = Path(tempfile.mkdtemp(prefix=".step4-work-"))
    try:
        patched_root = stage / "patched-src"
        patched_root.mkdir()
        reports = []
        for entry in entries:
            reports.append(apply_entry(entry, vendor_root, patch_root, work_root, patched_root))
        patched_headers = {
            item["logical_source"]
            for item in reports
            if item["logical_source"].endswith(".h")
        }
        if "i286c/i286c.h" in patched_headers:
            prepare_i286_host_overlay(
                vendor_root,
                patched_root,
                stage / "host-overlay",
            )
        shutil.rmtree(work_root)
        work_root = None  # type: ignore[assignment]
        report = {"entries": reports, "schema_version": SCHEMA_VERSION, "upstream_commit": commit}
        source_map = {
            "entries": [
                {"generated_path": item["generated_path"], "logical_source": item["logical_source"]}
                for item in reports
                if not item["logical_source"].endswith(".h")
            ],
            "schema_version": SCHEMA_VERSION,
            "upstream_commit": commit,
        }
        write_json(stage / "patch-report.json", report)
        write_json(stage / "source-map.json", source_map)
        install_atomically(stage, output_root)
        stage = None  # type: ignore[assignment]
    finally:
        if work_root is not None and work_root.exists():
            shutil.rmtree(work_root, ignore_errors=True)
        if stage is not None and stage.exists():
            shutil.rmtree(stage, ignore_errors=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vendor-root", required=True, type=Path)
    parser.add_argument("--import-manifest", required=True, type=Path)
    parser.add_argument("--patch-set", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    try:
        prepare(parse_args())
    except (OSError, PreparationError) as exc:
        print(f"error: {exc}")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
