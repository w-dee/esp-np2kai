#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
readonly TEST_ROOT="${REPOSITORY_ROOT}/tools"
readonly CXX="${CXX:-g++}"

if ! command -v "${CXX}" >/dev/null 2>&1; then
    printf 'ERROR: C++ compiler not found: %s\n' "${CXX}" >&2
    exit 1
fi

readonly TEMP_ROOT="$(mktemp -d)"
trap 'rm -rf "${TEMP_ROOT}"' EXIT

"${CXX}" -std=c++20 -Wall -Wextra -Werror \
    -I"${REPOSITORY_ROOT}/firmware/components/control_stream/include" \
    -I"${REPOSITORY_ROOT}/firmware/components/control_plane/include" \
    -I"${REPOSITORY_ROOT}/firmware/components/binary_data_plane/include" \
    -I"${REPOSITORY_ROOT}/firmware/components/binary_data_plane" \
    "${TEST_ROOT}/control_stream_sync_test.cpp" \
    "${REPOSITORY_ROOT}/firmware/components/control_stream/control_stream.cpp" \
    "${REPOSITORY_ROOT}/firmware/components/binary_data_plane/binary_codec.cpp" \
    "${REPOSITORY_ROOT}/firmware/components/binary_data_plane/cobs.cpp" \
    "${REPOSITORY_ROOT}/firmware/components/binary_data_plane/crc32.cpp" \
    -o "${TEMP_ROOT}/control_stream_sync_test"

"${TEMP_ROOT}/control_stream_sync_test"
printf '%s\n' 'PASS: control_stream TRANSPORT_SYNC unit and randomized recovery tests'
