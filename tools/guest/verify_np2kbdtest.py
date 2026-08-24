#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Verify a deterministic NP2 keyboard fixture artifact."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

try:
    from .build_np2kbdtest import LayoutError, load_layout
except ImportError:
    from build_np2kbdtest import LayoutError, load_layout


class VerificationError(ValueError):
    pass


def verify(layout_path: Path, image_path: Path, sha_path: Path | None = None) -> str:
    layout = load_layout(layout_path)
    image = image_path.read_bytes()
    if len(image) != layout["image"]["size_bytes"]:
        raise VerificationError("image size is invalid")
    if any(image[1024:]):
        raise VerificationError("bytes after the standalone IPL must be zero")
    if image[510:512] != b"\x55\xaa" or image[1022:1024] != b"\x55\xaa":
        raise VerificationError("IPL signatures are invalid")
    digest = hashlib.sha256(image).hexdigest()
    expected = layout["artifact"].get("expected_sha256")
    if expected is not None and digest != expected:
        raise VerificationError(f"sha256 is {digest}, expected {expected}")
    if sha_path is not None:
        fields = sha_path.read_text(encoding="ascii").strip().split()
        if len(fields) < 2 or fields[0].lower() != digest or fields[-1] != image_path.name:
            raise VerificationError("checksum file does not match image")
    return digest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--layout", type=Path, required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--sha256", type=Path)
    args = parser.parse_args()
    try:
        print(f"verified {args.image} ({verify(args.layout, args.image, args.sha256)})")
    except (LayoutError, VerificationError, OSError) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
