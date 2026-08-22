#!/usr/bin/env python3
"""Synthetic tests for the observation-only formal NP2TEST UART harness."""

from __future__ import annotations

import unittest

from np2test_physical import EXPECTED_FIXTURE_PATH, parse_log


PASS_LOG = """\
I (1) boot: unrelated ESP-IDF output
ESP-NP2KAI HELLO WORLD OK
NP2TEST_SD_MOUNTED path=/sdcard fixture=/sdcard/files/np2-fixtures/np2test-a2-20260821-r1-3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3.hdm
NP2TEST profile=formal formal_extmem=13 effective_extmem=13
NP2TEST_VFS_FIXTURE_VERIFY result=PASS logical=./np2test-fd1232.hdm physical=/sdcard/files/np2-fixtures/np2test-a2-20260821-r1-3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3.hdm size=1261568 sha256=3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3 read_only=1
NP2TEST_DISK_SOURCE kind=vfs logical=./np2test-fd1232.hdm physical=/sdcard/files/np2-fixtures/np2test-a2-20260821-r1-3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3.hdm
NP2TEST_MEMORY extmem_mb=13 actual_bytes=13631488 ptr_external=1 psram_size=33554432 free_spiram=1 largest_spiram=1
NP2TEST_FDD_READY path=./np2test-fd1232.hdm tracks=154 sectors=8 n=3 disktype=2 read_only=1
NP2TEST_PASS completed=13 passed=13 failed=0 stored_crc=0x58f5b827
NP2TEST_RESULT=PASS
"""


class NP2TestPhysicalHarnessTests(unittest.TestCase):
    def assert_rejected(self, log: str) -> None:
        report = parse_log(log)
        self.assertFalse(report.ok, report)
        self.assertTrue(report.reasons, report)

    def test_complete_pass_sequence(self) -> None:
        report = parse_log(PASS_LOG)
        self.assertTrue(report.ok, report)
        self.assertEqual(report.terminal_result, "PASS")

    def test_wrong_crc_rejected(self) -> None:
        self.assert_rejected(PASS_LOG.replace("0x58f5b827", "0x00000000"))

    def test_twelve_of_thirteen_rejected(self) -> None:
        self.assert_rejected(PASS_LOG.replace("completed=13 passed=13", "completed=12 passed=12"))

    def test_guest_failure_rejected(self) -> None:
        self.assert_rejected(
            PASS_LOG.replace(
                "NP2TEST_PASS completed=13 passed=13 failed=0 stored_crc=0x58f5b827\nNP2TEST_RESULT=PASS",
                "NP2TEST_FAIL first_failed_id=0x0101 completed=13 passed=12 failed=1 diagnostic_length=0 diagnostic_hex=\nNP2TEST_RESULT=FAIL",
            )
        )

    def test_sd_mount_failure_rejected(self) -> None:
        self.assert_rejected(
            "NP2TEST_SD_MOUNT=FAIL reason=ESP_ERR_TIMEOUT\n"
            "NP2TEST_RESULT=HARNESS_ERROR reason=sd_mount\n"
        )

    def test_fixture_integrity_failure_rejected(self) -> None:
        self.assert_rejected(
            PASS_LOG.replace(
                "NP2TEST_VFS_FIXTURE_VERIFY result=PASS logical=./np2test-fd1232.hdm physical=/sdcard/files/np2-fixtures/np2test-a2-20260821-r1-3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3.hdm size=1261568 sha256=3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3 read_only=1",
                "NP2TEST_VFS_FIXTURE_VERIFY result=FAIL reason=sha256_mismatch physical=/sdcard/files/np2-fixtures/np2test-a2-20260821-r1-3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3.hdm",
            )
        )

    def test_extmem8_rejected(self) -> None:
        self.assert_rejected(
            PASS_LOG.replace("effective_extmem=13", "effective_extmem=8")
            .replace("extmem_mb=13", "extmem_mb=8")
        )

    def test_non_external_memory_rejected(self) -> None:
        self.assert_rejected(PASS_LOG.replace("ptr_external=1", "ptr_external=0"))

    def test_writable_fdd_rejected(self) -> None:
        self.assert_rejected(PASS_LOG.replace("read_only=1\nNP2TEST_PASS", "read_only=0\nNP2TEST_PASS"))

    def test_missing_final_marker_rejected(self) -> None:
        self.assert_rejected(PASS_LOG.replace("NP2TEST_RESULT=PASS\n", ""))

    def test_running_timeout_rejected(self) -> None:
        self.assert_rejected(PASS_LOG.replace("NP2TEST_PASS completed=13 passed=13 failed=0 stored_crc=0x58f5b827\nNP2TEST_RESULT=PASS", "NP2TEST_RESULT=RUNNING_TIMEOUT"))

    def test_unrelated_and_repeated_lines_are_ignored(self) -> None:
        log = PASS_LOG.replace(
            "NP2TEST profile=formal formal_extmem=13 effective_extmem=13\n",
            "I (2) sdmmc: card ready\n"
            "NP2TEST profile=formal formal_extmem=13 effective_extmem=13\n"
            "I (3) storage: unchanged\n"
            "NP2TEST profile=formal formal_extmem=13 effective_extmem=13\n",
        )
        report = parse_log(log)
        self.assertTrue(report.ok, report)

    def test_truncated_session_rejected(self) -> None:
        self.assert_rejected("\n".join(PASS_LOG.splitlines()[:3]) + "\n")

    def test_default_fixture_path_is_canonical(self) -> None:
        self.assertIn(EXPECTED_FIXTURE_PATH, PASS_LOG)


if __name__ == "__main__":
    unittest.main()
