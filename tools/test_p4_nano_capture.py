#!/usr/bin/env python3
"""Host-only tests for the durable P4-NANO UART capture core."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools/dev/p4_nano_capture.py"
SPEC = importlib.util.spec_from_file_location("p4_nano_capture", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
CAPTURE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = CAPTURE
SPEC.loader.exec_module(CAPTURE)


class FakeClock:
    def __init__(self) -> None:
        self.value = 0.0

    def __call__(self) -> float:
        return self.value

    def advance(self, seconds: float) -> None:
        self.value += seconds


class RecordingSerial:
    """Small pyserial model for deterministic DTR/RTS sequence tests."""

    def __init__(self) -> None:
        self.rts = True
        self.dtr = True
        self.events: list[tuple[object, ...]] = [
            ("initial", self.rts, self.dtr),
        ]
        self.is_open = False

    def open(self) -> None:
        self.is_open = True
        self.events.append(("open", self.rts, self.dtr))

    def setRTS(self, value: bool) -> None:
        self.rts = value
        self.events.append(("setRTS", value))

    def setDTR(self, value: bool) -> None:
        self.dtr = value
        self.events.append(("setDTR", value))

    def record_sleep(self, duration: float) -> None:
        self.events.append(("sleep", duration))

    def close(self) -> None:
        self.is_open = False
        self.events.append(("close",))


def make_session(
    root: Path,
    clock: FakeClock,
    timeout: float = 360.0,
    *,
    post_terminal_drain: float = CAPTURE.DEFAULT_POST_TERMINAL_DRAIN_SECONDS,
    pass_marker: str | bytes = CAPTURE.PASS_MARKER,
    fail_marker: str | bytes = CAPTURE.FAIL_MARKER,
):
    session = CAPTURE.CaptureSession(
        root / "capture.raw",
        root / "capture.status.json",
        hard_timeout_seconds=timeout,
        post_terminal_drain_seconds=post_terminal_drain,
        clock=clock,
        terminal_pass_marker=pass_marker,
        terminal_fail_marker=fail_marker,
    )
    session.prepare()
    return session


def wait_for_raw_prefix(path: Path, prefix: bytes, timeout: float = 5.0) -> None:
    """Wait until the child has armed capture and durably written its prefix."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if path.read_bytes() == prefix:
                return
        except FileNotFoundError:
            pass
        time.sleep(0.01)
    raise AssertionError("capture child did not durably write the expected prefix")


