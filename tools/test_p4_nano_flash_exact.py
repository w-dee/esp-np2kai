#!/usr/bin/env python3
"""Host-only contract tests for the no-rebuild P4-NANO flash helper."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
import venv
from pathlib import Path
from unittest import mock


TOOLS_DEV = Path(__file__).resolve().parent / "dev"
sys.path.insert(0, str(TOOLS_DEV))
import p4_nano_flash_exact as exact  # noqa: E402
from p4_nano_flash_metadata import MetadataError, parse_flash_plan  # noqa: E402


HELPER = TOOLS_DEV / "p4-nano-flash-exact.sh"
VERIFY = TOOLS_DEV / "p4-nano-verify-app.sh"


class FlashHelperContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="p4-flash-fixture-")
        self.build = Path(self.temp.name)
        (self.build / "bootloader").mkdir()
        (self.build / "partition_table").mkdir()
        (self.build / "bootloader/bootloader.bin").write_bytes(b"BOOTLOADER")
        (self.build / "partition_table/partition-table.bin").write_bytes(b"PARTITION")
        (self.build / "esp_np2kai.bin").write_bytes(b"APPLICATION")
        (self.build / "esp_np2kai.elf").write_bytes(b"ELF")
        (self.build / "esp_np2kai.map").write_bytes(b"MAP")
        (self.build / ".p4-production-variant").write_text("p4-v1x\n", encoding="utf-8")
        (self.build / ".p4-production-board").write_text("p4-nano\n", encoding="utf-8")
        (self.build / "CMakeCache.txt").write_text(
            "IDF_TARGET:STRING=esp32p4\n"
            "P4_NANO_AUDIO_ONLY_BENCHMARK_PROFILE:UNINITIALIZED=1\n"
            "P4_NANO_AUDIO_OPT:STRING=o2\n"
            f"PYTHON:UNINITIALIZED={sys.executable}\n",
            encoding="utf-8",
        )
        self.metadata = {
            "write_flash_args": ["--flash_mode", "dio", "--flash_size", "8MB", "--flash_freq", "80m"],
            "flash_settings": {"flash_mode": "dio", "flash_size": "8MB", "flash_freq": "80m"},
            "flash_files": {
                "0x2000": "bootloader/bootloader.bin",
                "0x10000": "esp_np2kai.bin",
                "0x8000": "partition_table/partition-table.bin",
            },
            "bootloader": {"offset": "0x2000", "file": "bootloader/bootloader.bin"},
            "app": {"offset": "0x10000", "file": "esp_np2kai.bin"},
            "partition-table": {"offset": "0x8000", "file": "partition_table/partition-table.bin"},
            "extra_esptool_args": {"after": "hard_reset", "before": "default_reset", "chip": "esp32p4"},
        }
        self.write_metadata()

    def tearDown(self) -> None:
        self.temp.cleanup()

    def write_metadata(self) -> None:
        (self.build / "flasher_args.json").write_text(
            json.dumps(self.metadata, indent=2) + "\n", encoding="utf-8"
        )

    def run_helper(self, *args: str, helper: Path = HELPER) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        env.pop("P4_NANO_SERIAL", None)
        return subprocess.run(
            [str(helper), "--build-dir", str(self.build), *args],
            text=True,
            capture_output=True,
            env=env,
            check=False,
        )

    def test_valid_plan_and_identity(self) -> None:
        plan = parse_flash_plan(self.build, expected_variant="p4-v1x", expected_board="p4-nano")
        self.assertEqual(plan.app.offset, 0x10000)
        self.assertEqual(plan.app.path, (self.build / "esp_np2kai.bin").resolve())
        self.assertEqual(plan.app.byte_count, len(b"APPLICATION"))
        self.assertEqual(plan.audio_profile, "1")
        result = self.run_helper("--print-plan")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("P4_NANO_FLASH_EXACT_PLAN result=PASS", result.stdout)
        self.assertIn("P4_NANO_FLASH_EXACT_APP offset=0x10000", result.stdout)
        self.assertIn("audio_only=1 audio_opt=o2", result.stdout)

    def assert_plan_rejected(self) -> None:
        self.write_metadata()
        with self.assertRaises(MetadataError):
            parse_flash_plan(self.build)

    @staticmethod
    def create_venv(path: Path) -> tuple[Path, str, str]:
        venv.EnvBuilder(with_pip=False, symlinks=True).create(path)
        launcher = path / "bin" / "python"
        result = subprocess.run(
            [str(launcher), "-c", "import sys; print(sys.prefix); print(sys.base_prefix)"],
            capture_output=True,
            text=True,
            check=True,
        )
        prefix, base_prefix = result.stdout.splitlines()
        return launcher, prefix, base_prefix

    def test_venv_symlink_launcher_is_accepted_without_realpath_execution(self) -> None:
        launcher, prefix, base_prefix = self.create_venv(self.build / "venv-positive")
        self.assertNotEqual(os.path.realpath(launcher), str(launcher))
        selected = exact._validate_python_environment(
            str(launcher), prefix, base_prefix, launcher.parent.parent
        )
        self.assertEqual(selected, str(launcher))
        self.assertNotEqual(selected, os.path.realpath(launcher))

    def test_active_environment_returns_venv_launcher_and_probes_same_environment(self) -> None:
        launcher, prefix, base_prefix = self.create_venv(self.build / "venv-active")
        (self.build / "CMakeCache.txt").write_text(
            "IDF_TARGET:STRING=esp32p4\n"
            f"PYTHON:UNINITIALIZED={launcher}\n",
            encoding="utf-8",
        )

        def fake_run(command: list[str], **_: object) -> subprocess.CompletedProcess[str]:
            if "import esptool" in command:
                return subprocess.CompletedProcess(command, 0, "", "")
            return subprocess.CompletedProcess(command, 0, f"{prefix}\n{base_prefix}\n", "")

        with mock.patch.dict(
            os.environ,
            {
                "P4_NANO_SERIAL": "/dev/test",
                "IDF_PATH": str(self.build / "idf"),
                "IDF_PYTHON_ENV_PATH": str(launcher.parent.parent),
            },
            clear=False,
        ), mock.patch.object(exact.sys, "executable", str(launcher)), mock.patch.object(
            exact.sys, "prefix", prefix
        ), mock.patch.object(exact.sys, "base_prefix", base_prefix), mock.patch.object(
            exact.subprocess, "run", side_effect=fake_run
        ):
            (self.build / "idf/tools").mkdir(parents=True)
            selected, serial = exact._active_environment(parse_flash_plan(self.build))
        self.assertEqual(selected, str(launcher))
        self.assertNotEqual(selected, os.path.realpath(launcher))
        self.assertEqual(serial, "/dev/test")

    def test_system_python_rejected_even_when_realpath_matches_venv_launcher(self) -> None:
        launcher, prefix, base_prefix = self.create_venv(self.build / "venv-system-negative")
        with self.assertRaises(RuntimeError):
            exact._validate_python_environment(
                sys.executable, sys.prefix, sys.base_prefix, launcher.parent.parent
            )

    def test_wrong_venv_rejected(self) -> None:
        launcher_a, prefix_a, base_a = self.create_venv(self.build / "venv-a")
        launcher_b, _, _ = self.create_venv(self.build / "venv-b")
        with self.assertRaises(RuntimeError):
            exact._validate_python_environment(
                str(launcher_a), prefix_a, base_a, launcher_b.parent.parent
            )

    def test_missing_idf_python_environment_rejected(self) -> None:
        with self.assertRaises(RuntimeError):
            exact._validate_python_environment(
                sys.executable, sys.prefix, sys.base_prefix, self.build / "missing-venv"
            )

    def test_esptool_unavailable_rejected(self) -> None:
        launcher, prefix, base_prefix = self.create_venv(self.build / "venv-no-esptool")
        (self.build / "idf/tools").mkdir(parents=True)

        with mock.patch.dict(
            os.environ,
            {
                "P4_NANO_SERIAL": "/dev/test",
                "IDF_PATH": str(self.build / "idf"),
                "IDF_PYTHON_ENV_PATH": str(launcher.parent.parent),
            },
            clear=False,
        ), mock.patch.object(exact.sys, "executable", str(launcher)), mock.patch.object(
            exact.sys, "prefix", prefix
        ), mock.patch.object(exact.sys, "base_prefix", base_prefix), mock.patch.object(
            exact.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 1, "", "missing esptool"),
        ):
            with self.assertRaisesRegex(RuntimeError, "esptool is unavailable"):
                exact._active_environment(parse_flash_plan(self.build))

    def test_build_python_environment_mismatch_rejected(self) -> None:
        launcher, prefix, base_prefix = self.create_venv(self.build / "venv-build-mismatch")
        (self.build / "idf/tools").mkdir(parents=True)
        original_prefix, original_base = sys.prefix, sys.base_prefix
        (self.build / "CMakeCache.txt").write_text(
            "IDF_TARGET:STRING=esp32p4\n"
            f"PYTHON:UNINITIALIZED={sys.executable}\n",
            encoding="utf-8",
        )

        def fake_run(command: list[str], **_: object) -> subprocess.CompletedProcess[str]:
            if "import esptool" in command:
                return subprocess.CompletedProcess(command, 0, "", "")
            return subprocess.CompletedProcess(
                command, 0, f"{original_prefix}\n{original_base}\n", ""
            )

        with mock.patch.dict(
            os.environ,
            {
                "P4_NANO_SERIAL": "/dev/test",
                "IDF_PATH": str(self.build / "idf"),
                "IDF_PYTHON_ENV_PATH": str(launcher.parent.parent),
            },
            clear=False,
        ), mock.patch.object(exact.sys, "executable", str(launcher)), mock.patch.object(
            exact.sys, "prefix", prefix
        ), mock.patch.object(exact.sys, "base_prefix", base_prefix), mock.patch.object(
            exact.subprocess, "run", side_effect=fake_run
        ):
            with self.assertRaisesRegex(RuntimeError, "build Python does not match"):
                exact._active_environment(parse_flash_plan(self.build))

    def test_rejects_chip_mismatch(self) -> None:
        self.metadata["extra_esptool_args"]["chip"] = "esp32s3"
        self.assert_plan_rejected()

    def test_rejects_chip_case_mismatch(self) -> None:
        self.metadata["extra_esptool_args"]["chip"] = "ESP32P4"
        self.assert_plan_rejected()

    def test_rejects_force_write_argument(self) -> None:
        self.metadata["write_flash_args"].append("--force")
        self.assert_plan_rejected()

    def test_rejects_encrypt_write_argument(self) -> None:
        self.metadata["write_flash_args"].append("--encrypt")
        self.assert_plan_rejected()

    def test_rejects_unknown_write_argument(self) -> None:
        self.metadata["write_flash_args"].append("--made-up-option")
        self.assert_plan_rejected()

    def test_rejects_stray_write_positional(self) -> None:
        self.metadata["write_flash_args"].append("stray")
        self.assert_plan_rejected()

    def test_rejects_duplicate_permitted_write_argument(self) -> None:
        self.metadata["write_flash_args"][2:2] = ["--flash_size", "4MB"]
        self.assert_plan_rejected()

    def test_verify_app_uses_shared_parser(self) -> None:
        result = self.run_helper("--print-plan", helper=VERIFY)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("P4_NANO_FLASH_APP_IDENTITY", result.stdout)
        self.assertIn("hardware_touched=NO", result.stdout)

    def test_rejects_malformed_json(self) -> None:
        (self.build / "flasher_args.json").write_text("{", encoding="utf-8")
        with self.assertRaises(MetadataError):
            parse_flash_plan(self.build)

    def test_rejects_missing_app(self) -> None:
        self.metadata.pop("app")
        self.write_metadata()
        with self.assertRaises(MetadataError):
            parse_flash_plan(self.build)

    def test_rejects_ambiguous_app_offset(self) -> None:
        self.metadata["flash_files"]["65536"] = "esp_np2kai.bin"
        self.write_metadata()
        with self.assertRaises(MetadataError):
            parse_flash_plan(self.build)

    def test_rejects_duplicate_offset(self) -> None:
        self.metadata["flash_files"]["8192"] = "partition_table/partition-table.bin"
        self.write_metadata()
        with self.assertRaises(MetadataError):
            parse_flash_plan(self.build)

    def test_rejects_missing_payload(self) -> None:
        (self.build / "bootloader/bootloader.bin").unlink()
        with self.assertRaises(MetadataError):
            parse_flash_plan(self.build)

    def test_rejects_path_escape(self) -> None:
        self.metadata["app"]["file"] = "../outside.bin"
        self.metadata["flash_files"]["0x10000"] = "../outside.bin"
        self.write_metadata()
        with self.assertRaises(MetadataError):
            parse_flash_plan(self.build)

    def test_rejects_wrong_profile_markers(self) -> None:
        (self.build / ".p4-production-variant").write_text("p4-v3x\n", encoding="utf-8")
        result = self.run_helper("--print-plan")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("P4_NANO_FLASH_EXACT_PLAN result=FAIL", result.stderr)
        (self.build / ".p4-production-variant").write_text("p4-v1x\n", encoding="utf-8")
        (self.build / ".p4-production-board").write_text("generic\n", encoding="utf-8")
        with self.assertRaises(MetadataError):
            parse_flash_plan(self.build, expected_variant="p4-v1x", expected_board="p4-nano")

    def test_print_plan_never_requires_or_opens_serial(self) -> None:
        result = self.run_helper("--print-plan")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("hardware_touched=NO", result.stdout)
        self.assertNotIn("/dev/serial", result.stdout)

    def test_payload_change_detection(self) -> None:
        plan = parse_flash_plan(self.build)
        before = exact.snapshot_payloads(plan.payloads)
        (self.build / "esp_np2kai.bin").write_bytes(b"CHANGED")
        self.assertFalse(exact.payloads_stable(before, plan.payloads))

    def test_no_build_system_path_and_no_build_proof(self) -> None:
        source = (TOOLS_DEV / "p4_nano_flash_exact.py").read_text(encoding="utf-8")
        source += (HELPER).read_text(encoding="utf-8")
        for forbidden in ("idf.py", "ninja", "cmake", "build-production.sh"):
            self.assertNotIn(forbidden, source)
        ninja = self.build / "build.ninja"
        ninja.write_text("# deliberately newer than all payloads\n", encoding="utf-8")
        old_mtime = ninja.stat().st_mtime_ns
        time.sleep(0.01)
        result = self.run_helper("--print-plan")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(ninja.stat().st_mtime_ns, old_mtime)

    def test_esptool_command_preserves_generated_settings(self) -> None:
        plan = parse_flash_plan(self.build)
        command = exact._esptool_command(plan, "/idfp/python", "/dev/test")
        self.assertEqual(command[:15], [
            "/idfp/python", "-m", "esptool", "--chip", "esp32p4", "-p", "/dev/test",
            "-b", "1500000", "--before", "default_reset", "--after", "hard_reset", "write_flash",
            "--flash_mode",
        ])
        self.assertEqual(command[15:20], ["dio", "--flash_size", "8MB", "--flash_freq", "80m"])
        self.assertIn("0x2000", command)
        self.assertIn("0x8000", command)
        self.assertIn("0x10000", command)


if __name__ == "__main__":
    unittest.main()
