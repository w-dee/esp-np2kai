#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly BUILD_DIR="${REPOSITORY_ROOT}/firmware/build"
readonly MERGED_IMAGE="${BUILD_DIR}/merged-binary.bin"

printf '%s\n' 'Running the existing Binary Data Plane regression first.'
"${SCRIPT_DIR}/test-uart-binary-data-plane.sh"

python3 "${SCRIPT_DIR}/file_transfer_base_test.py" \
    --firmware "${MERGED_IMAGE}" \
    --log "${BUILD_DIR}/esp-emu-file-transfer-base.log" \
    --uart-log "${BUILD_DIR}/esp-emu-file-transfer-base.uart.bin"
