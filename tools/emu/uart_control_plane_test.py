#!/usr/bin/env python3
"""One-shot UART control-plane round-trip test for esp-emu v0.39.0."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


SUCCESS_MARKER = "ESP-NP2KAI HELLO WORLD OK"
READY_MARKER = "ESP-NP2KAI UART CONTROL READY"
FRAME_PREFIX = "@ESP-NP2 "
EXPECTED_CAPABILITIES = ["protocol.hello", "system.ping", "system.info"]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--esp-emu", default=os.path.expanduser("~/.local/bin/esp-emu"))
    return parser.parse_args()


def request(request_id: int, command: str) -> str:
    return json.dumps(
        {"v": 1, "id": request_id, "cmd": command},
        separators=(",", ":"),
    )


def protocol_lines(output: str) -> list[dict]:
    messages: list[dict] = []
    for line in output.splitlines():
        if not line.startswith(FRAME_PREFIX):
            continue
        payload = line[len(FRAME_PREFIX) :]
        try:
            messages.append(json.loads(payload))
        except json.JSONDecodeError as exc:
            raise AssertionError(f"malformed emitted protocol frame: {line!r}") from exc
    return messages


def main() -> int:
    args = parse_args()
    if not args.firmware.is_file():
        raise AssertionError(f"firmware image not found: {args.firmware}")

    malformed = '@ESP-NP2 {"v":1,"id":100,"cmd":'
    injected_lines = [
        malformed,
        request(101, "protocol.hello"),
        request(102, "system.ping"),
        request(103, "system.info"),
    ]
    # esp-emu's --inject option accepts escaped newline sequences. Keep the
    # payload as one argument so no shell parsing is involved.
    injected_payload = r"\n".join(f"{FRAME_PREFIX}{line}" for line in injected_lines) + r"\n"

    command = [
        args.esp_emu,
        "--chip",
        "esp32p4",
        "--firmware",
        str(args.firmware),
        "--inject-on",
        READY_MARKER,
        "--inject",
        injected_payload,
        "--exit-on",
        '@ESP-NP2 {"type":"response","v":1,"id":103,"ok":true,"result":{"project":"',
        "--timeout",
        "15s",
        "--log-color",
        "never",
    ]

    args.log.parent.mkdir(parents=True, exist_ok=True)
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env={**os.environ, "NO_COLOR": "1"},
    )
    try:
        output_bytes, _ = process.communicate(timeout=20)
    except subprocess.TimeoutExpired as exc:
        process.kill()
        output_bytes, _ = process.communicate()
        args.log.write_bytes(output_bytes)
        raise AssertionError("esp-emu UART control-plane test timed out") from exc

    output = output_bytes.decode("utf-8", errors="replace")
    args.log.write_bytes(output_bytes)

    if process.returncode != 0:
        raise AssertionError(f"esp-emu exited with status {process.returncode}")
    if SUCCESS_MARKER not in output:
        raise AssertionError("existing Hello World marker was not observed")
    if READY_MARKER not in output:
        raise AssertionError("UART control readiness marker was not observed")

    messages = protocol_lines(output)
    by_id: dict[int | None, dict] = {}
    for message in messages:
        message_id = message.get("id")
        if message_id in by_id:
            raise AssertionError(f"duplicate response id: {message_id!r}")
        by_id[message_id] = message

    malformed_response = by_id.get(None)
    if malformed_response is None or malformed_response.get("ok") is not False:
        raise AssertionError("malformed input did not produce an id:null error")
    if malformed_response.get("error", {}).get("code") != "MALFORMED_JSON":
        raise AssertionError("malformed input returned the wrong error code")

    hello = by_id.get(101)
    if hello is None or hello.get("ok") is not True:
        raise AssertionError("protocol.hello did not succeed")
    hello_result = hello.get("result", {})
    if hello_result.get("protocol_version") != 1:
        raise AssertionError("protocol.hello returned the wrong version")
    if hello_result.get("project") != "esp_np2kai":
        raise AssertionError("protocol.hello returned the wrong project")
    if hello_result.get("target") != "esp32p4":
        raise AssertionError("protocol.hello returned the wrong target")
    if hello_result.get("capabilities") != EXPECTED_CAPABILITIES:
        raise AssertionError("protocol.hello returned the wrong capabilities")

    ping = by_id.get(102)
    if ping is None or ping.get("ok") is not True or ping.get("result") != {"pong": True}:
        raise AssertionError("system.ping returned the wrong result")

    info = by_id.get(103)
    if info is None or info.get("ok") is not True:
        raise AssertionError("system.info did not succeed")
    info_result = info.get("result", {})
    if info_result.get("project") != "esp_np2kai":
        raise AssertionError("system.info returned the wrong project")
    if info_result.get("idf_version") != "v5.5.4":
        raise AssertionError("system.info returned the wrong IDF version")
    if info_result.get("target") != "esp32p4":
        raise AssertionError("system.info returned the wrong target")

    print("PASS: UART CONTROL PLANE ROUND TRIP OK")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
