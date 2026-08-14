#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Standard-library tests for the NP2TEST foundation tools."""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from unittest import mock
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
        manifest = json.loads(output_a.with_name("a.image.manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["toolchain"]["assembler"]["version"], "2.16.01")
        self.assertEqual(manifest["toolchain"]["python"]["version_major"], 3)
        self.assertEqual(manifest["toolchain"]["python"]["version_minor"], 12)

    def test_wrong_geometry_is_rejected(self) -> None:
        layout = self.copy_layout(lambda value: value["image"]["geometry"].update(heads=1))
        with self.assertRaises(build_np2test.LayoutError):
            build_np2test.load_layout(layout)

    def test_selected_extension_is_rejected_until_review(self) -> None:
        layout = self.copy_layout(lambda value: value["image"].update(extension=".hdm"))
        with self.assertRaises(build_np2test.LayoutError):
            build_np2test.load_layout(layout)

    def test_invalid_result_address_is_rejected(self) -> None:
        layout = self.copy_layout(lambda value: value["result"].update(physical_address=0x29001))
        with self.assertRaises(build_np2test.LayoutError):
            build_np2test.load_layout(layout)

    def test_overlapping_owned_regions_are_rejected(self) -> None:
        layout = self.copy_layout(lambda value: value["memory"]["owned_regions"][2].update(start=0x27F00))
        with self.assertRaises(build_np2test.LayoutError):
            build_np2test.load_layout(layout)

    def test_invalid_result_field_offset_is_rejected(self) -> None:
        layout = self.copy_layout(lambda value: value["result"]["wire"]["fields"][0].update(offset=1))
        with self.assertRaises(build_np2test.LayoutError):
            build_np2test.load_layout(layout)

    def test_invalid_checksum_coverage_is_rejected(self) -> None:
        layout = self.copy_layout(lambda value: value["result"]["wire"]["checksum"]["coverage_ranges"][0].update(end_exclusive=119))
        with self.assertRaises(build_np2test.LayoutError):
            build_np2test.load_layout(layout)

    def test_invalid_state_encoding_is_rejected(self) -> None:
        layout = self.copy_layout(lambda value: value["result"]["wire"]["state"]["values"].update(PASS=4))
        with self.assertRaises(build_np2test.LayoutError):
            build_np2test.load_layout(layout)

    def test_wrong_signature_is_rejected(self) -> None:
        layout = self.copy_layout(lambda value: value["ipl"]["signatures"][0].update(bytes="aa55"))
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

    def test_wrong_image_size_is_rejected(self) -> None:
        layout = self.copy_layout()
        output = self.work / "short.image"
        build_np2test.build(layout, output)
        output.write_bytes(output.read_bytes()[:-1])
        with self.assertRaises(verify_np2test.VerificationError):
            verify_np2test.verify(layout, output)

    def test_checksum_mismatch_is_rejected(self) -> None:
        layout = self.copy_layout()
        output = self.work / "checksum.image"
        build_np2test.build(layout, output)
        checksum = output.with_name("checksum.image.sha256")
        checksum.write_text("0" * 64 + "  checksum.image\n", encoding="ascii")
        with self.assertRaises(verify_np2test.VerificationError):
            verify_np2test.verify(layout, output, checksum)

    def test_artifact_verification_does_not_require_nasm(self) -> None:
        layout = self.copy_layout()
        output = self.work / "without-nasm.image"
        digest = build_np2test.build(layout, output)
        with mock.patch.object(build_np2test, "check_nasm", side_effect=AssertionError("verifier invoked NASM")):
            self.assertEqual(verify_np2test.verify(layout, output), digest)


if __name__ == "__main__":
    unittest.main()
