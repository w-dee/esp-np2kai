#!/usr/bin/env python3
"""Prove the host mapped I286/V30 units consume the fastpath overlay in A/B."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "host"
NP2CORE_INCLUDE = ROOT / "firmware/components/np2core/include"
VENDOR_SRC = ROOT / "third_party/np2kai/src"
CONTRACT_DEFINES = (
    "DISABLE_SOUND",
    "SUPPORT_KAI_IMAGES",
    "SUPPORT_FAST_MEMORYCHECK",
    "SUPPORT_16BPP",
    "BYTESEX_LITTLE",
    "OSLANG_UTF8",
    "SUPPORT_UTF8",
)


def run(command: list[str], *, cwd: Path = ROOT) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if completed.returncode:
        raise RuntimeError(
            f"command failed ({completed.returncode}): {' '.join(command)}\n"
            f"{completed.stdout}"
        )
    return completed


def preprocess(
    compiler: str, source: Path, overlay: Path, mode: str, output_root: Path
) -> tuple[str, str]:
    output = output_root / f"{source.stem}-{mode}.i"
    trace = output_root / f"{source.stem}-{mode}.includes"
    command = [
        compiler,
        "-std=c11",
        "-E",
        "-dD",
        "-H",
        f"-DNP2_I286C_INLINE_MEM_FASTPATH={mode}",
        *(f"-D{item}" for item in CONTRACT_DEFINES),
        "-I",
        str(overlay),
        "-I",
        str(NP2CORE_INCLUDE),
        "-I",
        str(HOST / "compat"),
        "-I",
        str(VENDOR_SRC),
        "-I",
        str(VENDOR_SRC / "i286c"),
        str(source),
    ]
    completed = subprocess.run(
        command,
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode:
        raise RuntimeError(
            f"preprocess failed ({completed.returncode}): {' '.join(command)}\n"
            f"{completed.stdout}\n{completed.stderr}"
        )
    output.write_text(completed.stdout, encoding="utf-8")
    trace.write_text(completed.stderr, encoding="utf-8")
    return completed.stdout, completed.stderr


def require(value: str, fragment: str, description: str) -> None:
    if fragment not in value:
        raise AssertionError(f"missing {description}: {fragment}")


def main() -> int:
    compiler = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
    if not compiler:
        print("SKIP: no host C compiler available")
        return 0

    with tempfile.TemporaryDirectory(prefix="np2-i286-host-integration-") as temporary:
        root = Path(temporary)
        for mode in ("0", "1"):
            build_dir = root / f"build-{mode}"
            run(
                [
                    "make",
                    "-C",
                    str(HOST),
                    f"BUILD_DIR={build_dir}",
                    f"FASTPATH={mode}",
                    "phase1-probe-mapped",
                    "phase1-link-probe-mapped",
                ]
            )
            compile_results = json.loads(
                (build_dir / "compile-mapped/compile-results.json").read_text(
                    encoding="utf-8"
                )
            )
            if f"NP2_I286C_INLINE_MEM_FASTPATH={mode}" not in compile_results["defines"]:
                raise AssertionError("mapped compile did not record the requested FASTPATH mode")
            for logical in ("i286c/i286c.c", "i286c/v30patch.c"):
                result = next(
                    item for item in compile_results["results"] if item["source"] == logical
                )
                if result["ownership"] != "vendor-overlay" or result["returncode"] != 0:
                    raise AssertionError(f"mapped {logical} did not compile through overlay")

            overlay = build_dir / "patches/host-overlay/i286c"
            for logical in ("i286c.c", "v30patch.c"):
                preprocessed, includes = preprocess(
                    compiler, overlay / logical, overlay, mode, root
                )
                require(includes, str(overlay / "i286c.h"), f"overlay i286c.h for {logical}")
                require(
                    includes,
                    "np2_i286_inline_mem_fastpath.h",
                    f"project fastpath header for {logical}",
                )
                require(
                    preprocessed,
                    f"#define NP2_I286C_INLINE_MEM_FASTPATH {mode}",
                    f"FASTPATH={mode} preprocessor definition for {logical}",
                )
                if mode == "1":
                    require(
                        preprocessed,
                        "np2_i286_inline_memoryread8(UINT32 address)",
                        f"optimized helper in {logical}",
                    )
                    require(
                        preprocessed,
                        "NP2_I286_MEMORYREAD8(address) np2_i286_inline_memoryread8((address))",
                        f"optimized access macro in {logical}",
                    )
                else:
                    require(
                        preprocessed,
                        "NP2_I286_MEMORYREAD8(address) memp_read8(address)",
                        f"reference access macro in {logical}",
                    )

            print(
                f"I286_HOST_INTEGRATION_PASS mode={mode} "
                "units=i286c.c,v30patch.c ownership=vendor-overlay"
            )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, RuntimeError, json.JSONDecodeError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
