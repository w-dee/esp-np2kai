#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Build and verify NP2TEST twice in independent temporary directories."""

from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path


def _run(command: list[str], label: str) -> None:
    print(f"[reproducibility] {label}: {' '.join(command)}")
    completed = subprocess.run(command, check=False)
    if completed.returncode != 0:
        raise RuntimeError(f"{label} failed with exit status {completed.returncode}")


def _build_and_verify(layout: Path, builder: Path, verifier: Path, output_dir: Path,
                      label: str) -> tuple[Path, str]:
    image = output_dir / "np2test-fd1232.image"
    checksum = image.with_name(image.name + ".sha256")
    manifest = image.with_name(image.name + ".manifest.json")
    _run([
        sys.executable,
        str(builder),
        "--layout",
        str(layout),
        "--output",
        str(image),
    ], f"build {label}")
    _run([
        sys.executable,
        str(verifier),
        "--layout",
        str(layout),
        "--image",
        str(image),
        "--sha256",
        str(checksum),
    ], f"verify {label}")
    if not manifest.is_file():
        raise RuntimeError(f"build {label} did not produce {manifest}")
    digest = hashlib.sha256(image.read_bytes()).hexdigest()
    return image, digest


def check(layout: Path, builder: Path, verifier: Path) -> int:
    with tempfile.TemporaryDirectory(prefix="np2test-repro-") as temporary:
        root = Path(temporary)
        output_a = root / "build-a"
        output_b = root / "build-b"
        output_a.mkdir()
        output_b.mkdir()
        try:
            image_a, digest_a = _build_and_verify(layout, builder, verifier, output_a, "A")
            image_b, digest_b = _build_and_verify(layout, builder, verifier, output_b, "B")
        except (OSError, RuntimeError) as exc:
            print(f"error: NP2TEST_REPRODUCIBILITY_ERROR: {exc}", file=sys.stderr)
            return 2

        if image_a.read_bytes() != image_b.read_bytes() or digest_a != digest_b:
            print("error: BUILD_REPRODUCIBILITY_ERROR: independent builds differ", file=sys.stderr)
            print(f"  A: {image_a} sha256={digest_a}", file=sys.stderr)
            print(f"  B: {image_b} sha256={digest_b}", file=sys.stderr)
            return 1
        print(f"[reproducibility] equal image bytes and SHA-256: {digest_a}")
        print(f"[reproducibility] independent outputs: {image_a}, {image_b}")
        return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--layout", type=Path, required=True)
    parser.add_argument("--builder", type=Path, required=True)
    parser.add_argument("--verifier", type=Path, required=True)
    args = parser.parse_args(argv)
    return check(args.layout, args.builder, args.verifier)


if __name__ == "__main__":
    raise SystemExit(main())
