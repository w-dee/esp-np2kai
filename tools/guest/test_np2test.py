#!/usr/bin/env python3
"""Standard-library tests for the NP2TEST foundation tools."""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
ROOT = TOOLS_DIR.parents[1]
sys.path.insert(0, str(TOOLS_DIR))

import build_np2test  # noqa: E402
import verify_np2test  # noqa: E402


LAYOUT_PATH = ROOT / "tests/guest/np2test/layout.json"


class NP2TestFoundationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.work = Path(self.tempdir.name)

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def copy_layout(self, mutate=None) -> Path:
        layout = json.loads(LAYOUT_PATH.read_text(encoding="utf-8"))
        if mutate is not None:
            mutate(layout)
        path = self.work / "layout.json"
        path.write_text(json.dumps(layout), encoding="utf-8")
        return path

    def test_build_is_deterministic_and_verifies(self) -> None:
        output_a = self.work / "a.image"
        output_b = self.work / "b.image"
        layout = self.copy_layout()
        digest_a = build_np2test.build(layout, output_a)
        digest_b = build_np2test.build(layout, output_b)
        self.assertEqual(output_a.read_bytes(), output_b.read_bytes())
        self.assertEqual(digest_a, digest_b)
        self.assertEqual(digest_a, hashlib.sha256(output_a.read_bytes()).hexdigest())
        self.assertEqual(verify_np2test.verify(layout, output_a, output_a.with_name("a.image.sha256")), digest_a)
        data = output_a.read_bytes()
        self.assertEqual(data[510:512], b"\x55\xaa")
        self.assertEqual(data[1022:1024], b"\x55\xaa")
        self.assertEqual(sum(byte != 0 for byte in data), 4)

    def test_wrong_geometry_is_rejected(self) -> None:
        layout = self.copy_layout(lambda value: value["image"]["geometry"].update(heads=1))
        with self.assertRaises(build_np2test.LayoutError):
            build_np2test.load_layout(layout)

    def test_selected_extension_is_rejected_until_review(self) -> None:
        layout = self.copy_layout(lambda value: value["image"].update(extension=".hdm"))
        with self.assertRaises(build_np2test.LayoutError):
            build_np2test.load_layout(layout)

    def test_corrupted_payload_is_rejected(self) -> None:
        layout = self.copy_layout()
        output = self.work / "corrupt.image"
        build_np2test.build(layout, output)
        image = bytearray(output.read_bytes())
        image[0x100] = 1
        output.write_bytes(image)
        with self.assertRaises(verify_np2test.VerificationError):
            verify_np2test.verify(layout, output)


if __name__ == "__main__":
    unittest.main()
