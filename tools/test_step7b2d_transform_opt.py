#!/usr/bin/env python3
"""Contract checks for the Step 7B.2d transform TU-only optimization A/B."""

from __future__ import annotations

import argparse
import json
import pathlib
import shlex
import subprocess
from typing import Any, NoReturn


ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD_SCRIPT = ROOT / "tools/emu/build-production.sh"
DISPLAY_CMAKE = ROOT / "firmware/components/p4_nano_display/CMakeLists.txt"
SDKCONFIG = ROOT / "firmware/sdkconfig"


def fail(message: str) -> NoReturn:
    raise AssertionError(message)


def optimization_flags(command: str) -> list[str]:
    """Return optimization flags in command order, excluding unrelated -f flags."""

    flags: list[str] = []
    for token in shlex.split(command):
        if token in {"-O", "-O0", "-O1", "-O2", "-O3", "-Og", "-Os", "-Oz", "-Ofast"}:
            flags.append(token)
    return flags


def source_name(entry: dict[str, Any]) -> str:
    return pathlib.Path(str(entry.get("file", ""))).name


def load_compile_commands(path: pathlib.Path) -> list[dict[str, Any]]:
    try:
        entries = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"cannot read compile_commands.json {path}: {exc}")
    if not isinstance(entries, list):
        fail(f"compile_commands.json is not an array: {path}")
    return entries


def project_source_identity(file_name: str) -> str | None:
    for marker in ("/firmware/", "/np2core/"):
        if marker in file_name:
            return file_name[file_name.index(marker):]
    return None


def check_non_transform_flags_match(
    current: list[dict[str, Any]], baseline: list[dict[str, Any]]
) -> None:
    def project_flags(entries: list[dict[str, Any]]) -> dict[str, list[str]]:
        result: dict[str, list[str]] = {}
        for entry in entries:
            file_name = str(entry.get("file", ""))
            identity = project_source_identity(file_name)
            if identity is not None:
                result[identity] = optimization_flags(str(entry.get("command", "")))
        return result

    current_flags = project_flags(current)
    baseline_flags = project_flags(baseline)
    if current_flags.keys() != baseline_flags.keys():
        fail("debug/o2 compile-command project source inventories differ")
    for identity in sorted(current_flags):
        if identity.endswith("/p4_nano_display_transform.cpp"):
            continue
        if current_flags[identity] != baseline_flags[identity]:
            fail("transform optimization leaked into non-transform TU "
                 f"{identity}: baseline={baseline_flags[identity]} "
                 f"current={current_flags[identity]}")


def check_compile_commands(
    path: pathlib.Path, expected: str, baseline_path: pathlib.Path | None = None
) -> None:
    entries = load_compile_commands(path)

    transform_entries = [entry for entry in entries
                         if source_name(entry) == "p4_nano_display_transform.cpp"]
    if len(transform_entries) != 1:
        fail("expected exactly one transform translation-unit command, found "
             f"{len(transform_entries)}")
    transform_flags = optimization_flags(str(transform_entries[0].get("command", "")))
    if expected == "debug":
        if transform_flags[-1:] != ["-Og"] or "-O2" in transform_flags:
            fail(f"debug transform command does not end in -Og: {transform_flags}")
    elif expected == "o2":
        try:
            debug_index = transform_flags.index("-Og")
            o2_index = len(transform_flags) - 1 - transform_flags[::-1].index("-O2")
        except ValueError:
            fail(f"o2 transform command lacks global -Og followed by -O2: {transform_flags}")
        if o2_index <= debug_index or transform_flags[-1:] != ["-O2"]:
            fail(f"o2 transform command has unexpected optimization order: {transform_flags}")
    else:
        fail(f"unsupported expected mode: {expected}")

    representative_names = {
        "p4_nano_display.cpp",
        "p4_nano_live_display.cpp",
        "np2video_runner.c",
    }
    representatives: dict[str, dict[str, Any]] = {}
    np2core_entry: dict[str, Any] | None = None
    for entry in entries:
        name = source_name(entry)
        command = str(entry.get("command", ""))
        flags = optimization_flags(command)
        if name in representative_names:
            representatives[name] = entry
        file_name = str(entry.get("file", ""))
        if np2core_entry is None and "/np2core/" in file_name and name.endswith((".c", ".cc", ".cpp")):
            np2core_entry = entry
        project_source = "/firmware/" in file_name or "/np2core/" in file_name
        if (name != "p4_nano_display_transform.cpp" and project_source
                and "-O2" in flags):
            fail(f"-O2 leaked into non-transform TU {file_name}: {flags}")

    missing = sorted(representative_names - representatives.keys())
    if missing:
        fail("compile_commands.json is missing representative TUs: " + ", ".join(missing))
    if np2core_entry is None:
        fail("compile_commands.json is missing an np2core translation unit")
    for name, entry in sorted(representatives.items()):
        flags = optimization_flags(str(entry.get("command", "")))
        if flags[-1:] != ["-Og"]:
            fail(f"representative {name} does not remain -Og: {flags}")
    np2core_flags = optimization_flags(str(np2core_entry.get("command", "")))
    if "-O2" in np2core_flags or np2core_flags[-1:] != ["-Og"]:
        fail(f"np2core TU does not remain -Og: {np2core_flags}")
    if baseline_path is not None:
        check_non_transform_flags_match(entries, load_compile_commands(baseline_path))

    print(f"TRANSFORM_O2_SCOPE=TU_ONLY mode={expected} compile_commands={path}")


