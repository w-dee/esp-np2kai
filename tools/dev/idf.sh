#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly FIRMWARE_DIR="${REPOSITORY_ROOT}/firmware"

if [[ ! -f "${FIRMWARE_DIR}/CMakeLists.txt" ]]; then
    printf 'ERROR: ESP-IDF project directory is missing: %s\n' "${FIRMWARE_DIR}" >&2
    exit 1
fi

idf_tool=""
if command -v idf.py >/dev/null 2>&1; then
    idf_tool="$(command -v idf.py)"
elif [[ -n "${IDF_PATH:-}" && -x "${IDF_PATH}/tools/idf.py" ]]; then
    idf_tool="${IDF_PATH}/tools/idf.py"
fi

if [[ -z "${idf_tool}" ]]; then
    printf 'ERROR: idf.py is unavailable; activate the ESP-IDF v5.5.4 environment first.\n' >&2
    exit 1
fi

"${idf_tool}" -C "${FIRMWARE_DIR}" "$@"
