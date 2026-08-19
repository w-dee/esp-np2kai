#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly FIRMWARE_DIR="${REPOSITORY_ROOT}/firmware"
readonly BUILD_DIR="${P4_V3X_BUILD_DIR:-${FIRMWARE_DIR}/build-p4-v3x}"
readonly MERGED_IMAGE="${BUILD_DIR}/merged-binary.bin"
readonly CONTROL_LOG="${BUILD_DIR}/esp-emu-uart-control-plane.log"

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

printf '%s\n' 'Running the existing Hello World regression first.'
"${SCRIPT_DIR}/test-hello-world.sh"

if [[ ! -f "${MERGED_IMAGE}" ]]; then
    fail "merged image was not created: ${MERGED_IMAGE}"
fi

if ! command -v python3 >/dev/null 2>&1; then
    fail 'python3 is required for the UART control-plane integration test'
fi

python3 "${SCRIPT_DIR}/uart_control_plane_test.py" \
    --firmware "${MERGED_IMAGE}" \
    --log "${CONTROL_LOG}"
