#!/usr/bin/env python3
"""Fail-closed fresh-process validator for the E1B SPSC host harness."""

import argparse
import re
import subprocess
import sys


MODES = ("FAST_PRODUCER", "FAST_CONSUMER", "ALTERNATING", "WRAP_PRESSURE")
TSAN_ENVIRONMENT_BLOCKER = "FATAL: ThreadSanitizer: unexpected memory mapping"
EXPECTED = {
    "event_sha256": "f22cb06362889470385de729b807da50dcc68d09c37739242e3c33589e272af5",
    "pcm_sha256": "a19c7d651f9d3bf0aa6d493eb80d66f0543464f8c937796047fbc962f550a2e9",
}


def one_line(output, prefix):
    lines = [line for line in output.splitlines() if line.startswith(prefix)]
    if len(lines) != 1:
        raise ValueError("marker %s count=%d" % (prefix.rstrip(), len(lines)))
    return lines[0]


def fields(line):
    result = {}
    for token in line.split()[1:]:
        if "=" not in token:
            raise ValueError("malformed token %s" % token)
        key, value = token.split("=", 1)
        if key in result:
            raise ValueError("duplicate field %s" % key)
        result[key] = value
    return result


def require(values, key, expected):
    actual = values.get(key)
    if actual != expected:
        raise ValueError("%s expected=%s actual=%s" % (key, expected, actual))


def require_positive(values, key):
    try:
        value = int(values[key], 0)
    except (KeyError, ValueError) as error:
        raise ValueError("invalid %s" % key) from error
    if value <= 0:
        raise ValueError("%s must be positive, actual=%d" % (key, value))


def validate_output(output, mode):
    if "=FAIL" in output:
        raise ValueError("explicit FAIL marker")
    if output.count("E1B_SPSC_RESULT=PASS") != 1:
        raise ValueError("E1B result marker must occur exactly once")
    if not re.search(r"^E1B_SPSC_RESULT=PASS$", output, re.MULTILINE):
        raise ValueError("missing exact E1B pass marker")

    meta = fields(one_line(output, "E1B_SPSC_META "))
    queue = fields(one_line(output, "E1B_SPSC_QUEUE "))
    events = fields(one_line(output, "E1B_SPSC_EVENTS "))
    pcm = fields(one_line(output, "E1B_SPSC_PCM "))
    invariants = fields(one_line(output, "E1B_SPSC_INVARIANTS "))
    one_line(output, "E1A_SYNTH_EVENT_RESULT=PASS")
    trace_meta = fields(one_line(output, "E1A_SYNTH_EVENT_META "))
    trace = fields(one_line(output, "E1A_SYNTH_EVENT_TRACE "))

    for key, expected in {
        "version": "1", "mode": mode, "capacity": "8", "events": "64",
        "end_frame": "28800",
    }.items():
        require(meta, key, expected)
    for key, expected in {
        "enqueue": "64", "dequeue": "64", "sequence_errors": "0",
        "rendered_frames": "28800", "final_sequence": "63", "first_error": "0",
        "head_counter": "64", "tail_counter": "64",
    }.items():
        require(queue, key, expected)
    require_positive(queue, "producer_slot_wraps")
    require_positive(queue, "consumer_slot_wraps")
    for key, expected in {"count": "64", "record_bytes": "24"}.items():
        require(events, key, expected)
        require(trace_meta, key, expected)
    for key, expected in {
        "crc32": "0x807a514e", "sha256": EXPECTED["event_sha256"],
    }.items():
        require(events, key, expected)
        require(trace, key, expected)
    require_positive(events, "same_timestamp_adjacent")
    for key, expected in {
        "bytes": "115200", "crc32": "0x17496602",
        "sha256": EXPECTED["pcm_sha256"],
    }.items():
        require(pcm, key, expected)
    for key in ("reference_match", "event_trace", "ordering", "wrap", "mode_pressure"):
        require(invariants, key, "PASS")
    if mode == "FAST_CONSUMER":
        require_positive(queue, "empty_waits")
    if mode == "WRAP_PRESSURE":
        require_positive(queue, "full_waits")


def run_once(binary, mode, timeout_seconds):
    try:
        result = subprocess.run(
            [binary, "--mode", mode], text=True, capture_output=True,
            timeout=timeout_seconds, check=False,
        )
    except subprocess.TimeoutExpired as error:
        output = (error.stdout or "") + (error.stderr or "")
        return "timeout after %ss" % timeout_seconds, output
    output = result.stdout + result.stderr
    if result.returncode != 0:
        return "exit=%d" % result.returncode, output
    try:
        validate_output(output, mode)
    except ValueError as error:
        return str(error), output
    return None, output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--repetitions", type=int, required=True)
    parser.add_argument("--timeout-seconds", type=float, required=True)
    parser.add_argument("--tsan-status", action="store_true")
    args = parser.parse_args()
    if args.repetitions <= 0 or args.timeout_seconds <= 0:
        parser.error("repetitions and timeout-seconds must be positive")

    for mode in MODES:
        passed = 0
        for iteration in range(1, args.repetitions + 1):
            error, output = run_once(args.binary, mode, args.timeout_seconds)
            if error is not None:
                if args.tsan_status and TSAN_ENVIRONMENT_BLOCKER in output:
                    print("E1B_FORMAL_TSAN status=BLOCKED-BY-ENVIRONMENT")
                    return 0
                print("E1B_FORMAL_MODE mode=%s pass=%d total=%d" %
                      (mode, passed, args.repetitions))
                print("E1B_FORMAL_RESULT=FAIL")
                if args.tsan_status:
                    print("E1B_FORMAL_TSAN status=FAIL")
                print("E1B formal failure mode=%s iteration=%d: %s" %
                      (mode, iteration, error), file=sys.stderr)
                print(output, file=sys.stderr)
                return 1
            passed += 1
        print("E1B_FORMAL_MODE mode=%s pass=%d total=%d" %
              (mode, passed, args.repetitions))
    print("E1B_FORMAL_RESULT=PASS")
    if args.tsan_status:
        print("E1B_FORMAL_TSAN status=PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
