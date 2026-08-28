#!/usr/bin/env python3
"""No-build exact flash implementation for one frozen ESP-IDF build."""

from __future__ import annotations

import hashlib
import os
import subprocess
import sys
from pathlib import Path

from p4_nano_flash_metadata import FlashPlan, MetadataError, Payload, parse_flash_plan


BAUD = "1500000"
TRANSPORT_MARKERS = (
    "failed to connect",
    "no serial data received",
    "no response",
    "timed out",
    "could not open port",
    "port is busy",
    "serialexception",
)


def _shell_quote(value: str) -> str:
    import shlex

    return shlex.quote(value)


def snapshot_payloads(payloads: tuple[Payload, ...]) -> dict[Path, tuple[int, str]]:
    snapshot: dict[Path, tuple[int, str]] = {}
    for payload in payloads:
        try:
            data = payload.path.read_bytes()
            snapshot[payload.path] = (len(data), hashlib.sha256(data).hexdigest())
        except OSError as exc:
            raise MetadataError(f"cannot snapshot payload {payload.path}: {exc}") from exc
    return snapshot


def payloads_stable(
    before: dict[Path, tuple[int, str]], payloads: tuple[Payload, ...]
) -> bool:
    try:
        after = snapshot_payloads(payloads)
    except MetadataError:
        return False
    return after == before


def _matches_plan(snapshot: dict[Path, tuple[int, str]], plan: FlashPlan) -> bool:
    return all(
        snapshot.get(payload.path) == (payload.byte_count, payload.sha256)
        for payload in plan.payloads
    )


def _print_payloads(plan: FlashPlan, snapshots: dict[Path, tuple[int, str]], attempt: int | None = None) -> None:
    suffix = "" if attempt is None else f" attempt={attempt}"
    for payload in plan.payloads:
        size, sha256 = snapshots[payload.path]
        print(
            "P4_NANO_FLASH_EXACT_PAYLOAD"
            f" role={payload.role} offset=0x{payload.offset:x} bytes={size}"
            f" sha256={sha256} path={_shell_quote(str(payload.path))}{suffix}"
        )


def _print_plan(plan: FlashPlan) -> None:
    settings = plan.write_flash_args
    setting_values = {settings[index][2:]: settings[index + 1]
                      for index in range(len(settings) - 1)
                      if settings[index].startswith("--")}
    print(
        "P4_NANO_FLASH_EXACT_PLAN"
        f" build_dir={_shell_quote(str(plan.build_dir))}"
        f" variant={plan.variant or 'UNKNOWN'} board={plan.board or 'UNKNOWN'}"
        f" chip={plan.chip} flash_mode={setting_values.get('flash_mode', 'UNKNOWN')}"
        f" flash_freq={setting_values.get('flash_freq', 'UNKNOWN')}"
        f" flash_size={setting_values.get('flash_size', 'UNKNOWN')} result=PENDING"
    )
    snapshots = snapshot_payloads(plan.payloads)
    _print_payloads(plan, snapshots)
    print(
        "P4_NANO_FLASH_EXACT_APP"
        f" offset=0x{plan.app.offset:x} bytes={plan.app.byte_count}"
        f" sha256={plan.app.sha256} path={_shell_quote(str(plan.app.path))}"
    )
    for provenance in (plan.elf, plan.map_file):
        if provenance is not None:
            print(
                "P4_NANO_FLASH_EXACT_PROVENANCE"
                f" role={provenance.role} bytes={provenance.byte_count}"
                f" sha256={provenance.sha256} path={_shell_quote(str(provenance.path))}"
            )
    print(
        "P4_NANO_FLASH_EXACT_PROFILE"
        f" audio_only={plan.audio_profile or 'UNKNOWN'} audio_opt={plan.audio_opt or 'UNKNOWN'}"
    )


def _canonical_dir(value: str | os.PathLike[str]) -> Path:
    return Path(value).expanduser().resolve()


