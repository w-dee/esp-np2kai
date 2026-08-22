#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly BUILD_DIR="${P4_V3X_BUILD_DIR:-${REPOSITORY_ROOT}/firmware/build-p4-v3x}"
readonly MERGED_IMAGE="${BUILD_DIR}/merged-binary.bin"

python3 "${SCRIPT_DIR}/file_transfer_final_ack_replay_test.py" \
    --firmware "${MERGED_IMAGE}" \
    --log "${BUILD_DIR}/esp-emu-file-transfer-final-ack-replay.log" \
    --uart-log "${BUILD_DIR}/esp-emu-file-transfer-final-ack-replay.uart.bin"
