#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Snapshot and compare a repository state without modifying the worktree."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import stat
import subprocess
import sys
from pathlib import Path
from typing import Any


def _git(root: Path, *arguments: str) -> bytes:
    completed = subprocess.run(
        ["git", *arguments],
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        message = completed.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"git {' '.join(arguments)} failed: {message}")
    return completed.stdout


def _b64(value: bytes) -> str:
    return base64.b64encode(value).decode("ascii")


def _hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _untracked_entry(root: Path, raw_path: bytes) -> dict[str, Any]:
    path = root / os.fsdecode(raw_path)
    try:
        info = path.lstat()
    except OSError as exc:
        raise RuntimeError(f"cannot stat untracked path {path}: {exc}") from exc
    mode = stat.S_IMODE(info.st_mode)
    entry: dict[str, Any] = {"mode": mode, "path": _b64(raw_path)}
    if stat.S_ISREG(info.st_mode):
        entry["kind"] = "file"
        entry["sha256"] = _hash_file(path)
    elif stat.S_ISLNK(info.st_mode):
        entry["kind"] = "symlink"
        entry["target_sha256"] = hashlib.sha256(os.fsencode(os.readlink(path))).hexdigest()
    else:
        entry["kind"] = "other"
    return entry


def snapshot(root: Path) -> dict[str, Any]:
    untracked = _git(root, "ls-files", "--others", "--exclude-standard", "-z")
    paths = sorted(path for path in untracked.split(b"\0") if path)
    entries = [_untracked_entry(root, path) for path in paths]
    return {
        "version": 1,
        "diff_worktree": _b64(_git(root, "diff", "--binary", "--no-ext-diff", "--no-color")),
        "diff_cached": _b64(_git(root, "diff", "--cached", "--binary", "--no-ext-diff", "--no-color")),
        "status": _b64(_git(root, "status", "--porcelain=v2", "--branch", "--untracked-files=all")),
        "untracked": entries,
    }


def _write_snapshot(root: Path, destination: Path) -> None:
    state = snapshot(root)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(state, sort_keys=True, separators=(",", ":")) + "\n", encoding="ascii")
    print(f"worktree state snapshot written: {destination}")


def _path_label(encoded: str) -> str:
    try:
        return os.fsdecode(base64.b64decode(encoded.encode("ascii")))
    except (ValueError, UnicodeError):
        return "<unrepresentable path>"


def _difference(before: dict[str, Any], after: dict[str, Any]) -> list[str]:
    differences: list[str] = []
    for key in ("version", "diff_worktree", "diff_cached", "status"):
        if before.get(key) != after.get(key):
            differences.append(key)
    before_entries = {entry["path"]: entry for entry in before.get("untracked", [])}
    after_entries = {entry["path"]: entry for entry in after.get("untracked", [])}
    for path in sorted(set(before_entries) | set(after_entries)):
        if before_entries.get(path) != after_entries.get(path):
            differences.append(f"untracked:{_path_label(path)}")
    return differences


def _read_snapshot(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="ascii"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"cannot read snapshot {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise RuntimeError(f"snapshot {path} is not an object")
    return value


def compare(before_path: Path, after_path: Path) -> int:
    try:
        before = _read_snapshot(before_path)
        after = _read_snapshot(after_path)
    except RuntimeError as exc:
        print(f"error: SOURCE_TREE_STATE_ERROR: {exc}", file=sys.stderr)
        return 2
    differences = _difference(before, after)
    if differences:
        print("error: SOURCE_TREE_STATE_CHANGED: validation modified the source tree", file=sys.stderr)
        for difference in differences:
            print(f"  changed: {difference}", file=sys.stderr)
        return 1
    print("worktree state unchanged")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--snapshot", type=Path)
    group.add_argument("--compare", nargs=2, metavar=("BEFORE", "AFTER"), type=Path)
    args = parser.parse_args(argv)
    root = args.repo.resolve()
    try:
        if args.snapshot is not None:
            _write_snapshot(root, args.snapshot)
            return 0
        assert args.compare is not None
        return compare(args.compare[0], args.compare[1])
    except (OSError, RuntimeError) as exc:
        print(f"error: SOURCE_TREE_STATE_ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
