#!/usr/bin/env python3
"""Self-test the accepted S3 descriptor validator and fail-closed mutations."""

from __future__ import annotations

from pathlib import Path

from validate_opngen_s98_fixture_log import load_descriptor, validate_text


DESCRIPTOR = load_descriptor(Path(__file__).with_name("opngen_s98_fixture_golden.json"))


def valid_log(case: dict[str, object]) -> str:
    name = str(case["fixture_name"])
    event_crc = str(case["event_crc32"])[2:]
    event_sha = str(case["event_sha256"])
    return "\n".join((
        f"S98_S3_SOURCE fixture={name} source_bytes={case['source_bytes']} source_sha256={case['source_sha256']}",
        f"S98_S3_META fixture={name} s98_version=3 device_count={case['device_count']} device_type={case['device_type']} declared_clock={case['declared_device_clock_hz']} effective_clock={case['effective_opngen_clock_hz']} clock_policy={case['clock_policy']} raw_timer={case['raw_timer_numerator']}/{case['raw_timer_denominator']} effective_timer={case['effective_timer_numerator']}/{case['effective_timer_denominator']} data_offset={case['data_offset']} tag_offset={case['tag_offset']} loop_offset={case['loop_offset']} source_writes={case['source_write_count']} ignored_writes={case['ignored_write_count']} final_sync={case['final_sync_count']} end_frame={case['end_frame']}",
        f"S98_S3_EVENTS fixture={name} preflight_count={case['event_count']} preflight_crc32={event_crc} preflight_sha256={event_sha} producer_count={case['event_count']} producer_crc32={event_crc} producer_sha256={event_sha} consumer_count={case['event_count']} consumer_crc32={event_crc} consumer_sha256={event_sha} sequence_errors=0 same_timestamp_pairs={case['same_timestamp_pairs']}",
        f"S98_S3_PCM fixture={name} frames={case['pcm_frames']} bytes={case['pcm_bytes']} crc32={str(case['pcm_crc32'])[2:]} sha256={case['pcm_sha256']}",
        f"S98_S3_INVARIANTS fixture={name} parser_repeat=PASS transport_identity=PASS source_immutable=PASS",
        f"S98_S3_RESULT fixture={name} PASS",
        ""))


def replace_once(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise AssertionError(f"mutation source is not unique: {old!r}")
    return text.replace(old, new, 1)


def main() -> int:
    for name, case in DESCRIPTOR["fixtures"].items():
        if validate_text(valid_log(case), DESCRIPTOR, name):
            raise AssertionError(f"known-valid fixture was rejected: {name}")

    case = DESCRIPTOR["fixtures"]["fm_single_tone"]
    original = valid_log(case)
    event_crc = str(case["event_crc32"])[2:]
    event_sha = str(case["event_sha256"])
    pcm_crc = str(case["pcm_crc32"])[2:]
    pcm_sha = str(case["pcm_sha256"])
    mutations = {
        "missing terminal": original.replace("S98_S3_RESULT fixture=fm_single_tone PASS\n", ""),
        "duplicate terminal": original + "S98_S3_RESULT fixture=fm_single_tone PASS\n",
        "explicit FAIL": replace_once(original, "S98_S3_RESULT fixture=fm_single_tone PASS", "S98_S3_RESULT fixture=fm_single_tone FAIL"),
        "unknown fixture": original.replace("fm_single_tone", "unknown_fixture"),
        "wrong source size": replace_once(original, "source_bytes=89", "source_bytes=90"),
        "malformed source SHA": replace_once(original, str(case["source_sha256"]), "xyz"),
        "altered source SHA": replace_once(original, str(case["source_sha256"]), "0" * 64),
        "wrong declared clock": replace_once(original, "declared_clock=3993600", "declared_clock=4000000"),
        "wrong timer metadata": replace_once(original, "raw_timer=1/1000", "raw_timer=2/1000"),
        "wrong source write count": replace_once(original, "source_writes=12", "source_writes=11"),
        "ignored write count": replace_once(original, "ignored_writes=0", "ignored_writes=1"),
        "wrong final sync": replace_once(original, "final_sync=50", "final_sync=49"),
        "wrong end frame": replace_once(original, "end_frame=2400", "end_frame=2401"),
        "wrong preflight count": replace_once(original, "preflight_count=12", "preflight_count=11"),
        "wrong producer count": replace_once(original, "producer_count=12", "producer_count=11"),
        "wrong consumer count": replace_once(original, "consumer_count=12", "consumer_count=11"),
        "count mismatch": replace_once(original, "consumer_count=12", "consumer_count=13"),
        "malformed event CRC": replace_once(original, f"preflight_crc32={event_crc}", "preflight_crc32=123"),
        "altered event CRC": replace_once(original, f"producer_crc32={event_crc}", "producer_crc32=00000000"),
        "malformed event SHA": replace_once(original, f"preflight_sha256={event_sha}", "preflight_sha256=xyz"),
        "altered event SHA": replace_once(original, f"producer_sha256={event_sha}", "producer_sha256=" + "0" * 64),
        "event SHA mismatch": replace_once(original, f"consumer_sha256={event_sha}", "consumer_sha256=" + "0" * 64),
        "sequence errors": replace_once(original, "sequence_errors=0", "sequence_errors=1"),
        "wrong PCM frames": replace_once(original, "frames=2400", "frames=2401"),
        "wrong PCM bytes": replace_once(original, "bytes=9600", "bytes=9601"),
        "altered PCM CRC": replace_once(original, f"crc32={pcm_crc}", "crc32=00000000"),
        "malformed PCM SHA": replace_once(original, f"sha256={pcm_sha}", "sha256=xyz"),
        "altered PCM SHA": replace_once(original, f"sha256={pcm_sha}", "sha256=" + "0" * 64),
    }
    for name, mutated in mutations.items():
        if not validate_text(mutated, DESCRIPTOR, "fm_single_tone"):
            raise AssertionError(f"mutation was accepted: {name}")
    print("S98_S3_VALIDATOR_SELFTEST=PASS")
    print(f"S98_S3_VALIDATOR_CASES={len(DESCRIPTOR['fixtures']) + len(mutations)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
