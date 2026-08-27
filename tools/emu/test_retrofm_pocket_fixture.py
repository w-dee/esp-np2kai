#!/usr/bin/env python3
"""Offline provenance and fail-closed curation tests for RetroFM."""

from __future__ import annotations

import copy
import json
import struct
import tempfile
import unittest
from pathlib import Path

from curate_retrofm_s98 import CurationError, curate_file
from validate_retrofm_pocket_provenance import validate_manifest


ROOT = Path(__file__).resolve().parents[2]
MANIFEST_PATH = ROOT / "tools" / "emu" / "retrofm_pocket_fixture_provenance.json"
CURATED_PATH = ROOT / "testdata" / "s98" / "retrofm-pocket-demo-strict.s98"


def _raw_from_curated() -> bytes:
    data = bytearray(CURATED_PATH.read_bytes())
    data[48:48] = b"\x00\x22\x00\x00\x27\x00\x00\x07\x3f"
    struct.pack_into("<I", data, 0x10, 3587)
    return bytes(data)


class RetroFmProvenanceTest(unittest.TestCase):
    def test_manifest_and_artifacts(self) -> None:
        validate_manifest(MANIFEST_PATH)

    def test_manifest_mutations_fail_closed(self) -> None:
        original = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        mutations = {
            ("upstream", "commit"): "0" * 40,
            ("upstream", "generator_sha256"): "0" * 64,
            ("source_vgm", "sha256"): "0" * 64,
            ("converter", "commit"): "0" * 40,
            ("converter", "script_sha256"): "0" * 64,
            ("raw_derived_s98", "sha256"): "0" * 64,
            ("raw_derived_s98", "bytes"): 3761,
            ("converter", "command"): "python3 unrelated-tool.py",
            ("strict_derivative", "sha256"): "0" * 64,
        }
        for path, value in mutations.items():
            mutated = copy.deepcopy(original)
            mutated[path[0]][path[1]] = value
            with self.subTest(path=path):
                with tempfile.TemporaryDirectory() as directory:
                    candidate = Path(directory) / "manifest.json"
                    candidate.write_text(json.dumps(mutated), encoding="utf-8")
                    with self.assertRaises(ValueError):
                        validate_manifest(candidate)

        for index, field, value in (
            (0, "write_index", 1), (0, "register", 0x23),
            (0, "value", 1), (0, "sync", 1), (0, "occurrence_count", 2),
        ):
            mutated = copy.deepcopy(original)
            mutated["curation"]["removed_writes"][index][field] = value
            with self.subTest(field=field):
                with tempfile.TemporaryDirectory() as directory:
                    candidate = Path(directory) / "manifest.json"
                    candidate.write_text(json.dumps(mutated), encoding="utf-8")
                    with self.assertRaises(ValueError):
                        validate_manifest(candidate)

    def test_curation_positive_and_negative_mutations(self) -> None:
        raw = _raw_from_curated()
        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            raw_path = directory_path / "raw.s98"
            raw_path.write_bytes(raw)
            output = directory_path / "curated.s98"
            curate_file(raw_path, output)
            self.assertEqual(output.read_bytes(), CURATED_PATH.read_bytes())

            mutations: dict[str, bytes] = {}
            changed = bytearray(raw)
            changed[50] = 1
            mutations["changed first value"] = bytes(changed)
            changed = bytearray(raw)
            changed[48:54] = changed[51:54] + changed[48:51]
            mutations["reordered specials"] = bytes(changed)
            for name, register, value in (
                ("extra 0x22", 0x22, 0), ("extra 0x27", 0x27, 0),
                ("extra 0x07", 0x07, 0x3F),
            ):
                changed = bytearray(raw)
                changed[60:60] = bytes((0, register, value))
                struct.pack_into("<I", changed, 0x10, 3590)
                mutations[name] = bytes(changed)
            for name, offset, value in (
                ("wrong clock", 0x24, 3993600),
                ("wrong timer", 0x08, 22050),
                ("wrong raw write count", 0x1C, 2),
            ):
                changed = bytearray(raw)
                struct.pack_into("<I", changed, offset, value)
                mutations[name] = bytes(changed)
            changed = bytearray(raw)
            changed[60 + 1] = 0x22
            mutations["unexpected unsupported register"] = bytes(changed)
            changed = bytearray(raw)
            changed[48:48] = b"\xFF"
            mutations["special moved after wait"] = bytes(changed)
            changed = bytearray(raw)
            changed[-1] ^= 1
            mutations["wrong raw SHA"] = bytes(changed)

            for name, invalid in mutations.items():
                with self.subTest(name=name):
                    invalid_path = directory_path / f"{len(name)}-invalid.s98"
                    invalid_path.write_bytes(invalid)
                    invalid_output = directory_path / f"{len(name)}-invalid.out.s98"
                    with self.assertRaises(CurationError):
                        curate_file(invalid_path, invalid_output)
                    self.assertFalse(invalid_output.exists())


if __name__ == "__main__":
    unittest.main()
