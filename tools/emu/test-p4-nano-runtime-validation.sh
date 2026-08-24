#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly FIRMWARE_DIR="${REPOSITORY_ROOT}/firmware"
readonly BUILD_DIR="${P4_NANO_RUNTIME_VALIDATION_BUILD_DIR:-${FIRMWARE_DIR}/build-runtime-validation-p4-v3x}"
readonly MERGED_IMAGE="${BUILD_DIR}/p4-nano-runtime-validation-merged.bin"
readonly EMULATOR_LOG="${BUILD_DIR}/p4-nano-runtime-validation.log"
readonly ESP_EMU="${ESP_EMU:-${HOME}/.local/bin/esp-emu}"

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

source "${SCRIPT_DIR}/activate-idf.sh"
[[ "$(idf.py --version)" == *'v5.5.4'* ]] ||
    fail "ESP-IDF v5.5.4 is required: $(idf.py --version)"
[[ -x "${ESP_EMU}" ]] || fail "esp-emu executable not found: ${ESP_EMU}"
[[ "$(${ESP_EMU} --version 2>&1 | sed -n '/^esp-emu /{p;q;}')" == \
    'esp-emu 0.39.0' ]] || fail "esp-emu 0.39.0 is required"

bash "${SCRIPT_DIR}/build-production.sh" \
    --variant p4-v3x --board generic --runtime-validation --esp-emu-test \
    --build-dir "${BUILD_DIR}"
python3 "${REPOSITORY_ROOT}/tools/emu/build_storage_fatfs_flash.py" \
    --repository-root "${REPOSITORY_ROOT}" \
    --build-dir "${BUILD_DIR}" \
    --output "${MERGED_IMAGE}"

set +e
timeout --foreground 60s "${ESP_EMU}" \
    --chip esp32p4 --firmware "${MERGED_IMAGE}" \
    --exit-on 'P4_NANO_RUNTIME_VALIDATION_EXIT=' --timeout 30s \
    --log-color never 2>&1 | tee "${EMULATOR_LOG}"
status="${PIPESTATUS[0]}"
set -e
(( status == 0 )) || fail "esp-emu failed with status ${status}"

grep -Fq 'P4_NANO_RUNTIME_SD_MOUNT=PASS mount=/persist' "${EMULATOR_LOG}" || fail 'SPI-NOR mount proof missing'
grep -Fq 'P4_NANO_RUNTIME_MEDIA result=FOUND path=/persist/fixtures/np2test-fd1232.hdm' "${EMULATOR_LOG}" || fail 'validation media proof missing'
grep -Fq 'P4_NANO_RUNTIME_DOSIO=READY logical=./runtime-validation-fdd0.hdm' "${EMULATOR_LOG}" || fail 'DOSIO mapping proof missing'
grep -Fq 'P4_NANO_RUNTIME_FDD0=ATTACHED type=autodetect readonly=1 fddequip=0x01' "${EMULATOR_LOG}" || fail 'FDD0 attach proof missing'
grep -Fq 'P4_NANO_RUNTIME_CORE=RUNNING' "${EMULATOR_LOG}" || fail 'runtime start proof missing'
grep -Fq 'P4_NANO_RUNTIME_DISPLAY=VISIBLE' "${EMULATOR_LOG}" || fail 'display/session proof missing'
grep -Eq 'P4_NANO_RUNTIME_DISK_READS opens=[1-9][0-9]* calls=[1-9][0-9]* bytes=[1-9][0-9]*' "${EMULATOR_LOG}" || fail 'disk read proof missing'
grep -Eq 'P4_NANO_RUNTIME_SESSION submitted=[1-9][0-9]* acquired=[1-9][0-9]* transformed=[1-9][0-9]* released=[1-9][0-9]*' "${EMULATOR_LOG}" || fail 'session counter proof missing'
grep -Fq 'P4_NANO_RUNTIME_VALIDATION_RESULT=PASS' "${EMULATOR_LOG}" || fail 'validation did not pass'

printf '%s\n' 'PASS: P4-NANO bounded runtime composition validation'
