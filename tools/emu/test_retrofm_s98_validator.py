#!/usr/bin/env python3
"""Mutation tests for the accepted RetroFM playback descriptor validator."""

from __future__ import annotations

import unittest
from pathlib import Path

from validate_retrofm_s98_log import load_descriptor, validate_text


DESCRIPTOR_PATH = Path(__file__).with_name("opngen_retrofm_s98_golden.json")
DESCRIPTOR = load_descriptor(DESCRIPTOR_PATH)
FIXTURE = DESCRIPTOR["fixture_id"]


def _good_log() -> str:
    parser = DESCRIPTOR["parser"]
    event = DESCRIPTOR["event"]
    pcm = DESCRIPTOR["pcm"]
    return "\n".join((
        f"S98_S3_SOURCE fixture={FIXTURE} source_bytes={DESCRIPTOR['input']['bytes']} source_sha256={DESCRIPTOR['input']['sha256']}",
        f"S98_S3_META fixture={FIXTURE} s98_version={parser['s98_version']} device_count={parser['device_count']} device_type={parser['device_type']} declared_clock={parser['declared_device_clock_hz']} effective_clock={parser['effective_opngen_clock_hz']} clock_policy={parser['clock_policy']} raw_timer={parser['timer_numerator']}/{parser['timer_denominator']} effective_timer={parser['timer_numerator']}/{parser['timer_denominator']} data_offset={parser['data_offset']} tag_offset={parser['tag_offset']} loop_offset={parser['loop_offset']} source_writes={parser['source_write_count']} ignored_writes={parser['ignored_write_count']} final_sync={parser['final_sync_count']} end_frame={parser['end_frame']}",
        f"S98_S3_EVENTS fixture={FIXTURE} preflight_count={event['count']} preflight_crc32={event['crc32'][2:]} preflight_sha256={event['sha256']} producer_count={event['count']} producer_crc32={event['crc32'][2:]} producer_sha256={event['sha256']} consumer_count={event['count']} consumer_crc32={event['crc32'][2:]} consumer_sha256={event['sha256']} sequence_errors=0 same_timestamp_pairs=0",
        f"S98_S3_PCM fixture={FIXTURE} frames={pcm['frames']} bytes={pcm['bytes']} crc32={pcm['crc32'][2:]} sha256={pcm['sha256']}",
        f"S98_S3_INVARIANTS fixture={FIXTURE} parser_repeat=PASS transport_identity=PASS source_immutable=PASS",
        f"S98_S3_RESULT fixture={FIXTURE} PASS",
        "",
    ))


def _replace_once(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise AssertionError(f"non-unique mutation source: {old!r}")
    return text.replace(old, new, 1)


class RetroFmPlaybackValidatorTest(unittest.TestCase):
    def test_known_good_log(self) -> None:
        self.assertEqual(validate_text(_good_log(), DESCRIPTOR), [])

    def test_required_mutations_fail_closed(self) -> None:
        original = _good_log()
        event = DESCRIPTOR["event"]
        mutations = {
            "missing result": original.replace(f"S98_S3_RESULT fixture={FIXTURE} PASS\n", ""),
            "duplicate result": original + f"S98_S3_RESULT fixture={FIXTURE} PASS\n",
            "explicit FAIL": _replace_once(original, f"S98_S3_RESULT fixture={FIXTURE} PASS", f"S98_S3_RESULT fixture={FIXTURE} FAIL"),
            "wrong fixture": original.replace(FIXTURE, "other_fixture"),
            "wrong input bytes": _replace_once(original, "source_bytes=3753", "source_bytes=3752"),
            "malformed input SHA": _replace_once(original, DESCRIPTOR["input"]["sha256"], "xyz"),
            "altered input SHA": _replace_once(original, DESCRIPTOR["input"]["sha256"], "0" * 64),
            "wrong declared clock": _replace_once(original, "declared_clock=4000000", "declared_clock=3993600"),
            "wrong effective clock": _replace_once(original, "effective_clock=3993600", "effective_clock=4000000"),
            "wrong clock policy": _replace_once(original, "clock_policy=WORKLOAD_CLOCK_MISMATCH", "clock_policy=EXACT_NP2"),
            "wrong timer numerator": _replace_once(original, "raw_timer=1/44100", "raw_timer=2/44100"),
            "wrong timer denominator": _replace_once(original, "effective_timer=1/44100", "effective_timer=1/1000"),
            "wrong source writes": _replace_once(original, "source_writes=1047", "source_writes=1046"),
            "ignored writes": _replace_once(original, "ignored_writes=0", "ignored_writes=1"),
            "wrong final sync": _replace_once(original, "final_sync=530082", "final_sync=530081"),
            "wrong end frame": _replace_once(original, "end_frame=576960", "end_frame=576959"),
            "wrong preflight count": _replace_once(original, "preflight_count=1047", "preflight_count=1046"),
            "wrong producer count": _replace_once(original, "producer_count=1047", "producer_count=1046"),
            "wrong consumer count": _replace_once(original, "consumer_count=1047", "consumer_count=1046"),
            "wrong event CRC": _replace_once(original, f"preflight_crc32={event['crc32'][2:]}", "preflight_crc32=00000000"),
            "malformed event SHA": _replace_once(original, f"preflight_sha256={event['sha256']}", "preflight_sha256=xyz"),
            "altered event SHA": _replace_once(original, f"producer_sha256={event['sha256']}", "producer_sha256=" + "0" * 64),
            "three-way mismatch": _replace_once(original, f"consumer_crc32={event['crc32'][2:]}", "consumer_crc32=00000000"),
            "sequence errors": _replace_once(original, "sequence_errors=0", "sequence_errors=1"),
            "wrong PCM frames": _replace_once(original, "frames=576960", "frames=576959"),
            "wrong PCM bytes": _replace_once(original, "bytes=2307840", "bytes=2307839"),
            "wrong PCM CRC": _replace_once(original, "crc32=79b0dfad", "crc32=00000000"),
            "malformed PCM SHA": _replace_once(original, "sha256=1d4d24ad9c966dea085607afee6a9ecb049c2c476863c534dbfe0e50ace1016b", "sha256=xyz"),
            "altered PCM SHA": _replace_once(original, "sha256=1d4d24ad9c966dea085607afee6a9ecb049c2c476863c534dbfe0e50ace1016b", "sha256=" + "0" * 64),
        }
        for name, mutated in mutations.items():
            with self.subTest(name=name):
                self.assertTrue(validate_text(mutated, DESCRIPTOR), name)


if __name__ == "__main__":
    unittest.main()
