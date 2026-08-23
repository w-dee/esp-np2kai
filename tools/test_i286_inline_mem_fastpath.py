#!/usr/bin/env python3
"""Compile and run the I286 normal-RAM access contract in both A/B modes."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "host/tests/i286_inline_mem_fastpath_contract.c"
INCLUDE = ROOT / "firmware/components/np2core/include"


def run(command: list[str]) -> None:
    completed = subprocess.run(
        command,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if completed.returncode:
        raise RuntimeError(f"command failed ({completed.returncode}): {' '.join(command)}\n"
                           f"{completed.stdout}")
    if completed.stdout:
        print(completed.stdout, end="")


def main() -> int:
    compiler = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
    if not compiler:
        print("SKIP: no host C compiler available")
        return 0
    with tempfile.TemporaryDirectory(prefix="np2-i286-fastpath-") as temporary:
        output_root = Path(temporary)
        for mode in ("0", "1"):
            executable = output_root / f"contract-{mode}"
            run([
                compiler,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-DNP2_I286C_INLINE_MEM_FASTPATH={mode}",
                "-I",
                str(INCLUDE),
                str(SOURCE),
                "-o",
                str(executable),
            ])
            run([str(executable)])
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
