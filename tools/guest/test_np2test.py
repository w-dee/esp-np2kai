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

    def test_crc32_iso_hdlc_check_vector_and_reflection_are_explicit(self) -> None:
        layout = build_np2test.load_layout(LAYOUT_PATH)
        checksum = layout["result"]["wire"]["checksum"]
        self.assertTrue(checksum["refin"])
        self.assertTrue(checksum["refout"])
        self.assertEqual(checksum["polynomial"], "0x04c11db7")
        self.assertEqual(checksum["reflected_polynomial"], "0xedb88320")
        self.assertEqual(build_np2test.crc32_iso_hdlc(b"123456789"), 0xcbf43926)

    def test_crc_reflection_parameter_is_validated(self) -> None:
        layout = self.copy_layout(lambda value: value["result"]["wire"]["checksum"].update(refin=False))
        with self.assertRaises(build_np2test.LayoutError):
            build_np2test.load_layout(layout)

    def test_state_is_outside_crc_and_live_polling_is_state_only(self) -> None:
        layout = build_np2test.load_layout(LAYOUT_PATH)
        fields = {field["name"]: field for field in layout["result"]["wire"]["fields"]}
        self.assertEqual(fields["state"]["coverage"], "excluded")
        self.assertEqual(fields["state"]["offset"], 124)
        self.assertEqual(layout["result"]["wire"]["checksum"]["coverage_ranges"], [{"start": 0, "end_exclusive": 120}])
        live = layout["result"]["wire"]["state"]["live_polling"]
        self.assertEqual(live["while_running"], "poll-state-only")
        self.assertEqual(live["body_validation"], "deferred-until-terminal-state")
        self.assertEqual(live["final_states_immutable"], ["PASS", "FAIL"])
        invalid = self.copy_layout(lambda value: value["result"]["wire"]["fields"][16].update(coverage="included"))
        with self.assertRaises(build_np2test.LayoutError):
            build_np2test.load_layout(invalid)

    def test_old_itf_rom_exclusion_is_rejected(self) -> None:
        def old_map(value):
            value["memory"]["excluded_ranges"][-1].update(name="pc98-itf-rom", start=0xF8000, size_bytes=0x8000)

        layout = self.copy_layout(old_map)
        with self.assertRaises(build_np2test.LayoutError):
            build_np2test.load_layout(layout)

    def test_firmware_exclusion_and_extended_itf_map_are_exact(self) -> None:
        layout = build_np2test.load_layout(LAYOUT_PATH)
        excluded = {item["name"]: item for item in layout["memory"]["excluded_ranges"]}
        self.assertEqual((excluded["pc98-bios-firmware"]["start"], excluded["pc98-bios-firmware"]["size_bytes"]), (0xE8000, 0x18000))
        extended = layout["memory"]["extended_excluded_ranges"][0]
        self.assertEqual((extended["start"], extended["size_bytes"]), (0x1F8000, 0x8000))
        self.assertFalse(extended["within_validator_address_space"])

    def test_owned_regions_remain_outside_all_exclusions(self) -> None:
        layout = build_np2test.load_layout(LAYOUT_PATH)
        exclusions = [(item["start"], item["start"] + item["size_bytes"]) for item in layout["memory"]["excluded_ranges"]]
        for region in layout["memory"]["owned_regions"]:
            start = region["start"]
            end = start + region["size_bytes"]
            self.assertFalse(any(start < excluded_end and excluded_start < end for excluded_start, excluded_end in exclusions), region["name"])

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
