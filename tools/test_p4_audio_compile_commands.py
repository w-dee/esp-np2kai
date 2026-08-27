#!/usr/bin/env python3
"""Check final optimization arguments for the measured audio pipeline."""

from __future__ import annotations

import argparse
import json
import shlex
from pathlib import Path


HOT_SOURCES = (
    "p4_nano_audio_benchmark.cpp",
    "np2opngen_e1b_stream.c",
    "np2opngen_spsc.c",
    "np2opngen_pcm_canonical.c",
    "np2opngen_synth_event.c",
    "np2opngen_s98.c",
    "np2opngen_synthetic_workload.c",
    "opngenc.c",
    "opngeng.c",
)
OPTIMIZATION_FLAGS = {"-O0", "-O1", "-O2", "-O3", "-Og", "-Os", "-Oz"}


def command_tokens(entry: dict[str, object]) -> list[str]:
    if isinstance(entry.get("arguments"), list):
        return [str(token) for token in entry["arguments"]]
    command = entry.get("command")
    if not isinstance(command, str):
        raise ValueError("compile command has neither arguments nor command")
    return shlex.split(command)


def source_name(entry: dict[str, object]) -> str:
    file_name = entry.get("file")
    if isinstance(file_name, str):
        return Path(file_name).name
    tokens = command_tokens(entry)
    for token in reversed(tokens):
        if token.endswith(HOT_SOURCES):
            return Path(token).name
    return ""


def check(build_dir: Path, expected: str) -> None:
    path = build_dir / "compile_commands.json"
    entries = json.loads(path.read_text(encoding="utf-8"))
    by_name: dict[str, list[tuple[str, list[str]]]] = {name: [] for name in HOT_SOURCES}
    for entry in entries:
        name = source_name(entry)
        if name not in by_name:
            continue
        tokens = command_tokens(entry)
        options = [token for token in tokens if token in OPTIMIZATION_FLAGS]
        by_name[name].append((str(entry.get("file", name)), options))

    missing = [name for name, commands in by_name.items() if not commands]
    if missing:
        raise AssertionError("missing compile commands: " + ",".join(missing))

    for name in HOT_SOURCES:
        commands = by_name[name]
        if len(commands) != 1:
            raise AssertionError(f"expected one command for {name}, got {len(commands)}")
        file_name, options = commands[0]
        if not options:
            raise AssertionError(f"no optimization flag for {file_name}")
        final = options[-1]
        if expected == "o2" and final != "-O2":
            raise AssertionError(f"{file_name}: optimization order={options!r}")
        if expected == "debug" and final in {"-O2", "-O3"}:
            raise AssertionError(f"{file_name}: debug optimization order={options!r}")
        print(f"P4_AUDIO_COMPILE source={name} optimization_order={' '.join(options)} final={final}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--expected", choices=("o2", "debug"), required=True)
    args = parser.parse_args()
    check(args.build_dir, args.expected)
    print(f"P4_AUDIO_COMPILE_COMMANDS=PASS expected={args.expected}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
