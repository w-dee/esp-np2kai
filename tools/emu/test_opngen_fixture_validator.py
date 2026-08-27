#!/usr/bin/env python3
"""Self-test the E1 OPNGEN log validator and golden enforcement."""

from __future__ import annotations

from pathlib import Path

from validate_opngen_fixture_log import load_golden, validate_text


GOLDEN = load_golden(Path(__file__).with_name("opngen_fixture_golden.json"))


def valid_log() -> str:
    fixture = GOLDEN["fixture"]
    deterministic = GOLDEN["deterministic"]
    table = deterministic["table"]
    pcm = deterministic["pcm"]
    metrics = deterministic["metrics"]
    invariants = deterministic["invariants"]
    return "\n".join((
        "E1_OPNGEN_META " + " ".join(
            f"{key}={fixture[key]}" for key in
            ("version", "rate_hz", "frames", "channels", "pcm_bytes", "volume")),
        "E1_OPNGEN_TABLE " + " ".join(
            f"{key}={table[key]}" for key in
            ("ratebit", "calc1024", "fmvol", "sintable_crc32",
             "envtable_crc32", "envcurve_crc32")),
        "E1_OPNGEN_PCM " + " ".join(
            f"{key}={pcm[key]}" for key in
            ("crc_algorithm", "crc32", "sha256", "s32_abs_peak",
             "nonzero_s16_samples", "clip_samples")),
        "E1_OPNGEN_METRICS " + " ".join(
            f"{key}={metrics[key]}" for key in
            ("l_sumsq", "r_sumsq", "l_rms_q16", "r_rms_q16")),
        "E1_OPNGEN_INVARIANTS " + " ".join(
            f"{key}={invariants[key]}" for key in
            ("repeat", "partition_240", "partition_1", "silence",
             "frequency_change", "keyoff", "nontrivial")),
        "E1_OPNGEN_TIMING init_us=0 render_us=0 us_per_frame_q16=0 realtime_factor_q16=0",
        "E1A_SYNTH_EVENT_META version=1 count=64 record_bytes=24",
        "E1A_SYNTH_EVENT_TRACE crc32=0x00000001 sha256=" + "0" * 64,
        "E1A_SYNTH_EVENT_INVARIANTS validation=PASS reference_match=PASS order_sensitive=PASS",
        "E1A_SYNTH_EVENT_RESULT=PASS",
        "E1_OPNGEN_RESULT=PASS",
        ""))


def replace_once(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise AssertionError(f"mutation source is not unique: {old!r}")
    return text.replace(old, new, 1)


def main() -> int:
    original = valid_log()
    cases = {
        "valid": original,
        "missing terminal result": original.replace("E1_OPNGEN_RESULT=PASS\n", ""),
        "explicit RESULT=FAIL": replace_once(original, "E1_OPNGEN_RESULT=PASS", "E1_OPNGEN_RESULT=FAIL"),
        "duplicate terminal result": original + "E1_OPNGEN_RESULT=PASS\n",
        "malformed CRC32": replace_once(original, "sintable_crc32=0x7aba7bbd", "sintable_crc32=0x123"),
        "malformed SHA-256": replace_once(original, "sha256=" + GOLDEN["deterministic"]["pcm"]["sha256"], "sha256=xyz"),
        "missing META line": "\n".join(line for line in original.splitlines() if not line.startswith("E1_OPNGEN_META ")) + "\n",
        "missing TABLE line": "\n".join(line for line in original.splitlines() if not line.startswith("E1_OPNGEN_TABLE ")) + "\n",
        "missing PCM line": "\n".join(line for line in original.splitlines() if not line.startswith("E1_OPNGEN_PCM ")) + "\n",
        "missing invariant field": replace_once(original, " keyoff=PASS", ""),
        "wrong frame count": replace_once(original, "frames=28800", "frames=28801"),
        "wrong PCM byte count": replace_once(original, "pcm_bytes=115200", "pcm_bytes=115202"),
        "altered accepted PCM CRC": replace_once(original, "crc32=0x17496602", "crc32=0x17496603"),
        "altered accepted PCM SHA": replace_once(original, "sha256=" + GOLDEN["deterministic"]["pcm"]["sha256"], "sha256=" + "0" * 64),
        "altered table CRC": replace_once(original, "sintable_crc32=0x7aba7bbd", "sintable_crc32=0x7aba7bbe"),
        "missing E1A terminal": original.replace("E1A_SYNTH_EVENT_RESULT=PASS\n", ""),
        "duplicate E1A terminal": original + "E1A_SYNTH_EVENT_RESULT=PASS\n",
        "explicit E1A RESULT=FAIL": replace_once(original, "E1A_SYNTH_EVENT_RESULT=PASS", "E1A_SYNTH_EVENT_RESULT=FAIL"),
        "missing E1A count": replace_once(original, " count=64", ""),
        "malformed E1A CRC32": replace_once(original, "E1A_SYNTH_EVENT_TRACE crc32=0x00000001", "E1A_SYNTH_EVENT_TRACE crc32=0x123"),
        "malformed E1A SHA-256": replace_once(original, "E1A_SYNTH_EVENT_TRACE crc32=0x00000001 sha256=" + "0" * 64, "E1A_SYNTH_EVENT_TRACE crc32=0x00000001 sha256=xyz"),
        "missing E1A invariant": replace_once(original, " order_sensitive=PASS", ""),
        "E1A reference mismatch": replace_once(original, "reference_match=PASS", "reference_match=FAIL"),
    }
    if validate_text(cases.pop("valid"), GOLDEN):
        raise AssertionError("known-valid formal log was rejected")
    for name, text in cases.items():
        if not validate_text(text, GOLDEN):
            raise AssertionError(f"mutated log was accepted: {name}")
    print("E1_OPNGEN_VALIDATOR_SELFTEST=PASS")
    print(f"E1_OPNGEN_VALIDATOR_CASES={len(cases) + 1}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
