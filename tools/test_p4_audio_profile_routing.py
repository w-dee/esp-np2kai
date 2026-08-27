#!/usr/bin/env python3
"""Static contract for the two qualified P4 audio benchmark build routes."""

from __future__ import annotations

import pathlib
import re
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "tools/emu/build-production.sh"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run_rejected(*arguments: str, expected_fragment: str) -> None:
    completed = subprocess.run(
        [str(BUILD), *arguments],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    require(
        completed.returncode == 2,
        f"expected build-production.sh exit 2 for {arguments}, got "
        f"{completed.returncode}: {completed.stdout}",
    )
    require(
        expected_fragment in completed.stdout,
        f"missing rejection {expected_fragment!r} for {arguments}: "
        f"{completed.stdout}",
    )


def main() -> int:
    source = BUILD.read_text(encoding="utf-8")

    require(
        "[[ \"${variant}\" == \"p4-v3x\" && \"${board}\" == \"generic\" ]]"
        in source,
        "v3x/generic audio benchmark route is missing",
    )
    require(
        "[[ \"${variant}\" == \"p4-v1x\" && \"${board}\" == \"p4-nano\" ]]"
        in source,
        "v1x/p4-nano audio benchmark route is missing",
    )
    require(
        "--audio-only-benchmark requires p4-v3x/generic or p4-v1x/p4-nano"
        in source,
        "audio benchmark rejection must describe the qualified routes",
    )
    require(
        "--audio-opt debug|o2" in source and
        'audio_opt=o2' in source and
        'audio_opt=debug' in source,
        "audio optimization CLI/default policy is missing",
    )
    require(
        "--audio-opt o2 requires --audio-only-benchmark" in source,
        "audio O2 must remain profile-scoped",
    )

    audio_guard = re.search(
        r"if \(\( audio_only_benchmark \)\).*?\nfi\n",
        source,
        re.DOTALL,
    )
    require(audio_guard is not None, "audio benchmark route guard is missing")
    guard = audio_guard.group(0)
    require(
        "variant" in guard and "board" in guard and "p4-v3x" in guard
        and "generic" in guard and "p4-v1x" in guard and "p4-nano" in guard,
        "audio benchmark guard does not cover both qualified routes",
    )

    require(
        'if (( esp_emu_test )) && [[ "${variant}" != "p4-v3x" || '
        '"${board}" != "generic" ]]; then' in source,
        "esp-emu must remain restricted to v3x/generic",
    )
    require(
        '[[ "${variant}" == "p4-v1x" ]] || {' in source
        and "--board p4-nano requires --variant p4-v1x" in source,
        "p4-nano must remain restricted to v1x",
    )

    run_rejected(
        "--variant", "p4-v1x", "--board", "generic",
        "--audio-only-benchmark",
        expected_fragment="requires p4-v3x/generic or p4-v1x/p4-nano",
    )
    run_rejected(
        "--variant", "p4-v3x", "--board", "p4-nano",
        "--audio-only-benchmark",
        expected_fragment="requires --variant p4-v1x",
    )
    run_rejected(
        "--variant", "p4-v1x", "--board", "p4-nano",
        "--audio-only-benchmark", "--esp-emu-test",
        expected_fragment="requires the generic p4-v3x emulator build",
    )
    run_rejected(
        "--variant", "p4-v1x", "--board", "p4-nano",
        "--audio-opt", "o2",
        expected_fragment="--audio-opt o2 requires --audio-only-benchmark",
    )

    print("P4_AUDIO_PROFILE_ROUTING=PASS")
    print("accepted=p4-v3x/generic,p4-v1x/p4-nano")
    print("rejected=p4-v1x/generic,p4-v3x/p4-nano,p4-v1x/p4-nano+esp-emu,o2-without-audio")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