def run_rejected(*arguments: str, expected_fragment: str) -> None:
    completed = subprocess.run(
        [str(BUILD_SCRIPT), *arguments],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if completed.returncode != 2:
        fail(f"expected build-production.sh exit 2 for {arguments}, got "
             f"{completed.returncode}: {completed.stdout}")
    if expected_fragment not in completed.stdout:
        fail(f"missing rejection {expected_fragment!r} for {arguments}: {completed.stdout}")


def check_source_contract() -> None:
    build = BUILD_SCRIPT.read_text(encoding="utf-8")
    cmake = DISPLAY_CMAKE.read_text(encoding="utf-8")
    sdkconfig = SDKCONFIG.read_text(encoding="utf-8")

    if "transform_opt=debug" not in build:
        fail("transform selector default is not debug")
    for option in ("--transform-opt)", "--transform-opt=*"):
        if option not in build:
            fail(f"missing CLI form {option}")
    if "debug|o2" not in build or "requires exactly debug or o2" not in build:
        fail("selector validation is incomplete")
    if "--transform-opt o2 requires --live-display-benchmark or --live-display-transform-isolated-benchmark" not in build:
        fail("o2 profile restriction is missing")
    if "! live_display_benchmark && ! live_display_transform_isolated_benchmark" not in build:
        fail("o2 must be allowed for both live benchmark profiles")
    for profile in ("--live-display-benchmark", "--live-display-transform-isolated-benchmark"):
        if profile not in build:
            fail(f"missing accepted O2 profile selector: {profile}")
    if "P4_NANO_DISPLAY_TRANSFORM_OPT=${transform_opt}" not in build:
        fail("transform selector is not passed to CMake")
    if "transform_opt=%s" not in build:
        fail("build output does not report transform_opt")
    if "P4_NANO_DISPLAY_TRANSFORM_OPT \"debug\" CACHE STRING" not in cmake:
        fail("CMake selector default is not debug")
    if "set_source_files_properties" not in cmake or "p4_nano_display_transform.cpp" not in cmake:
        fail("transform source-specific property is missing")
    if 'PROPERTIES COMPILE_OPTIONS "-O2"' not in cmake:
        fail("transform TU -O2 property is missing")
    target_options = cmake.split("set(P4_NANO_DISPLAY_TRANSFORM_OPT", 1)[0]
    if "-O2" in target_options:
        fail("-O2 was added to component-wide options")
    if "-flto" in cmake or "-flto" in build:
        fail("transform selector introduced LTO")
    if "CONFIG_COMPILER_OPTIMIZATION_DEBUG=y" not in sdkconfig:
        fail("global debug sdkconfig optimization baseline is missing")

    run_rejected("--variant", "p4-v1x", "--transform-opt", "fast",
                 expected_fragment="requires exactly debug or o2")
    run_rejected("--variant", "p4-v1x", "--board", "p4-nano",
                 "--live-display", "--transform-opt", "o2",
                 expected_fragment="requires --live-display-benchmark or --live-display-transform-isolated-benchmark")
    run_rejected("--variant", "p4-v1x", "--board", "p4-nano",
                 "--display-foundation", "--transform-opt", "o2",
                 expected_fragment="requires --live-display-benchmark or --live-display-transform-isolated-benchmark")
    run_rejected("--variant", "p4-v1x", "--board", "p4-nano",
                 "--display-transform-diagnostic", "--rotation", "cw",
                 "--transform-opt", "o2",
                 expected_fragment="requires --live-display-benchmark or --live-display-transform-isolated-benchmark")
    run_rejected("--variant", "p4-v3x", "--board", "generic",
                 "--transform-opt", "o2",
                 expected_fragment="requires --live-display-benchmark or --live-display-transform-isolated-benchmark")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compile-commands", type=pathlib.Path)
    parser.add_argument("--baseline-compile-commands", type=pathlib.Path)
    parser.add_argument("--expected", choices=("debug", "o2"))
    args = parser.parse_args()
    if (args.compile_commands is None) != (args.expected is None):
        parser.error("--compile-commands and --expected must be supplied together")
    if args.baseline_compile_commands is not None and args.compile_commands is None:
        parser.error("--baseline-compile-commands requires --compile-commands")

    check_source_contract()
    if args.compile_commands is not None:
        check_compile_commands(
            args.compile_commands, args.expected, args.baseline_compile_commands
        )
    print("Step 7B.2d transform TU-only optimization contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
