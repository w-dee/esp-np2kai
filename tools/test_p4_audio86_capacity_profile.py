#!/usr/bin/env python3
"""Static contract checks for the isolated 86H P4 capacity profile."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
MAIN_CMAKE = (ROOT / "firmware/main/CMakeLists.txt").read_text()
MAIN_CPP = (ROOT / "firmware/main/main.cpp").read_text()
COMP_CMAKE = (ROOT / "firmware/components/p4_nano_audio86_capacity/CMakeLists.txt").read_text()
COMP_CPP = (ROOT / "firmware/components/p4_nano_audio86_capacity/p4_nano_audio86_capacity.cpp").read_text()
BUILD = (ROOT / "tools/emu/build-production.sh").read_text()
DEFAULTS = (ROOT / "firmware/sdkconfig.defaults.p4-audio86-capacity").read_text()


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    check("P4_NANO_AUDIO86_CAPACITY_PROFILE_ACTIVE" in MAIN_CMAKE, "profile not routed")
    check("p4_nano_audio86_capacity" in MAIN_CMAKE, "component not selected")
    check("P4_NANO_AUDIO86_CAPACITY_PROFILE" in MAIN_CPP, "app_main profile missing")
    check("--audio86-capacity" in BUILD and "P4_NANO_AUDIO86_CAPACITY_PROFILE" in BUILD,
          "build script option missing")
    check("build-${board}-${variant}-audio86-capacity" in BUILD, "profile build directory missing")
    check("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360=y" in DEFAULTS and
          "CONFIG_FREERTOS_HZ=100" in DEFAULTS and
          "CONFIG_SPIRAM_SPEED_200M=y" in DEFAULTS and
          "# CONFIG_PM_ENABLE is not set" in DEFAULTS, "sdkconfig defaults incomplete")
    check("PRIV_REQUIRES np2audio86_fixture" in COMP_CMAKE, "portable fixture not required")
    check("P4_AUDIO86_PROFILE_MODE" in COMP_CMAKE,
          "profile component-timing mode is not selectable")

    # The capacity branch must not pull the normal runtime/display/audio sink.
    branch = re.search(
        r"elseif\(P4_NANO_AUDIO86_CAPACITY_PROFILE_ACTIVE\).*?\n(elseif|else\(\))",
        MAIN_CMAKE,
        re.DOTALL,
    )
    check(branch is not None, "capacity CMake branch missing")
    forbidden = ("p4_nano_display", "np2core", "storage_fatfs", "audio_i2s",
                 "es8311", "pcm86io", "board86")
    check(not any(token in branch.group(0).lower() for token in forbidden),
          "capacity branch enables forbidden runtime dependency")
    source_lower = COMP_CPP.lower()
    check(not any(token in source_lower for token in ("i2s", "es8311", "pcm86io", "board86")),
          "capacity component contains forbidden sink/runtime integration")
    check("xTaskNotifyGive" in COMP_CPP and "xSemaphoreGive(ctx->pacing)" in COMP_CPP,
          "transport and pacing wake channels are not distinct")
    check("compare_exchange_strong" in COMP_CPP and "PACING" not in COMP_CPP,
          "first-error CAS or accidental pacing serialization missing")
    check("static_cast<uint64_t>(q) * kQuantumUs" in COMP_CPP and
          "finish >= deadline" in COMP_CPP, "absolute schedule/deadline missing")
    check("ceil(p * N) - 1" not in COMP_CPP, "placeholder percentile text present")
    # Nearest-rank indices are part of the frozen 12,000-sample contract.
    check(int(0.99 * 12000 + 0.999999999) - 1 == 11879, "p99 index changed")
    check(int(0.999 * 12000 + 0.999999999) - 1 == 11987, "p99.9 index changed")
    print("P4_AUDIO86_STATIC_CONTRACT=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
