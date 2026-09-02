#!/usr/bin/env python3
"""Validate real-FreeRTOS physical-start failure evidence from esp-emu."""

import argparse
from pathlib import Path


def fields(line: str) -> dict[str, str]:
    return dict(item.split("=", 1) for item in line.split()[1:] if "=" in item)


def integer(record: dict[str, str], name: str) -> int:
    try:
        return int(record[name], 0)
    except (KeyError, ValueError) as error:
        raise ValueError(f"invalid/missing {name}") from error


def validate(text: str, stage: int) -> list[str]:
    errors: list[str] = []
    scenario = f"start_fatal_{stage}"
    evidence = [fields(line) for line in text.splitlines()
                if line.startswith("5D1_EVIDENCE ") and
                f"scenario={scenario} " in line]
    backend = [fields(line) for line in text.splitlines()
               if line.startswith("5D1_FAKE_BACKEND ") and
               f"scenario={scenario} " in line]
    history = [fields(line) for line in text.splitlines()
               if line.startswith("5D1_HISTORY ") and
               f"scenario={scenario} " in line]
    if len(evidence) != 1 or len(backend) != 1:
        return ["missing or duplicate lifecycle/backend record"]
    record = evidence[0]
    resource = backend[0]
    for item in [record, resource, *history]:
        if item.get("schema") != "2" or item.get("evidence_class") != "ESP_EMU_EXEC":
            errors.append("schema/evidence class mismatch")
    for name in ("start_fatal", "ready_wait", "forced_abort", "terminal_ack",
                 "consumer_quiescent", "terminal_wait", "owner_suspended",
                 "delete_performed", "sink_destroy_performed"):
        if integer(record, name) != 1:
            errors.append(f"{name} mismatch")
    if integer(record, "first_error") != 2:
        errors.append("first_error mismatch")
    for name in ("callback_residual", "resource_residual", "pa_high",
                 "i2c_residual"):
        if integer(record, name) != 0:
            errors.append(f"{name} mismatch")
    for name in ("i2s", "callbacks", "i2c", "codec", "pa_high"):
        if integer(resource, name) != 0:
            errors.append(f"backend {name} residual")
    if integer(resource, "released") != 1 or integer(resource, "destroyed") != 1:
        errors.append("backend release/destroy mismatch")
    operations = [item.get("operation", "") for item in history]
    prefix = ["PREPARE_BEGIN"]
    if stage >= 2:
        prefix.append("I2S_CREATE")
    if stage >= 3:
        prefix.append("CALLBACK_REGISTER")
    if stage >= 4:
        prefix.extend(["I2C_ACQUIRE", "CODEC_CONFIG", "PA_HIGH"])
    expected = prefix + ["PREPARE_FAIL", "CODEC_MUTE", "PA_LOW", "DISABLE",
                         "DELETE_BEGIN", "DELETE_END", "I2C_RELEASE", "DESTROY"]
    if operations != expected:
        errors.append(f"operation order mismatch: {operations!r}")
    for index, item in enumerate(history):
        if integer(item, "sequence") != index:
            errors.append("history sequence mismatch")
        for name in ("generation", "result", "bytes"):
            integer(item, name)
    if text.count("5D1_ESP_EMU_LIFECYCLE_RESULT=PASS") != 1:
        errors.append("computed lifecycle result missing")
    if text.count("P4_NANO_AUDIO86_REAL_GUEST_STATUS=PASS") != 1:
        errors.append("profile status missing")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", type=int, choices=range(1, 5), required=True)
    parser.add_argument("--log", type=Path, required=True)
    args = parser.parse_args()
    try:
        errors = validate(args.log.read_text(encoding="utf-8", errors="replace"),
                          args.stage)
    except ValueError as error:
        errors = [str(error)]
    if errors:
        for error in errors:
            print(f"5D1_ESP_EMU_VALIDATOR error={error}")
        return 1
    print(f"START_FATAL_STAGE_{args.stage}_ESP_EMU=PASS")
    print("START_FAILURE_ESUSPENDED_OBSERVED_REAL=PASS")
    print("START_FAILURE_RUNTIME_EVIDENCE_OBSERVED_NOT_SYNTHESIZED=PASS")
    print("OWNER_START_FAILURE_RUNTIME_EVIDENCE=PASS")
    print("FAKE_BACKEND_OPERATION_HISTORY=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