def _validate_python_environment(
    active_executable: str,
    active_prefix: str,
    active_base_prefix: str,
    expected_environment: Path,
) -> str:
    """Validate venv identity and return a launcher path, never its realpath."""
    expected = _canonical_dir(expected_environment)
    launcher = expected / "bin" / "python"
    if not launcher.is_file() or not os.access(launcher, os.X_OK):
        raise RuntimeError("ENVIRONMENT_INVALID: IDF_PYTHON_ENV_PATH launcher is invalid")
    if _canonical_dir(active_prefix) != expected:
        raise RuntimeError("ENVIRONMENT_INVALID: active Python prefix does not match IDF_PYTHON_ENV_PATH")
    if _canonical_dir(active_prefix) == _canonical_dir(active_base_prefix):
        raise RuntimeError("ENVIRONMENT_INVALID: active interpreter is not an IDF virtual environment")

    candidate = Path(active_executable)
    if candidate.is_absolute():
        try:
            candidate.resolve().relative_to(expected)
        except ValueError:
            # Some venvs expose bin/python as a symlink whose sys.executable is
            # the base interpreter. Keep the venv launcher for subprocesses.
            return str(launcher)
        return str(candidate)
    return str(launcher)


def _python_prefixes(executable: str) -> tuple[str, str]:
    try:
        probe = subprocess.run(
            [executable, "-c", "import sys; print(sys.prefix); print(sys.base_prefix)"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
    except OSError as exc:
        raise RuntimeError(f"ENVIRONMENT_INVALID: cannot execute Python launcher: {exc}") from exc
    if probe.returncode != 0:
        raise RuntimeError("ENVIRONMENT_INVALID: cannot inspect Python environment prefix")
    lines = probe.stdout.splitlines()
    if len(lines) != 2 or not all(lines):
        raise RuntimeError("ENVIRONMENT_INVALID: Python environment probe returned malformed output")
    return lines[0], lines[1]


def _active_environment(plan: FlashPlan) -> tuple[str, str]:
    serial = os.environ.get("P4_NANO_SERIAL", "")
    if not serial:
        raise RuntimeError("ENVIRONMENT_INVALID: P4_NANO_SERIAL is required")
    idf_path = os.environ.get("IDF_PATH", "")
    if not idf_path or not (Path(idf_path) / "tools").is_dir():
        raise RuntimeError("ENVIRONMENT_INVALID: IDF_PATH is not an active ESP-IDF installation")
    python_env = os.environ.get("IDF_PYTHON_ENV_PATH", "")
    expected = Path(python_env)
    if not python_env or not expected.is_dir():
        raise RuntimeError("ENVIRONMENT_INVALID: IDF_PYTHON_ENV_PATH is invalid")
    active_launcher = _validate_python_environment(
        sys.executable,
        sys.prefix,
        sys.base_prefix,
        expected,
    )
    probe = subprocess.run(
        [active_launcher, "-c", "import esptool"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if probe.returncode != 0:
        raise RuntimeError("ENVIRONMENT_INVALID: esptool is unavailable in the active Python")
    cache_path = plan.build_dir / "CMakeCache.txt"
    configured = None
    if cache_path.is_file():
        for line in cache_path.read_text(encoding="utf-8").splitlines():
            if line.startswith("PYTHON:") and "=" in line:
                configured = line.split("=", 1)[1]
                break
    if configured:
        configured_path = Path(configured)
        if not configured_path.is_absolute():
            configured_path = plan.build_dir / configured_path
        if not configured_path.is_file() or not os.access(configured_path, os.X_OK):
            raise RuntimeError("ENVIRONMENT_INVALID: build Python launcher is invalid")
        configured_prefix, configured_base_prefix = _python_prefixes(str(configured_path))
        if _canonical_dir(configured_prefix) != _canonical_dir(expected):
            raise RuntimeError("ENVIRONMENT_INVALID: build Python does not match IDF_PYTHON_ENV_PATH")
        if _canonical_dir(configured_base_prefix) != _canonical_dir(sys.base_prefix):
            raise RuntimeError("ENVIRONMENT_INVALID: build Python base environment does not match active Python")
    return active_launcher, serial


def _esptool_command(plan: FlashPlan, python_path: str, serial: str) -> list[str]:
    command = [
        python_path,
        "-m",
        "esptool",
        "--chip",
        plan.chip,
        "-p",
        serial,
        "-b",
        BAUD,
        "--before",
        plan.before,
        "--after",
        plan.after,
        "write_flash",
        *plan.write_flash_args,
    ]
    for payload in plan.payloads:
        command.extend((f"0x{payload.offset:x}", str(payload.path)))
    return command


def _is_transport_failure(output: str) -> bool:
    text = output.lower()
    return any(marker in text for marker in TRANSPORT_MARKERS)


def _run(plan: FlashPlan) -> int:
    try:
        python_path, serial = _active_environment(plan)
    except (OSError, RuntimeError) as exc:
        print(str(exc), file=sys.stderr)
        print("P4_NANO_FLASH_EXACT result=ENVIRONMENT_INVALID", file=sys.stderr)
        return 3

    command = _esptool_command(plan, python_path, serial)
    printable = "python -m esptool " + " ".join(_shell_quote(arg) for arg in command[3:])
    print(f"P4_NANO_FLASH_EXACT_COMMAND {printable}")
    for attempt in range(1, 4):
        try:
            snapshots = snapshot_payloads(plan.payloads)
        except MetadataError as exc:
            print(f"LOCAL_PLAN_INVALID: {exc}", file=sys.stderr)
            print("P4_NANO_FLASH_EXACT result=LOCAL_PLAN_INVALID", file=sys.stderr)
            return 2
        if not _matches_plan(snapshots, plan):
            print("P4_NANO_FLASH_INPUT_STABILITY result=FAIL", file=sys.stderr)
            print("P4_NANO_FLASH_EXACT result=INPUT_CHANGED_DURING_FLASH", file=sys.stderr)
            return 6
        _print_payloads(plan, snapshots, attempt)
        try:
            completed = subprocess.run(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                check=False,
            )
        except OSError as exc:
            print(f"esptool invocation failed: {exc}", file=sys.stderr)
            print("P4_NANO_FLASH_EXACT result=ESPTOOL_WRITE_FAILURE", file=sys.stderr)
            return 5
        if completed.stdout:
            print(completed.stdout, end="")
        stable = payloads_stable(snapshots, plan.payloads)
        if not stable:
            print("P4_NANO_FLASH_INPUT_STABILITY result=FAIL", file=sys.stderr)
            print("P4_NANO_FLASH_EXACT result=INPUT_CHANGED_DURING_FLASH", file=sys.stderr)
            return 6
        if completed.returncode == 0:
            print("P4_NANO_FLASH_INPUT_STABILITY result=PASS")
            print("P4_NANO_FLASH_EXACT result=SUCCESS")
            return 0
        if _is_transport_failure(completed.stdout or "") and attempt < 3:
            print(
                f"P4_NANO_FLASH_EXACT_RETRY attempt={attempt + 1} reason=SERIAL_TRANSPORT_FAILURE",
                file=sys.stderr,
            )
            continue
        if _is_transport_failure(completed.stdout or ""):
            print("P4_NANO_FLASH_EXACT result=SERIAL_TRANSPORT_FAILURE", file=sys.stderr)
            return 4
        print("P4_NANO_FLASH_EXACT result=ESPTOOL_WRITE_FAILURE", file=sys.stderr)
        return 5
    return 4


def main(argv: list[str]) -> int:
    import argparse

    parser = argparse.ArgumentParser(
        description="Write already-generated P4-NANO payloads without invoking a project generator."
    )
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--print-plan", action="store_true")
    parser.add_argument("--expected-variant", default="p4-v1x")
    parser.add_argument("--expected-board", default="p4-nano")
    args = parser.parse_args(argv)
    try:
        plan = parse_flash_plan(
            args.build_dir,
            expected_variant=args.expected_variant,
            expected_board=args.expected_board,
        )
        if args.print_plan:
            _print_plan(plan)
            print("P4_NANO_FLASH_EXACT_PLAN result=PASS")
            print("hardware_touched=NO")
            return 0
        return _run(plan)
    except MetadataError as exc:
        print(f"LOCAL_PLAN_INVALID: {exc}", file=sys.stderr)
        print("P4_NANO_FLASH_EXACT_PLAN result=FAIL", file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"LOCAL_PLAN_INVALID: {exc}", file=sys.stderr)
        print("P4_NANO_FLASH_EXACT_PLAN result=FAIL", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
