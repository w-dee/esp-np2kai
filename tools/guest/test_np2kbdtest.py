#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Pure structural tests for the NP2 keyboard fixture contract."""

from __future__ import annotations

import hashlib
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from build_np2kbdtest import load_layout


ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/guest/np2kbdtest"
LAYOUT = FIXTURE / "layout.json"


class KeyboardFixtureTests(unittest.TestCase):
    def test_layout_contract(self) -> None:
        layout = load_layout(LAYOUT)
        self.assertEqual(layout["control"]["physical_address"], 0x27FC0)
        self.assertEqual(layout["control"]["physical_address"] + 64, 0x28000)
        self.assertEqual(layout["result"]["physical_address"], 0x29000)
        self.assertEqual(layout["control"]["expected_make"], 0x1D)
        self.assertEqual(layout["control"]["expected_break"], 0x9D)

    def test_reproducible_build_and_zero_tail(self) -> None:
        builder = ROOT / "tools/guest/build_np2kbdtest.py"
        with tempfile.TemporaryDirectory(prefix="np2kbdtest-test-") as directory:
            first = Path(directory) / "first.image"
            second = Path(directory) / "second.image"
            subprocess.run(["python3", str(builder), "--layout", str(LAYOUT), "--output", str(first)], check=True)
            subprocess.run(["python3", str(builder), "--layout", str(LAYOUT), "--output", str(second)], check=True)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            self.assertEqual(len(first.read_bytes()), 1261568)
            self.assertEqual(hashlib.sha256(first.read_bytes()).digest(), hashlib.sha256(second.read_bytes()).digest())
            self.assertEqual(first.read_bytes()[510:512], b"\x55\xaa")
            self.assertEqual(first.read_bytes()[1022:1024], b"\x55\xaa")
            self.assertEqual(any(first.read_bytes()[1024:]), False)


if __name__ == "__main__":
    unittest.main()
