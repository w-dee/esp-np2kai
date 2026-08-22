#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
readonly TEMP_DIR="$(mktemp -d)"
trap 'rm -rf -- "${TEMP_DIR}"' EXIT

c++ -std=c++20 -Wall -Wextra -Werror \
    -I "${REPOSITORY_ROOT}/firmware/components/file_transfer/include" \
    "${REPOSITORY_ROOT}/firmware/components/file_transfer/zero_rle_v1.cpp" \
    "${SCRIPT_DIR}/test_zero_rle_v1.cpp" \
    -o "${TEMP_DIR}/test_zero_rle_v1"

"${TEMP_DIR}/test_zero_rle_v1" \
    "${REPOSITORY_ROOT}/tests/guest/np2test/golden/np2test-fd1232.image" \
    "${TEMP_DIR}/np2test-fd1232.decoded"

readonly EXPECTED_SHA256="3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3"
actual_sha256="$(sha256sum "${TEMP_DIR}/np2test-fd1232.decoded" | awk '{print $1}')"
if [[ "${actual_sha256}" != "${EXPECTED_SHA256}" ]]; then
    printf 'ERROR: decoded NP2TEST SHA-256 mismatch: %s\n' "${actual_sha256}" >&2
    exit 1
fi
printf '%s\n' 'PASS: zero-rle-v1 codec and NP2TEST regression'
