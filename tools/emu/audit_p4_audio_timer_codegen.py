#!/usr/bin/env python3
"""Audit the compiled pacing callback for out-of-line atomic helpers."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys


def fail(reason: str) -> int:
    print(f"P4_AUDIO_TIMER_CALLBACK_AUDIT=FAIL reason={reason}")
    return 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--object", required=True)
    parser.add_argument("--objdump", default="riscv32-esp-elf-objdump")
    args = parser.parse_args()
    try:
        symbols = subprocess.run(
            [args.objdump, "-t", args.object], check=True,
            capture_output=True, text=True
        ).stdout
        disassembly = subprocess.run(
            [args.objdump, "-dr", args.object], check=True,
            capture_output=True, text=True
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        return fail(f"objdump={exc}")

    symbol_line = next(
        (line for line in symbols.splitlines()
         if "pacing_timer_callback" in line and " F " in line),
        None,
    )
    if symbol_line is None:
        return fail("pacing_timer_callback_symbol_missing")
    fields = symbol_line.split()
    if len(fields) < 6:
        return fail("malformed_symbol_table")
    section = fields[-3]
    section_header = f"Disassembly of section {section}:"
    start = disassembly.find(section_header)
    if start < 0:
        return fail("pacing_timer_callback_disassembly_missing")
    body = disassembly[start:]
    next_section = body.find("\nDisassembly of section ", len(section_header))
    if next_section >= 0:
        body = body[:next_section]
    helpers = re.findall(r"(?:__atomic_|__sync_|libatomic)", body)
    if helpers:
        return fail(f"out_of_line_helpers={len(helpers)}")
    print("P4_AUDIO_TIMER_CALLBACK_AUDIT=PASS "
          "function=pacing_timer_callback helpers=0 out_of_line=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
