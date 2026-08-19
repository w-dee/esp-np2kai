#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly FIRMWARE_DIR="${REPOSITORY_ROOT}/firmware"
readonly BUILD_DIR="${P4_V3X_BUILD_DIR:-${FIRMWARE_DIR}/build-p4-v3x}"
readonly MERGED_IMAGE="${BUILD_DIR}/np2fixture-merged.bin"
readonly EMULATOR_LOG="${BUILD_DIR}/esp-emu-np2-fixture.log"
readonly ESP_EMU="${ESP_EMU:-${HOME}/.local/bin/esp-emu}"
readonly FIXTURE_RESULT_PREFIX="NP2FIXTURE_RESULT="
readonly EXPECTED_SHA256="3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3"

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

source "${SCRIPT_DIR}/activate-idf.sh"
source "${SCRIPT_DIR}/check-firmware-sdkconfig.sh"

[[ -n "${IDF_PATH:-}" ]] || fail 'IDF_PATH is not set after ESP-IDF activation'
idf_version="$(idf.py --version)"
printf '%s\n' "${idf_version}"
[[ "${idf_version}" == *'v5.5.4'* ]] || fail "ESP-IDF v5.5.4 is required: ${idf_version}"
[[ -x "${ESP_EMU}" ]] || fail "esp-emu executable not found: ${ESP_EMU}"
emu_version_line="$(${ESP_EMU} --version 2>&1 | sed -n '/^esp-emu /{p;q;}')"
printf '%s\n' "${emu_version_line}"
[[ "${emu_version_line}" == 'esp-emu 0.39.0' ]] || fail "esp-emu v0.39.0 is required: ${emu_version_line}"

python3 "${REPOSITORY_ROOT}/tools/guest/verify_np2test.py" \
    --layout "${REPOSITORY_ROOT}/tests/guest/np2test/layout.json" \
    --image "${REPOSITORY_ROOT}/tests/guest/np2test/golden/np2test-fd1232.image" \
    --expected-sha256 "${EXPECTED_SHA256}"

bash "${SCRIPT_DIR}/build-production.sh" \
    --variant p4-v3x \
    --build-dir "${BUILD_DIR}"

python3 "${REPOSITORY_ROOT}/tools/emu/build_np2_fixture_flash.py" \
    --repository-root "${REPOSITORY_ROOT}" \
    --build-dir "${BUILD_DIR}" \
    --output "${MERGED_IMAGE}"

set +e
"${ESP_EMU}" \
    --chip esp32p4 \
    --firmware "${MERGED_IMAGE}" \
    --exit-on "${FIXTURE_RESULT_PREFIX}" \
    --timeout 20s \
    --log-color never \
    2>&1 | tee "${EMULATOR_LOG}"
emulator_status="${PIPESTATUS[0]}"
set -e

if (( emulator_status != 0 )); then
    fail "esp-emu failed with status ${emulator_status}; output: ${EMULATOR_LOG}"
fi
grep -Eq $'NP2FIXTURE_RESULT=PASS\r?$' "${EMULATOR_LOG}" ||
    fail "fixture probe did not pass; output: ${EMULATOR_LOG}"
grep -Fq "NP2FIXTURE sha256=${EXPECTED_SHA256}" "${EMULATOR_LOG}" ||
    fail "runtime fixture SHA-256 evidence is missing or incorrect"
grep -Fq 'NP2FIXTURE geometry_tracks=154 sectors=8 n=3 disktype=2' "${EMULATOR_LOG}" ||
    fail "runtime XDF geometry evidence is missing"

printf '%s\n' 'PASS: NP2 raw fixture partition and non-CPU FDD recognition'
