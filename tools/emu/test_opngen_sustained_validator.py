#!/usr/bin/env python3
"""Self-test the accepted E1C descriptor validator and its failure modes."""

from __future__ import annotations

from pathlib import Path

from validate_opngen_sustained_log import load_descriptor, validate_text


DESCRIPTOR = load_descriptor(Path(__file__).with_name(
    "opngen_sustained_workload_golden.json"))


def valid_log(case: dict[str, object]) -> str:
    profile = case["profile"]
    duration = case["duration_frames"]
    count = case["event_count"]
    event_crc = case["event_crc32"]
    event_sha = case["event_sha256"]
    pcm_frames = case["pcm_frames"]
    pcm_bytes = case["pcm_bytes"]
    pcm_crc = case["pcm_crc32"]
    pcm_sha = case["pcm_sha256"]
    return "\n".join((
        f"E1C_WORKLOAD_META version=1 profile={profile} sample_rate=48000 "
        f"duration_frames={duration} warmup_frames=48000 quantum=240",
        f"E1C_EVENTS produced={count} consumed={count} "
        f"producer_crc32={event_crc} producer_sha256={event_sha} "
        f"consumer_crc32={event_crc} consumer_sha256={event_sha} "
        "sequence_errors=0",
        f"E1C_PCM frames={pcm_frames} bytes={pcm_bytes} crc32={pcm_crc} "
        f"sha256={pcm_sha}",
        "E1C_QUEUE capacity=8 enqueue=0 dequeue=0 max_occupancy=0 "
        "full_waits=0 empty_waits=0",
        "E1C_TIMING diagnostic=1 pure_opngen=DEFERRED",
        "E1C_RESULT=PASS",
        ""))


def replace_once(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise AssertionError(f"mutation source is not unique: {old!r}")
    return text.replace(old, new, 1)


def main() -> int:
    cases = DESCRIPTOR["cases"]
    for name, case in cases.items():
        text = valid_log(case)
        if validate_text(text, DESCRIPTOR, name):
            raise AssertionError(f"known-valid case was rejected: {name}")

    case = cases["SYNTHETIC-LIGHT-30"]
    original = valid_log(case)
    count = str(case["event_count"])
    event_crc = str(case["event_crc32"])
    event_sha = str(case["event_sha256"])
    pcm_crc = str(case["pcm_crc32"])
    pcm_sha = str(case["pcm_sha256"])
    mutations = {
        "missing result": original.replace("E1C_RESULT=PASS\n", ""),
        "duplicate result": original + "E1C_RESULT=PASS\n",
        "explicit FAIL": replace_once(original, "E1C_RESULT=PASS", "E1C_RESULT=FAIL"),
        "wrong workload version": replace_once(original, "version=1", "version=2"),
        "wrong profile": replace_once(original, "profile=SYNTHETIC-LIGHT", "profile=STRESS"),
        "wrong duration": replace_once(original, "duration_frames=1440000", "duration_frames=1440240"),
        "wrong producer count": replace_once(original, f"produced={count}", f"produced={int(count) + 1}"),
        "wrong consumer count": replace_once(original, f"consumed={count}", f"consumed={int(count) + 1}"),
        "count mismatch": replace_once(original, f"consumed={count}", f"consumed={int(count) - 1}"),
        "malformed event CRC": replace_once(original, f"producer_crc32={event_crc}", "producer_crc32=0x123"),
        "altered event CRC": replace_once(original, f"producer_crc32={event_crc}", "producer_crc32=0x00000000"),
        "malformed event SHA": replace_once(original, f"producer_sha256={event_sha}", "producer_sha256=xyz"),
        "altered event SHA": replace_once(original, f"producer_sha256={event_sha}", "producer_sha256=" + "0" * 64),
        "producer consumer SHA mismatch": replace_once(original, f"consumer_sha256={event_sha}", "consumer_sha256=" + "0" * 64),
        "sequence errors": replace_once(original, "sequence_errors=0", "sequence_errors=1"),
        "wrong PCM frames": replace_once(original, "E1C_PCM frames=1440000", "E1C_PCM frames=1440240"),
        "wrong PCM bytes": replace_once(original, "E1C_PCM frames=1440000 bytes=5760000", "E1C_PCM frames=1440000 bytes=5760240"),
        "malformed PCM CRC": replace_once(original, f"E1C_PCM frames=1440000 bytes=5760000 crc32={pcm_crc}", "E1C_PCM frames=1440000 bytes=5760000 crc32=0x123"),
        "altered PCM CRC": replace_once(original, f"E1C_PCM frames=1440000 bytes=5760000 crc32={pcm_crc}", "E1C_PCM frames=1440000 bytes=5760000 crc32=0x00000000"),
        "malformed PCM SHA": replace_once(original, f"E1C_PCM frames=1440000 bytes=5760000 crc32={pcm_crc} sha256={pcm_sha}", f"E1C_PCM frames=1440000 bytes=5760000 crc32={pcm_crc} sha256=xyz"),
        "altered PCM SHA": replace_once(original, f"E1C_PCM frames=1440000 bytes=5760000 crc32={pcm_crc} sha256={pcm_sha}", f"E1C_PCM frames=1440000 bytes=5760000 crc32={pcm_crc} sha256=" + "0" * 64),
    }
    for name, text in mutations.items():
        if not validate_text(text, DESCRIPTOR, "SYNTHETIC-LIGHT-30"):
            raise AssertionError(f"mutation was accepted: {name}")
    print("E1C_VALIDATOR_SELFTEST=PASS")
    print(f"E1C_VALIDATOR_CASES={len(cases) + len(mutations)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
