#!/usr/bin/env python3
"""Audit the exact P4 SPSC object for out-of-line atomic helpers."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys


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
            [args.objdump, "-d", args.object], check=True,
            capture_output=True, text=True
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"P4_AUDIO_ATOMIC codegen_audit=ERROR reason={exc}")
        return 2
    needed = ["np2opngen_spsc_enqueue", "np2opngen_spsc_dequeue"]
    missing = [name for name in needed if name not in symbols]
    helper = re.findall(r"__atomic_|__sync_", disassembly)
    out_of_line = re.findall(r"\bcall\b[^\n]*(?:__atomic_|__sync_|libatomic)", disassembly)
    if missing or helper or out_of_line:
        print("P4_AUDIO_ATOMIC codegen_audit=FAIL "
              f"missing={','.join(missing) or 'none'} helpers={len(helper)} "
              f"out_of_line={len(out_of_line)}")
        return 1
    print("P4_AUDIO_ATOMIC codegen_audit=PASS functions="
          "np2opngen_spsc_enqueue,np2opngen_spsc_dequeue helpers=0 out_of_line=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