class CaptureHarnessTests(unittest.TestCase):
    def test_monitor_equivalent_reset_sequence(self) -> None:
        serial_port = RecordingSerial()
        serial_port.open()
        CAPTURE.normalize_after_open(serial_port)
        with mock.patch.object(CAPTURE.time, "sleep", side_effect=serial_port.record_sleep):
            CAPTURE.hard_reset(serial_port)

        expected = [
            ("initial", True, True),
            ("open", True, True),
            ("setRTS", False),
            ("setDTR", False),
            ("setRTS", True),
            ("sleep", CAPTURE.RESET_LOW_SECONDS),
            ("setRTS", False),
        ]
        self.assertEqual(serial_port.events, expected)

    def test_reset_final_line_state_is_deasserted(self) -> None:
        serial_port = RecordingSerial()
        serial_port.open()
        CAPTURE.normalize_after_open(serial_port)
        with mock.patch.object(CAPTURE.time, "sleep", side_effect=serial_port.record_sleep):
            CAPTURE.hard_reset(serial_port)
        self.assertFalse(serial_port.rts)
        self.assertFalse(serial_port.dtr)

    def test_dtr_bootstrap_guard(self) -> None:
        serial_port = RecordingSerial()
        serial_port.open()
        CAPTURE.normalize_after_open(serial_port)
        with mock.patch.object(CAPTURE.time, "sleep", side_effect=serial_port.record_sleep):
            CAPTURE.hard_reset(serial_port)
        dtr_writes = [event[1] for event in serial_port.events if event[0] == "setDTR"]
        self.assertTrue(dtr_writes)
        self.assertTrue(all(value is False for value in dtr_writes))
        self.assertFalse(serial_port.dtr)

    def test_pre_reset_terminal_marker_does_not_end_capture(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            clock = FakeClock()
            session = make_session(root, clock)
            marker = b"P4_AUDIO_ONLY_BENCHMARK_RESULT=PASS\n"
            session.feed(marker)
            self.assertIsNone(session.terminal_marker)
            session.issue_reset(lambda: None)
            session.feed(marker)
            self.assertEqual(session.finish()["exit_reason"], "TERMINAL_PASS")

    def test_chunk_boundaries_and_terminal_pass(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            clock = FakeClock()
            session = make_session(root, clock)
            pre_reset = b"\x00setup\x1b[32m\r\n"
            session.feed(pre_reset)
            session.issue_reset(lambda: None)
            source = (
                b"P4_AUDIO_RESULT workload=RETROFM identity=PASS\n"
                b"P4_AUDIO_A2_RESULT workload=RETROFM mode=TIMING\n"
                b"P4_AUDIO_ONLY_BENCHMARK_RESULT=PASS\r\n"
            )
            for index in range(0, len(source), 3):
                session.feed(source[index : index + 3])
            status = session.finish()
            self.assertEqual((root / "capture.raw").read_bytes(), pre_reset + source)
            self.assertEqual(status["terminal_status"], "PASS")
            self.assertEqual(status["exit_reason"], "TERMINAL_PASS")
            self.assertEqual(status["reset_count"], 1)
            self.assertEqual(status["reset_byte_offset"], len(pre_reset))
            terminal_start = len(pre_reset) + source.index(
                b"P4_AUDIO_ONLY_BENCHMARK_RESULT=PASS"
            )
            self.assertEqual(status["schema_version"], 2)
            self.assertEqual(status["terminal_line_start_offset"], terminal_start)
            self.assertEqual(status["terminal_line_end_offset"], len(pre_reset + source))
            self.assertTrue(status["terminal_line_complete"])
            self.assertTrue(status["raw_final_line_complete"])

    def test_terminal_offsets_are_chunk_boundary_invariant(self) -> None:
        marker = b"P4_AUDIO_ONLY_BENCHMARK_RESULT=PASS"
        pre_reset = b"pre-reset\r\n"
        before = b"diagnostic\r\n"
        tail = b"post-terminal " + b"x" * 8192 + b"\r\n"
        source = before + marker + b"\r\n" + tail
        marker_start = len(before)
        marker_end = marker_start + len(marker) + 2
        variants = (
            (source,),
            (source[: marker_start + len(marker)],
             source[marker_start + len(marker) :]),
            (source[: marker_end - 1], source[marker_end - 1 :]),
            (source[:marker_end], source[marker_end:]),
            tuple(source[index : index + 1] for index in range(len(source))),
        )
        observed: set[tuple[object, ...]] = set()
        for chunks in variants:
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                session = make_session(root, FakeClock())
                session.feed(pre_reset)
                session.issue_reset(lambda: None)
                for chunk in chunks:
                    session.feed(chunk)
                status = session.finish()
                self.assertEqual((root / "capture.raw").read_bytes(), pre_reset + source)
                observed.add((
                    status["terminal_marker"],
                    status["terminal_line_start_offset"],
                    status["terminal_line_end_offset"],
                    status["terminal_line_complete"],
                ))
        self.assertEqual(observed, {
            ("PASS", len(pre_reset) + marker_start,
             len(pre_reset) + marker_end, True),
        })

    def test_terminal_complete_is_independent_of_partial_raw_tail(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            session = make_session(root, FakeClock())
            session.issue_reset(lambda: None)
            marker = b"P4_AUDIO_ONLY_BENCHMARK_RESULT=PASS\r\n"
            session.feed(marker + b"partial benign tail")
            status = session.finish()
            self.assertTrue(status["terminal_line_complete"])
            self.assertEqual(status["terminal_line_end_offset"], len(marker))
            self.assertFalse(status["raw_final_line_complete"])
            self.assertNotIn("final_line_complete", status)

    def test_crlf_lf_nul_ansi_and_intermediate_markers(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            clock = FakeClock()
            session = make_session(root, clock)
            session.issue_reset(lambda: None)
            data = (
                b"\x00\x1b[31mP4_AUDIO_RESULT workload=RETROFM\x1b[0m\r\n"
                b"P4_AUDIO_A2_RESULT workload=RETROFM mode=CORRECTNESS\n"
                b"P4_AUDIO_EMU_VALIDATION=PASS\n"
                b"P4_AUDIO_ONLY_BENCHMARK_RESULT=PASS\n"
            )
            for byte in data:
                session.feed(bytes((byte,)))
            status = session.finish()
            self.assertEqual((root / "capture.raw").read_bytes(), data)
            self.assertEqual(status["exit_reason"], "TERMINAL_PASS")

    def test_terminal_fail_is_target_result_not_host_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            clock = FakeClock()
            session = make_session(root, clock)
            session.issue_reset(lambda: None)
            session.feed(b"P4_AUDIO_ONLY_BENCHMARK_RESULT=FAIL\n")
            status = session.finish()
            self.assertEqual(status["terminal_status"], "FAIL")
            self.assertEqual(status["exit_reason"], "TERMINAL_FAIL")
            self.assertIsNone(status["serial_error"])

    def test_custom_pass_lf_and_crlf_after_reset(self) -> None:
        pass_marker = "AUDIO86_P4_RESULT=PASS"
        fail_marker = "AUDIO86_P4_RESULT=FAIL"
        for suffix in (b"\n", b"\r\n"):
            with self.subTest(suffix=suffix), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                clock = FakeClock()
                session = make_session(
                    root,
                    clock,
                    pass_marker=pass_marker,
                    fail_marker=fail_marker,
                )
                session.feed(pass_marker.encode("ascii") + suffix)
                self.assertIsNone(session.terminal_marker)
                session.issue_reset(lambda: None)
                encoded = pass_marker.encode("ascii") + suffix
                for offset in range(0, len(encoded), 2):
                    session.feed(encoded[offset : offset + 2])
                status = session.finish()
                self.assertEqual(status["terminal_status"], "PASS")
                self.assertEqual(status["exit_reason"], "TERMINAL_PASS")
                self.assertEqual(status["terminal_pass_marker_config"], pass_marker)
                self.assertEqual(status["terminal_fail_marker_config"], fail_marker)

    def test_custom_fail_is_target_result_not_host_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            clock = FakeClock()
            session = make_session(
                root,
                clock,
                pass_marker="AUDIO86_P4_RESULT=PASS",
                fail_marker="AUDIO86_P4_RESULT=FAIL",
            )
            session.issue_reset(lambda: None)
            session.feed(b"AUDIO86_P4_RESULT=FAIL\n")
            status = session.finish()
            self.assertEqual(status["terminal_status"], "FAIL")
            self.assertEqual(status["exit_reason"], "TERMINAL_FAIL")
            self.assertIsNone(status["serial_error"])

    def test_custom_marker_exclusivity(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            custom = make_session(
                root,
                FakeClock(),
                pass_marker="AUDIO86_P4_RESULT=PASS",
                fail_marker="AUDIO86_P4_RESULT=FAIL",
            )
            custom.issue_reset(lambda: None)
            self.assertIsNone(custom.feed(b"P4_AUDIO_ONLY_BENCHMARK_RESULT=PASS\n"))
            custom.feed(b"AUDIO86_P4_RESULT=PASS\n")
            self.assertEqual(custom.finish()["exit_reason"], "TERMINAL_PASS")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            legacy = make_session(root, FakeClock())
            legacy.issue_reset(lambda: None)
            self.assertIsNone(legacy.feed(b"AUDIO86_P4_RESULT=PASS\n"))
            legacy.feed(b"P4_AUDIO_ONLY_BENCHMARK_RESULT=PASS\n")
            self.assertEqual(legacy.finish()["exit_reason"], "TERMINAL_PASS")

    def test_custom_near_matches_require_exact_line(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            clock = FakeClock()
            session = make_session(
                root,
                clock,
                pass_marker="AUDIO86_P4_RESULT=PASS",
                fail_marker="AUDIO86_P4_RESULT=FAIL",
            )
            session.issue_reset(lambda: None)
            near_matches = (
                b"xAUDIO86_P4_RESULT=PASS\n",
                b"AUDIO86_P4_RESULT=PASSx\n",
                b"AUDIO86_P4_RESULT=PASS extra\n",
                b" AUDIO86_P4_RESULT=PASS\n",
                b"\x1b[32mAUDIO86_P4_RESULT=PASS\x1b[0m\n",
            )
            for line in near_matches:
                self.assertIsNone(session.feed(line))
            session.feed(b"AUDIO86_P4_RESULT=PASS\n")
            self.assertEqual(session.finish()["exit_reason"], "TERMINAL_PASS")

    def test_invalid_marker_rejected_before_capture_side_effects(self) -> None:
        invalid_pairs = (
            ("", "AUDIO86_P4_RESULT=FAIL"),
            ("AUDIO86_P4_RESULT=PASS", ""),
            ("AUDIO86_P4_RESULT=PASS\n", "AUDIO86_P4_RESULT=FAIL"),
            ("AUDIO86_P4_RESULT=PASS", "AUDIO86_P4_RESULT=FAIL\r"),
            ("AUDIO86_P4_RÉSULT=PASS", "AUDIO86_P4_RESULT=FAIL"),
            ("AUDIO86_P4_RESULT=PASS", "AUDIO86_P4_RESULT=PASS"),
        )
        for pass_marker, fail_marker in invalid_pairs:
            with self.subTest(pass_marker=pass_marker, fail_marker=fail_marker), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                with mock.patch.object(CAPTURE, "_open_serial") as open_serial:
                    with self.assertRaises((TypeError, ValueError)):
                        CAPTURE.run_serial_capture(
                            port="synthetic",
                            raw_path=root / "capture.raw",
                            status_path=root / "capture.status.json",
                            terminal_pass_marker=pass_marker,
                            terminal_fail_marker=fail_marker,
                        )
                open_serial.assert_not_called()
                self.assertFalse((root / "capture.raw").exists())
                self.assertFalse((root / "capture.status.json").exists())

    def test_cli_marker_contract(self) -> None:
        parser = CAPTURE.build_arg_parser()
        defaults = parser.parse_args(["--port", "synthetic", "--raw", "raw", "--status", "status"])
        self.assertEqual(defaults.terminal_pass_marker, "P4_AUDIO_ONLY_BENCHMARK_RESULT=PASS")
        self.assertEqual(defaults.terminal_fail_marker, "P4_AUDIO_ONLY_BENCHMARK_RESULT=FAIL")
        custom = parser.parse_args(
            [
                "--port",
                "synthetic",
                "--raw",
                "raw",
                "--status",
                "status",
                "--terminal-pass-marker",
                "AUDIO86_P4_RESULT=PASS",
                "--terminal-fail-marker",
                "AUDIO86_P4_RESULT=FAIL",
            ]
        )
        self.assertEqual(custom.terminal_pass_marker, "AUDIO86_P4_RESULT=PASS")
        self.assertEqual(custom.terminal_fail_marker, "AUDIO86_P4_RESULT=FAIL")

    def test_seventy_second_silence_has_no_idle_timeout(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            clock = FakeClock()
            session = make_session(root, clock)
            session.issue_reset(lambda: None)
            clock.advance(70.0)
            self.assertFalse(session.check_hard_timeout())
            session.feed(b"after-silence\nP4_AUDIO_ONLY_BENCHMARK_RESULT=PASS\n")
            self.assertEqual(session.finish()["exit_reason"], "TERMINAL_PASS")

    def test_forced_timeout_preserves_exact_partial_prefix(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            clock = FakeClock()
            session = make_session(root, clock)
            session.issue_reset(lambda: None)
            prefix = b"known\x00prefix\nunterminated terminal prefix: P4_AUDIO_ONLY_"
            session.feed(prefix)
            clock.advance(360.0)
            self.assertTrue(session.check_hard_timeout())
            status = session.finish(CAPTURE.ExitReason.HARD_TIMEOUT)
            raw = (root / "capture.raw").read_bytes()
            self.assertEqual(raw, prefix)
            self.assertGreater(len(raw), 0)
            self.assertEqual(status["raw_sha256"], hashlib.sha256(prefix).hexdigest())
            self.assertFalse(status["terminal_line_complete"])
            self.assertFalse(status["raw_final_line_complete"])
            self.assertIsNone(status["terminal_line_start_offset"])
            self.assertIsNone(status["terminal_line_end_offset"])
            self.assertEqual(status["exit_reason"], "HARD_TIMEOUT")

    def test_abrupt_source_termination_preserves_prefix(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            clock = FakeClock()
            session = make_session(root, clock)
            session.issue_reset(lambda: None)
            prefix = b"source-prefix\n"
            session.feed(prefix)
            status = session.finish(CAPTURE.ExitReason.SERIAL_ERROR, serial_error="source ended")
            self.assertEqual((root / "capture.raw").read_bytes(), prefix)
            self.assertEqual(status["exit_reason"], "SERIAL_ERROR")
            self.assertEqual(status["serial_error"], "source ended")

    def test_parser_exception_preserves_prefix(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            clock = FakeClock()
            session = make_session(root, clock)
            session.issue_reset(lambda: None)
            prefix = b"parser-prefix\x00"

            def fail_parser(_data: bytes, _raw_start_offset: int) -> None:
                raise ValueError("synthetic parser failure")

            session.detector.feed = fail_parser  # type: ignore[method-assign]
            with self.assertRaises(ValueError):
                session.feed(prefix)
            status = session.finish(CAPTURE.ExitReason.INTERNAL_ERROR, serial_error="parser failure")
            self.assertEqual((root / "capture.raw").read_bytes(), prefix)
            self.assertEqual(status["exit_reason"], "INTERNAL_ERROR")

    def test_reset_epoch_and_single_reset(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            clock = FakeClock()
            session = make_session(root, clock)
            clock.advance(100.0)
            session.issue_reset(lambda: None)
            self.assertEqual(session.reset_monotonic, 100.0)
            self.assertFalse(session.check_hard_timeout())
            clock.advance(359.0)
            self.assertFalse(session.check_hard_timeout())
            clock.advance(1.0)
            self.assertTrue(session.check_hard_timeout())
            with self.assertRaises(CAPTURE.CaptureStateError):
                session.issue_reset(lambda: None)
            status = session.finish(CAPTURE.ExitReason.HARD_TIMEOUT)
            self.assertEqual(status["reset_count"], 1)

    def test_exclusive_create_does_not_overwrite(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "capture.raw").write_bytes(b"immutable")
            clock = FakeClock()
            with self.assertRaises(FileExistsError):
                make_session(root, clock)
            self.assertEqual((root / "capture.raw").read_bytes(), b"immutable")

    def test_sigterm_prefix_durability(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "capture.raw"
            status = root / "capture.status.json"
            prefix = b"SIGTERM-PREFIX\n"
            code = "\n".join([
                "import sys, time",
                f"sys.path.insert(0, {str(ROOT)!r})",
                "from pathlib import Path",
                "from tools.dev import p4_nano_capture as c",
                "class FakeSerial:",
                "    def __init__(self): self.sent = False; self.timeout = 0.05; self.dtr = False",
                "    def setRTS(self, value): pass",
                "    def setDTR(self, value): self.dtr = value",
                "    def read(self, size):",
                "        if not self.sent: self.sent = True; return " + repr(prefix),
                "        time.sleep(0.05); return b''",
                "    def close(self): pass",
                "c._open_serial = lambda *args: FakeSerial()",
                "c.hard_reset = lambda port: None",
                "raise SystemExit(c.run_serial_capture(port='synthetic', raw_path=Path(" + repr(str(raw)) + "), status_path=Path(" + repr(str(status)) + "), hard_timeout_seconds=30, read_timeout_seconds=0.05, post_terminal_drain_seconds=0.2))",
            ])
            process = subprocess.Popen([sys.executable, "-c", code])
            try:
                wait_for_raw_prefix(raw, prefix)
                process.send_signal(signal.SIGTERM)
                self.assertEqual(process.wait(timeout=5), 2)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
            self.assertEqual(raw.read_bytes(), prefix)
            self.assertEqual(json.loads(status.read_text())["exit_reason"], "CONTROL_ERROR")

    def test_sigkill_prefix_durability(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "capture.raw"
            status = root / "capture.status.json"
            prefix = b"SIGKILL-PREFIX\n"
            code = "\n".join([
                "import sys, time",
                f"sys.path.insert(0, {str(ROOT)!r})",
                "from pathlib import Path",
                "from tools.dev import p4_nano_capture as c",
                "class FakeSerial:",
                "    def __init__(self): self.sent = False; self.timeout = 0.05; self.dtr = False",
                "    def setRTS(self, value): pass",
                "    def setDTR(self, value): self.dtr = value",
                "    def read(self, size):",
                "        if not self.sent: self.sent = True; return " + repr(prefix),
                "        time.sleep(0.05); return b''",
                "    def close(self): pass",
                "c._open_serial = lambda *args: FakeSerial()",
                "c.hard_reset = lambda port: None",
                "c.run_serial_capture(port='synthetic', raw_path=Path(" + repr(str(raw)) + "), status_path=Path(" + repr(str(status)) + "), hard_timeout_seconds=30, read_timeout_seconds=0.05, post_terminal_drain_seconds=0.2)",
            ])
            process = subprocess.Popen([sys.executable, "-c", code])
            try:
                wait_for_raw_prefix(raw, prefix)
                process.kill()
                process.wait(timeout=5)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
            self.assertEqual(raw.read_bytes(), prefix)
            self.assertFalse(status.exists())


def _preserved_evidence() -> dict[str, tuple[int, str]]:
    root = Path(os.environ.get("P4_AUDIO_EVIDENCE_ROOT", "/tmp/p4-audio-a2-single"))
    result: dict[str, tuple[int, str]] = {}
    for name in ("run-01.log", "evidence.json", "run-02.log", "evidence-run-02.json"):
        path = root / name
        if path.is_file():
            data = path.read_bytes()
            result[name] = (len(data), hashlib.sha256(data).hexdigest())
    return result


def main() -> int:
    before = _preserved_evidence()
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(CaptureHarnessTests)
    )
    after = _preserved_evidence()
    if before != after:
        print("P4_AUDIO_CAPTURE_EVIDENCE_IMMUTABILITY=FAIL")
        return 1
    if not result.wasSuccessful():
        return 1
    print("P4_NANO_CAPTURE_RESET_SEQUENCE_TEST=PASS")
    print("P4_NANO_CAPTURE_RESET_FINAL_STATE_TEST=PASS")
    print("P4_NANO_CAPTURE_DTR_BOOTSTRAP_GUARD_TEST=PASS")
    print("P4_AUDIO_CAPTURE_CHUNK_TEST=PASS")
    print("CAPTURE_CHUNK_BOUNDARY_INVARIANCE=PASS")
    print("TERMINAL_BYTE_OFFSETS_RAW_RELATIVE=PASS")
    print("CAPTURE_STATUS_V2_SEMANTICS_EXPLICIT=PASS")
    print("TERMINAL_LINE_COMPLETE_BEFORE_CANONICAL_CLOSE=YES")
    print("FULL_RAW_CAPTURE_PRESERVED=YES")
    print("CAPTURE_HELPER_SEMANTIC_SCOPE=TRANSPORT_ONLY")
    print("P4_AUDIO_CAPTURE_CRLF_NUL_ANSI_TEST=PASS")
    print("P4_AUDIO_CAPTURE_INTERMEDIATE_MARKER_TEST=PASS")
    print("P4_AUDIO_CAPTURE_TERMINAL_PASS_TEST=PASS")
    print("P4_AUDIO_CAPTURE_TERMINAL_FAIL_TEST=PASS")
    print("P4_AUDIO_CAPTURE_PRE_RESET_MARKER_TEST=PASS")
    print("P4_AUDIO_CAPTURE_PARSER_EXCEPTION_TEST=PASS")
    print("P4_AUDIO_CAPTURE_NO_IDLE_TIMEOUT_TEST=PASS")
    print("P4_AUDIO_CAPTURE_TIMEOUT_PREFIX_TEST=PASS")
    print("P4_AUDIO_CAPTURE_ABRUPT_SOURCE_TEST=PASS")
    print("P4_AUDIO_CAPTURE_SIGTERM_PREFIX_TEST=PASS")
    print("P4_AUDIO_CAPTURE_SIGKILL_PREFIX_TEST=PASS")
    print("P4_AUDIO_CAPTURE_RESET_EPOCH_TEST=PASS")
    print("P4_AUDIO_CAPTURE_RESET_COUNT_TEST=PASS")
    print("P4_AUDIO_CAPTURE_EXCLUSIVE_CREATE_TEST=PASS")
    print("P4_AUDIO_CAPTURE_EVIDENCE_IMMUTABILITY=PASS")
    print("P4_AUDIO_CAPTURE_TESTS=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
