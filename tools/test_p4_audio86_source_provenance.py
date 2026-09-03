#!/usr/bin/env python3
"""Executable clean-source provenance tests for physical Audio 86 S1."""

from __future__ import annotations

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
RESOLVER = ROOT / "tools/emu/resolve-clean-source-git-sha.sh"


def git(repository: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return completed.stdout.strip()


def resolve(repository: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["bash", str(RESOLVER), str(repository)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def require_accepted(repository: Path, expected_sha: str, scenario: str) -> None:
    completed = resolve(repository)
    if completed.returncode != 0 or completed.stdout.strip() != expected_sha:
        raise AssertionError(
            f"{scenario}: clean provenance rejected or wrong SHA: {completed}"
        )


def require_rejected(repository: Path, reason: str, scenario: str) -> None:
    completed = resolve(repository)
    if completed.returncode == 0 or reason not in completed.stderr:
        raise AssertionError(
            f"{scenario}: dirty provenance was not rejected correctly: {completed}"
        )


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="p4-audio86-source-provenance-") as temp:
        repository = Path(temp) / "fixture"
        repository.mkdir()
        git(repository, "init", "--quiet")
        git(repository, "config", "user.name", "P4 Audio 86 Test")
        git(repository, "config", "user.email", "p4-audio86-test@example.invalid")
        (repository / ".gitignore").write_text("build/\n", encoding="utf-8")
        source = repository / "firmware-input.c"
        source.write_text("int fixture = 1;\n", encoding="utf-8")
        git(repository, "add", ".gitignore", "firmware-input.c")
        git(repository, "commit", "--quiet", "-m", "fixture")
        expected_sha = git(repository, "rev-parse", "HEAD")

        require_accepted(repository, expected_sha, "clean exact HEAD")

        source.write_text("int fixture = 2;\n", encoding="utf-8")
        require_rejected(repository, "unstaged tracked", "tracked modification")
        git(repository, "restore", "--worktree", "firmware-input.c")

        source.write_text("int fixture = 3;\n", encoding="utf-8")
        git(repository, "add", "firmware-input.c")
        require_rejected(repository, "staged tracked", "staged modification")
        git(repository, "restore", "--staged", "--worktree", "firmware-input.c")

        git(repository, "update-index", "--assume-unchanged", "firmware-input.c")
        source.write_text("int fixture = 4;\n", encoding="utf-8")
        require_rejected(repository, "hidden index visibility", "assume-unchanged")
        git(repository, "update-index", "--no-assume-unchanged", "firmware-input.c")
        git(repository, "restore", "--worktree", "firmware-input.c")

        git(repository, "update-index", "--skip-worktree", "firmware-input.c")
        source.write_text("int fixture = 5;\n", encoding="utf-8")
        require_rejected(repository, "hidden index visibility", "skip-worktree")
        git(repository, "update-index", "--no-skip-worktree", "firmware-input.c")
        git(repository, "restore", "--worktree", "firmware-input.c")

        untracked = repository / "new-build-input.c"
        untracked.write_text("int untracked = 1;\n", encoding="utf-8")
        require_rejected(repository, "non-ignored untracked", "untracked input")
        untracked.unlink()

        ignored = repository / "build" / "artifact.bin"
        ignored.parent.mkdir()
        ignored.write_bytes(b"ignored build output")
        require_accepted(repository, expected_sha, "ignored build artifact")

        ignored.unlink()
        ignored.parent.rmdir()
        require_accepted(repository, expected_sha, "accepted after cleanup")

    print("S1_SOURCE_SHA_BINDING=PASS")
    print("S1_SOURCE_SHA_NOT_HARDCODED=PASS")
    print("S1_DIRTY_TRACKED_SOURCE_REJECTED=PASS")
    print("S1_DIRTY_STAGED_SOURCE_REJECTED=PASS")
    print("S1_DIRTY_UNTRACKED_SOURCE_REJECTED=PASS")
    print("S1_HIDDEN_INDEX_SOURCE_REJECTED=PASS")
    print("S1_IGNORED_BUILD_ARTIFACT_POLICY=ACCEPT")
    print("S1_SOURCE_PROVENANCE_CLEANUP_RECOVERY=PASS")
    print("F3_PHYSICAL_PROVENANCE_TEST=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
