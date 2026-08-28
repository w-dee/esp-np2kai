#!/usr/bin/env python3
"""Machine-check the topology required by the P4 audio timer timestamp path."""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "firmware/components/p4_nano_audio_benchmark/p4_nano_audio_benchmark.cpp"


def fail(message: str) -> int:
    print(f"P4_AUDIO_TIMER_TOPOLOGY_AUDIT=FAIL reason={message}")
    return 1


def config_value(text: str, name: str) -> str | None:
    match = re.search(rf"^#define {re.escape(name)} ([^\n]+)$", text, re.MULTILINE)
    if match:
        return match.group(1).strip()
    match = re.search(rf"^{re.escape(name)}=([^\n]+)$", text, re.MULTILINE)
    return match.group(1).strip() if match else None


def parse_int(value: str | None) -> int | None:
    if value is None:
        return None
    value = value.strip()
    try:
        return int(value, 0)
    except ValueError:
        return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--idf-path", type=Path,
                        default=Path(os.environ["IDF_PATH"]) if os.environ.get("IDF_PATH") else None)
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    sdkconfig_h = build_dir / "config/sdkconfig.h"
    sdkconfig = build_dir / "sdkconfig"
    config_path = sdkconfig_h if sdkconfig_h.is_file() else sdkconfig
    if not config_path.is_file():
        return fail(f"missing_config={config_path}")
    if not SOURCE.is_file():
        return fail("missing_benchmark_source")
    if args.idf_path is None:
        return fail("missing_idf_path")

    config = config_path.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    esp_task = args.idf_path / "components/esp_system/include/esp_task.h"
    freertos = args.idf_path / "components/freertos/config/include/freertos/FreeRTOSConfig.h"
    if not esp_task.is_file() or not freertos.is_file():
        return fail("missing_idf_priority_headers")
    esp_task_text = esp_task.read_text(encoding="utf-8")
    freertos_text = freertos.read_text(encoding="utf-8")

    timer_affinity = parse_int(config_value(config, "CONFIG_ESP_TIMER_TASK_AFFINITY"))
    max_priorities_match = re.search(
        r"#define\s+configMAX_PRIORITIES\s+\(\s*(\d+)\s*\)", freertos_text)
    timer_priority_match = re.search(
        r"#define\s+ESP_TASK_TIMER_PRIO\s+\(\s*ESP_TASK_PRIO_MAX\s*-\s*(\d+)\s*\)",
        esp_task_text)
    if timer_affinity is None or max_priorities_match is None or timer_priority_match is None:
        return fail("unresolved_timer_configuration")
    max_priorities = int(max_priorities_match.group(1))
    timer_priority = max_priorities - int(timer_priority_match.group(1))

    consumer_core_match = re.search(r"constexpr int kConsumerCore = (\d+);", source)
    consumer_priority_match = re.search(
        r"constexpr UBaseType_t kConsumerPriority = tskIDLE_PRIORITY \+ (\d+);",
        source)
    consumer_create_match = re.search(
        r"xTaskCreatePinnedToCore\(pcm_consumer_task,\s*\"p4_audio_consumer\".*?"
        r"kConsumerPriority,\s*&ctx\.consumer_task,\s*kConsumerCore\)",
        source, re.DOTALL)
    timer_dispatch_match = re.search(
        r"pacing_args\s*=\s*\{pacing_timer_callback,\s*&ctx,\s*ESP_TIMER_TASK",
        source)
    if consumer_core_match is None or consumer_priority_match is None:
        return fail("unresolved_consumer_constants")
    if consumer_create_match is None or timer_dispatch_match is None:
        return fail("task_or_timer_dispatch_not_pinned")

    consumer_core = int(consumer_core_match.group(1))
    consumer_priority = int(consumer_priority_match.group(1))
    checks = {
        "timer_core": timer_affinity == 0,
        "consumer_core": consumer_core == 0,
        "timer_priority_gt_consumer": timer_priority > consumer_priority,
        "consumer_priority_idle_plus_5": consumer_priority == 5,
        "affinity_cpu0": config_value(config, "CONFIG_ESP_TIMER_TASK_AFFINITY_CPU0")
        in {"1", "y"},
    }
    failed = [name for name, passed in checks.items() if not passed]
    if failed:
        return fail("failed=" + ",".join(failed))
    print("P4_AUDIO_TIMER_TOPOLOGY_AUDIT=PASS "
          f"timer_core={timer_affinity} timer_priority={timer_priority} "
          f"consumer_core={consumer_core} consumer_priority={consumer_priority} "
          f"max_priorities={max_priorities}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
