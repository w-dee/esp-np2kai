#!/usr/bin/env python3
"""Fresh-process candidate-identity runner for the E1C host workload."""

import argparse
import re
import subprocess
import sys


CASES = (
    ("SYNTHETIC-LIGHT", 30, 2),
    ("SYNTHETIC-HEAVY", 30, 2),
    ("STRESS", 30, 2),
    ("STRESS", 60, 1),
)
EXPECTED_EVENTS = {
    ("SYNTHETIC-LIGHT", 30): 2157,
    ("SYNTHETIC-HEAVY", 30): 7197,
    ("STRESS", 30): 20607,
    ("STRESS", 60): 41127,
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
    if actual != str(expected):
        raise ValueError("%s expected=%s actual=%s" % (key, expected, actual))


def validate(output, profile, duration):
    if "=FAIL" in output or output.count("E1C_RESULT=PASS") != 1:
        raise ValueError("missing or explicit E1C failure result")
    if not re.search(r"^E1C_RESULT=PASS$", output, re.MULTILINE):
        raise ValueError("missing exact E1C pass result")
    for marker in (
        "E1C_PATH_COUNTER_WRAP=PASS",
        "E1C_PATH_PRODUCER_DONE_BOUNDARIES=PASS",
        "E1C_PATH_SAME_TIMESTAMP_BURST=PASS",
        "E1C_PATH_ABORT_PROTOCOL=PASS",
    ):
        one_line(output, marker)
    meta = fields(one_line(output, "E1C_WORKLOAD_META "))
    events = fields(one_line(output, "E1C_EVENTS "))
    pcm = fields(one_line(output, "E1C_PCM "))
    queue = fields(one_line(output, "E1C_QUEUE "))
    one_line(output, "E1C_TIMING ")
    frames = duration * 48000
    event_count = EXPECTED_EVENTS[(profile, duration)]
    require(meta, "version", 1)
    require(meta, "profile", profile)
    require(meta, "sample_rate", 48000)
    require(meta, "duration_frames", frames)
    require(meta, "warmup_frames", 48000)
    require(meta, "quantum", 240)
    for key in ("produced", "consumed"):
        require(events, key, event_count)
    require(events, "sequence_errors", 0)
    if events.get("producer_crc32") != events.get("consumer_crc32"):
        raise ValueError("producer/consumer CRC mismatch")
    if events.get("producer_sha256") != events.get("consumer_sha256"):
        raise ValueError("producer/consumer SHA mismatch")
    for key in ("producer_sha256", "consumer_sha256"):
        if re.fullmatch(r"[0-9a-f]{64}", events.get(key, "")) is None:
            raise ValueError("invalid %s" % key)
    require(pcm, "frames", frames)
    require(pcm, "bytes", frames * 4)
    if re.fullmatch(r"[0-9a-f]{64}", pcm.get("sha256", "")) is None:
        raise ValueError("invalid PCM SHA")
    if not re.fullmatch(r"0x[0-9a-f]{8}", pcm.get("crc32", "")):
        raise ValueError("invalid PCM CRC")
    require(queue, "capacity", 8)
    require(queue, "enqueue", event_count)
    require(queue, "dequeue", event_count)
    return {
        "events": events["produced"],
        "event_crc32": events["producer_crc32"],
        "event_sha256": events["producer_sha256"],
        "pcm_frames": pcm["frames"],
        "pcm_bytes": pcm["bytes"],
        "pcm_crc32": pcm["crc32"],
        "pcm_sha256": pcm["sha256"],
    }


def run_once(binary, profile, duration, timeout_seconds):
    try:
        result = subprocess.run(
            [binary, "--profile", profile, "--duration-seconds", str(duration)],
            text=True, capture_output=True, timeout=timeout_seconds, check=False,
        )
    except subprocess.TimeoutExpired as error:
        return "timeout after %ss" % timeout_seconds, (error.stdout or "")
    output = result.stdout + result.stderr
    if result.returncode != 0:
        return "exit=%d" % result.returncode, output
    try:
        identity = validate(output, profile, duration)
    except ValueError as error:
        return str(error), output
    return identity, output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--timeout-seconds", type=float, default=90.0)
    args = parser.parse_args()
    previous = {}
    for profile, duration, repetitions in CASES:
        key = (profile, duration)
        for iteration in range(1, repetitions + 1):
            identity, output = run_once(
                args.binary, profile, duration, args.timeout_seconds)
            if isinstance(identity, str):
                print("E1C_CANDIDATE_RESULT=FAIL")
                print("E1C failure profile=%s duration=%d iteration=%d: %s" %
                      (profile, duration, iteration, identity), file=sys.stderr)
                print(output, file=sys.stderr)
                return 1
            if key in previous and identity != previous[key]:
                print("E1C_CANDIDATE_RESULT=FAIL")
                print("candidate identity changed for %s/%ss" % key,
                      file=sys.stderr)
                return 1
            previous[key] = identity
            print("E1C_CANDIDATE profile=%s duration_seconds=%d event_count=%s"
                  " event_crc32=%s event_sha256=%s pcm_frames=%s pcm_bytes=%s"
                  " pcm_crc32=%s pcm_sha256=%s" %
                  (profile, duration, identity["events"],
                   identity["event_crc32"], identity["event_sha256"],
                   identity["pcm_frames"], identity["pcm_bytes"],
                   identity["pcm_crc32"], identity["pcm_sha256"]))
    print("E1C_CANDIDATE_RESULT=PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
