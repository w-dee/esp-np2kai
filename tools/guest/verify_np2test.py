#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Validate an NP2TEST Stage 1 image and its optional digest file."""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

try:
    from .build_np2test import LayoutError, load_layout
except ImportError:  # Direct execution: python3 tools/guest/verify_np2test.py
    from build_np2test import LayoutError, load_layout


class VerificationError(ValueError):
    """Raised when an NP2TEST artifact does not match its contract."""


def verify(layout_path: Path, image_path: Path, sha256_path: Path | None = None,
           expected_sha256: str | None = None) -> str:
    layout = load_layout(layout_path)
    try:
        image = image_path.read_bytes()
    except OSError as exc:
        raise VerificationError(f"cannot read image {image_path}: {exc}") from exc
    expected_size = layout["image"]["size_bytes"]
    if len(image) != expected_size:
        raise VerificationError(f"image size is {len(image)}, expected {expected_size}")

    ipl_size = layout["ipl"]["binary_size"]
    for offset, byte in enumerate(image[ipl_size:], start=ipl_size):
        if byte != 0:
            raise VerificationError(f"unexpected payload byte 0x{byte:02x} at offset 0x{offset:x}")

    for signature in layout["ipl"]["signatures"]:
        offset = signature["offset"]
        expected = bytes.fromhex(signature["bytes"])
        if image[offset:offset + len(expected)] != expected:
            raise VerificationError(f"IPL signature at offset 0x{offset:x} is invalid")

    digest = hashlib.sha256(image).hexdigest()
    layout_expected_sha256 = layout["artifact"].get("expected_sha256")
    if layout_expected_sha256 is not None and digest != layout_expected_sha256:
        raise VerificationError(f"sha256 is {digest}, expected layout golden {layout_expected_sha256}")
    if expected_sha256 is not None and digest != expected_sha256.lower():
        raise VerificationError(f"sha256 is {digest}, expected {expected_sha256}")
    if sha256_path is not None:
        try:
            fields = sha256_path.read_text(encoding="ascii").strip().split()
        except (OSError, UnicodeDecodeError) as exc:
            raise VerificationError(f"cannot read checksum file {sha256_path}: {exc}") from exc
        if len(fields) < 2 or fields[0].lower() != digest or fields[-1] != image_path.name:
            raise VerificationError(f"checksum file does not match {image_path.name}")
    return digest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--layout", type=Path, required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--sha256", type=Path)
    parser.add_argument("--expected-sha256")
    args = parser.parse_args(argv)
    try:
        digest = verify(args.layout, args.image, args.sha256, args.expected_sha256)
    except (LayoutError, VerificationError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(f"verified {args.image} ({digest})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
