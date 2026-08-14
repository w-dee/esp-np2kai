#!/usr/bin/env python3
"""Import an allowlisted NP2kai snapshot from pinned Git objects.

The tool is intentionally offline: it never fetches, checks out, initializes
submodules, or copies from a working tree.
"""

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
ALLOWED_ROLES = {"source", "header", "include-fragment", "resource-source", "license"}
ALLOWED_GROUPS = {"i286", "shared-core", "deferred-not-built"}
ALLOWED_MAPPING = {
    "np2-default",
    "component-specific",
    "file-header",
    "explicit-root-mit",
    "needs-review",
}
ALLOWED_STATUS = {"reviewed", "needs-review"}
EXPECTED_GENERATED = {"README.md", "LICENSE-MAP.md", "SHA256SUMS"}
EXPECTED_UPSTREAM_URL = "https://github.com/AZO234/NP2kai"
NOTICE_RE = re.compile(rb"(?i)(copyright|license|permission|all rights reserved)")


class ImportError(RuntimeError):
    pass


def git(source: Path, *args: str) -> bytes:
    proc = subprocess.run(
        ["git", "-C", str(source), *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if proc.returncode:
        detail = proc.stderr.decode("utf-8", "replace").strip()
        raise ImportError(f"git {' '.join(args)} failed: {detail}")
    return proc.stdout


def normalize_url(value: str) -> str:
    value = value.strip()
    if value.endswith(".git"):
        value = value[:-4]
    return value.rstrip("/")


def safe_relative(value: str) -> Path:
    if not isinstance(value, str):
        raise ImportError(f"repository-relative path must be a string: {value!r}")
    if not value or "\x00" in value:
        raise ImportError("empty or NUL-containing path")
    path = Path(value)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        raise ImportError(f"unsafe repository-relative path: {value!r}")
    if "\\" in value:
        raise ImportError(f"backslash in path: {value!r}")
    if path.as_posix() != value:
        raise ImportError(f"non-canonical repository-relative path: {value!r}")
    return path


def validate_manifest(manifest: dict[str, Any]) -> None:
    if manifest.get("schema_version") != SCHEMA_VERSION:
        raise ImportError("unsupported manifest schema_version")
    upstream = manifest.get("upstream")
    if not isinstance(upstream, dict):
        raise ImportError("manifest upstream must be an object")
    for key in ("url", "commit", "project_version"):
        if not isinstance(upstream.get(key), str) or not upstream[key]:
            raise ImportError(f"missing upstream.{key}")
    commit = upstream["commit"]
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ImportError("upstream.commit must be a 40-character lowercase SHA")
    baseline = manifest.get("baseline")
    if not isinstance(baseline, dict):
        raise ImportError("manifest baseline must be an object")
    forbidden_prefixes = manifest.get("forbidden_prefixes")
    forbidden_extensions = manifest.get("forbidden_extensions")
    if not isinstance(forbidden_prefixes, list) or not all(
        isinstance(value, str) and value for value in forbidden_prefixes
    ):
        raise ImportError("forbidden_prefixes must be a non-empty string list")
    if not isinstance(forbidden_extensions, list) or not all(
        isinstance(value, str) and value.startswith(".")
        for value in forbidden_extensions
    ):
        raise ImportError("forbidden_extensions must be a dot-prefixed string list")
    generated_files = manifest.get("generated_files")
    if generated_files != sorted(EXPECTED_GENERATED):
        raise ImportError("generated_files must be exactly the sorted generated metadata set")
    files = manifest.get("files")
    if not isinstance(files, list) or not files:
        raise ImportError("manifest files must be a non-empty list")

    origins: set[str] = set()
    destinations: set[str] = set()
    forbidden_ext = {str(value).lower() for value in forbidden_extensions}
    for entry in files:
        if not isinstance(entry, dict):
            raise ImportError("manifest file entry must be an object")
        required = {"upstream_path", "destination_path", "role", "build_group", "license_evidence"}
        if set(entry) != required:
            raise ImportError("manifest file entry has unexpected or missing keys")
        origin = safe_relative(entry["upstream_path"])
        destination = safe_relative(entry["destination_path"])
        origin_text = origin.as_posix()
        destination_text = destination.as_posix()
        if origin_text in origins or destination_text in destinations:
            raise ImportError("duplicate manifest origin or destination")
        origins.add(origin_text)
        destinations.add(destination_text)
        if not isinstance(entry["role"], str) or entry["role"] not in ALLOWED_ROLES:
            raise ImportError(f"unsupported role: {entry['role']!r}")
        if (
            not isinstance(entry["build_group"], str)
            or entry["build_group"] not in ALLOWED_GROUPS
        ):
            raise ImportError(f"unsupported build_group: {entry['build_group']!r}")
        if entry["role"] == "license" and destination_text.startswith("src/"):
            raise ImportError("license entry cannot be under src/")
        if entry["role"] != "license" and not destination_text.startswith("src/"):
            raise ImportError("non-license entry must be under src/")
        if origin_text.startswith("LICENSES/") and entry["role"] != "license":
            raise ImportError("license-directory source must be a license entry")
        evidence = entry["license_evidence"]
        if not isinstance(evidence, dict):
            raise ImportError("license_evidence must be an object")
        if set(evidence) != {"upstream_mapping", "documents", "source_notice", "review_status"}:
            raise ImportError("license_evidence has unexpected or missing keys")
        if (
            not isinstance(evidence["upstream_mapping"], str)
            or evidence["upstream_mapping"] not in ALLOWED_MAPPING
        ):
            raise ImportError("unsupported license evidence mapping")
        if not isinstance(evidence["documents"], list) or not evidence["documents"] or not all(
            isinstance(value, str) and value for value in evidence["documents"]
        ):
            raise ImportError("license evidence documents must be a non-empty string list")
        for document in evidence["documents"]:
            safe_relative(document)
        if evidence["source_notice"] != "preserved-unmodified":
            raise ImportError("source_notice must require preserved-unmodified bytes")
        if (
            not isinstance(evidence["review_status"], str)
            or evidence["review_status"] not in ALLOWED_STATUS
        ):
            raise ImportError("unsupported license evidence review status")
        if evidence["review_status"] != "reviewed":
            raise ImportError("needs-review license evidence cannot be imported")
        for prefix in forbidden_prefixes:
            if origin_text.startswith(prefix):
                raise ImportError(f"forbidden upstream path in manifest: {origin_text}")
        if origin.suffix.lower() in forbidden_ext:
            raise ImportError(f"forbidden upstream extension in manifest: {origin_text}")

    if not any(entry["role"] == "license" and entry["upstream_path"] == "LICENSE" for entry in files):
        raise ImportError("manifest must include upstream LICENSE")
    for entry in files:
        if entry["license_evidence"]["upstream_mapping"] == "needs-review":
            raise ImportError("needs-review license evidence cannot be imported")


def validate_license_documents(manifest: dict[str, Any]) -> None:
    by_origin = {entry["upstream_path"]: entry for entry in manifest["files"]}
    for entry in manifest["files"]:
        evidence = entry["license_evidence"]
        if evidence["review_status"] != "reviewed":
            raise ImportError(
                f"license evidence is not reviewed: {entry['upstream_path']}"
            )
        for document in evidence["documents"]:
            referenced = by_origin.get(document)
            if referenced is None or referenced["role"] != "license":
                raise ImportError(
                    f"license evidence document is not an imported license: {document}"
                )


def tree_mode(source: Path, commit: str, path: str) -> int:
    output = git(source, "ls-tree", "-z", commit, "--", path)
    records = output.split(b"\0")
    record = next((record for record in records if record), None)
    if record is None:
        raise ImportError(f"pinned commit does not contain {path}")
    metadata, _, _name = record.partition(b"\t")
    mode = metadata.split(b" ", 1)[0].decode("ascii", "strict")
    if mode not in {"100644", "100755"}:
        raise ImportError(f"{path} is not a regular file (mode {mode})")
    return int(mode[-3:], 8)


def blob_bytes(source: Path, commit: str, path: str) -> bytes:
    tree_mode(source, commit, path)
    return git(source, "show", f"{commit}:{path}")


def validate_source(source: Path, manifest: dict[str, Any]) -> None:
    if not source.is_dir():
        raise ImportError(f"source is not a directory: {source}")
    expected_url = normalize_url(manifest["upstream"]["url"])
    actual_url = normalize_url(
        git(source, "remote", "get-url", "origin").decode("utf-8", "replace")
    )
    if actual_url != expected_url:
        raise ImportError(f"origin URL mismatch: expected {expected_url}, got {actual_url}")
    commit = manifest["upstream"]["commit"]
    actual_commit = git(source, "rev-parse", "--verify", f"{commit}^{{commit}}").decode().strip()
    if actual_commit != commit:
        raise ImportError(f"pinned commit mismatch: expected {commit}, got {actual_commit}")
    for entry in manifest["files"]:
        blob_bytes(source, commit, entry["upstream_path"])


def notice_marker(data: bytes) -> str:
    return "present" if NOTICE_RE.search(data) else "none-observed"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def render_readme(manifest: dict[str, Any]) -> str:
    upstream = manifest["upstream"]
    baseline = manifest["baseline"]
    source_count = sum(entry["role"] != "license" for entry in manifest["files"])
    license_count = sum(entry["role"] == "license" for entry in manifest["files"])
    return (
        "# Vendored NP2kai snapshot\n\n"
        "This directory is generated by tools/np2kai/import_np2kai.py from the\n"
        "explicit import-manifest.json allowlist. It is a fixed, byte-preserved\n"
        "snapshot; it is not an upstream working-tree copy or Git submodule.\n\n"
        "## Upstream identity\n\n"
        f"- URL: {upstream['url']}\n"
        f"- Commit: {upstream['commit']}\n"
        f"- Observed default branch: {upstream.get('observed_default_branch', '')} (informational only)\n"
        f"- Project version: {upstream['project_version']}\n"
        f"- Commit author/date: {upstream.get('commit_author', '')} / {upstream.get('commit_date', '')}\n"
        f"- Subject: {upstream.get('subject', '')}\n\n"
        "## Import policy\n\n"
        "- Import command: `python3 tools/np2kai/import_np2kai.py --source <local-checkout>`\n"
        f"- Imported non-license entries: {source_count}\n"
        f"- Preserved upstream license/notice entries: {license_count}\n"
        "- All imported upstream bytes are obtained from the pinned Git commit.\n"
        "- Local modifications to vendored upstream source: none.\n"
        "- The manifest records upstream license/notice evidence and mapping; it does not\n"
        "  independently reclassify or provide legal advice about any file.\n"
        "- Host contracts and project adapters are outside this vendor snapshot.\n"
        f"- Excluded major component prefixes: {', '.join(manifest.get('forbidden_prefixes', []))}\n\n"
        "## Baseline intent\n\n"
        f"- CPU: {baseline.get('cpu', '')}\n"
        f"- Guest video core: {baseline.get('guest_video_core', '')}\n"
        f"- Host display backend: {baseline.get('host_display', '')}\n"
        f"- Guest sound state: {baseline.get('guest_sound_state', '')}\n"
        f"- Sound generation: {baseline.get('sound_generation', '')}\n"
        f"- Host audio backend: {baseline.get('host_audio', '')}\n"
        f"- Frontend: {baseline.get('frontend', '')}\n"
        f"- Threading: {baseline.get('threading', '')}\n\n"
        "Use SHA256SUMS for the imported byte set and LICENSE-MAP.md for the\n"
        "file-level upstream evidence inventory. Step 4 is the first compile/link/run\n"
        "milestone; this snapshot alone does not claim a complete dependency closure.\n"
    )


def render_license_map(manifest: dict[str, Any], blobs: dict[str, bytes]) -> str:
    lines = [
        "# NP2kai license and notice evidence map",
        "",
        "This is an evidence inventory derived from the manifest and preserved upstream",
        "bytes. It records upstream document mappings and observed notice markers; it is",
        "not an independent license classification or legal opinion.",
        "",
        "| Destination | Upstream path | Evidence mapping | Documents | Notice markers | Review |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for entry in manifest["files"]:
        destination = entry["destination_path"]
        data = blobs[destination]
        evidence = entry["license_evidence"]
        documents = ", ".join(value for value in evidence["documents"])
        lines.append(
            f"| {destination} | {entry['upstream_path']} | "
            f"{evidence['upstream_mapping']} | {documents} | "
            f"{notice_marker(data)}; preserved byte-for-byte | "
            f"{evidence['review_status']} |"
        )
    lines.extend(
        [
            "",
            "A needs-review mapping or missing evidence document is not acceptable for",
            "a completed Step 3 import. Source-file notices are retained in the copied",
            "bytes; the marker column is only an audit aid.",
            "",
        ]
    )
    return "\n".join(lines)


def render_hashes(manifest: dict[str, Any], blobs: dict[str, bytes]) -> str:
    lines = [
        f"{sha256(blobs[entry['destination_path']])}  {entry['destination_path']}"
        for entry in manifest["files"]
    ]
    return "\n".join(sorted(lines)) + "\n"


def write_file(root: Path, relative: str, data: bytes, mode: int = 0o644) -> None:
    path = root / safe_relative(relative)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    os.chmod(path, mode)


def path_exists(path: Path) -> bool:
    return os.path.lexists(os.fspath(path))


def absolute_path(path: Path) -> Path:
    return Path(os.path.abspath(os.fspath(path)))


def reject_symlink_components(path: Path) -> None:
    path = absolute_path(path)
    current = Path(path.anchor)
    for component in path.parts[1:]:
        current /= component
        if current.is_symlink():
            raise ImportError(f"symlink path component is not allowed: {current}")


def snapshot_files(root: Path) -> set[str]:
    if root.is_symlink() or not root.is_dir():
        raise ImportError(f"snapshot root is not a real directory: {root}")
    files: set[str] = set()
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ImportError(f"symlink is not allowed in snapshot: {path}")
        if path.is_file():
            files.add(path.relative_to(root).as_posix())
    return files


def expected_snapshot_files(manifest: dict[str, Any]) -> set[str]:
    return (
        {entry["destination_path"] for entry in manifest["files"]}
        | {"import-manifest.json"}
        | EXPECTED_GENERATED
    )


def validate_snapshot_tree(
    root: Path,
    manifest: dict[str, Any],
    manifest_bytes: bytes | None = None,
) -> None:
    actual = snapshot_files(root)
    expected = expected_snapshot_files(manifest)
    if actual != expected:
        missing = sorted(expected - actual)
        unexpected = sorted(actual - expected)
        raise ImportError(
            f"snapshot file set mismatch; missing={missing}, unexpected={unexpected}"
        )

    manifest_path = root / "import-manifest.json"
    if manifest_bytes is not None and manifest_path.read_bytes() != manifest_bytes:
        raise ImportError("snapshot manifest bytes do not match the requested manifest")

    blobs = {
        entry["destination_path"]: (root / entry["destination_path"]).read_bytes()
        for entry in manifest["files"]
    }
    hash_lines = (root / "SHA256SUMS").read_text(encoding="utf-8").splitlines()
    expected_hashes = {
        f"{sha256(blobs[entry['destination_path']])}  {entry['destination_path']}"
        for entry in manifest["files"]
    }
    if set(hash_lines) != expected_hashes or len(hash_lines) != len(expected_hashes):
        raise ImportError("SHA256SUMS does not match the manifest byte set")

    generated = {
        "README.md": render_readme(manifest).encode("utf-8"),
        "LICENSE-MAP.md": render_license_map(manifest, blobs).encode("utf-8"),
        "SHA256SUMS": render_hashes(manifest, blobs).encode("utf-8"),
    }
    for name, expected_bytes in generated.items():
        if (root / name).read_bytes() != expected_bytes:
            raise ImportError(f"generated metadata is stale or modified: {name}")


def read_manifest(path: Path) -> tuple[dict[str, Any], bytes]:
    try:
        manifest_bytes = path.read_bytes()
        manifest = json.loads(manifest_bytes.decode("utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ImportError(f"invalid UTF-8 JSON manifest: {exc}") from exc
    if not isinstance(manifest, dict):
        raise ImportError("manifest root must be an object")
    validate_manifest(manifest)
    validate_license_documents(manifest)
    return manifest, manifest_bytes


def validate_managed_snapshot(root: Path) -> dict[str, Any]:
    if root.is_symlink() or not root.is_dir():
        raise ImportError(f"replacement target is not a real directory: {root}")
    manifest, manifest_bytes = read_manifest(root / "import-manifest.json")
    if normalize_url(manifest["upstream"]["url"]) != EXPECTED_UPSTREAM_URL:
        raise ImportError("replacement target is not an NP2kai importer snapshot")
    validate_snapshot_tree(root, manifest, manifest_bytes)
    return manifest


def reserve_sibling(parent: Path, prefix: str) -> Path:
    fd, name = tempfile.mkstemp(prefix=prefix, dir=os.fspath(parent))
    os.close(fd)
    path = Path(name)
    path.unlink()
    return path


def import_snapshot(
    source: Path,
    manifest_path: Path,
    output_root: Path,
    replace: bool,
) -> None:
    manifest_path = absolute_path(manifest_path)
    output_root = absolute_path(output_root)
    reject_symlink_components(output_root.parent)
    if path_exists(output_root) and output_root.is_symlink():
        raise ImportError(f"output root may not be a symlink: {output_root}")
    output_root.parent.mkdir(parents=True, exist_ok=True)
    manifest, manifest_bytes = read_manifest(manifest_path)
    validate_source(source, manifest)

    stage_path = Path(tempfile.mkdtemp(prefix=".np2kai-import-", dir=str(output_root.parent)))
    backup: Path | None = None
    try:
        blobs: dict[str, bytes] = {}
        modes: dict[str, int] = {}
        for entry in manifest["files"]:
            data = blob_bytes(source, manifest["upstream"]["commit"], entry["upstream_path"])
            destination = entry["destination_path"]
            blobs[destination] = data
            modes[destination] = tree_mode(source, manifest["upstream"]["commit"], entry["upstream_path"])
            write_file(stage_path, destination, data, modes[destination])
        write_file(stage_path, "import-manifest.json", manifest_bytes)
        write_file(stage_path, "README.md", render_readme(manifest).encode("utf-8"))
        write_file(stage_path, "LICENSE-MAP.md", render_license_map(manifest, blobs).encode("utf-8"))
        write_file(stage_path, "SHA256SUMS", render_hashes(manifest, blobs).encode("utf-8"))
        validate_snapshot_tree(stage_path, manifest, manifest_bytes)

        if path_exists(output_root):
            if not replace:
                raise ImportError(
                    f"output exists with generated or unexpected files: {output_root}; "
                    "use --replace only for a validated importer snapshot"
                )
            validate_managed_snapshot(output_root)
            backup = reserve_sibling(output_root.parent, ".np2kai-backup-")
            os.replace(output_root, backup)
            try:
                os.replace(stage_path, output_root)
            except Exception:
                if not path_exists(output_root):
                    os.replace(backup, output_root)
                    backup = None
                raise
            shutil.rmtree(backup)
            backup = None
        else:
            os.replace(stage_path, output_root)
    except Exception:
        if path_exists(stage_path):
            shutil.rmtree(stage_path, ignore_errors=True)
        if backup is not None and path_exists(backup) and not path_exists(output_root):
            try:
                os.replace(backup, output_root)
            except OSError:
                pass
        raise
    print(f"imported {len(manifest['files'])} allowlisted files into {output_root}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path, help="local Git checkout of the upstream repository")
    parser.add_argument("--manifest", type=Path, help="manifest JSON (defaults to repo/third_party/np2kai/import-manifest.json)")
    parser.add_argument("--output-root", type=Path, help="output directory (defaults to manifest directory)")
    parser.add_argument(
        "--replace",
        action="store_true",
        help="replace an existing validated NP2kai importer snapshot transactionally",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = Path(__file__).resolve().parents[2]
    manifest_path = (args.manifest or (repo_root / "third_party/np2kai/import-manifest.json")).resolve()
    output_root = args.output_root or manifest_path.parent
    try:
        import_snapshot(args.source.resolve(), manifest_path, output_root, args.replace)
    except (ImportError, OSError) as exc:
        print(f"error: {exc}", file=os.sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
