#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly FIRMWARE_DIR="${REPOSITORY_ROOT}/firmware"
readonly PRODUCTION_DEFAULTS="${FIRMWARE_DIR}/sdkconfig.defaults;${FIRMWARE_DIR}/sdkconfig.defaults.p4-v3x"
if [[ -n "${NP2_OPNGEN_RUN_ROOT:-}" ]]; then
    readonly RUN_ROOT="${NP2_OPNGEN_RUN_ROOT}"
else
    readonly RUN_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/np2opngen.XXXXXX")"
fi
readonly BUILD_DIR="${RUN_ROOT}/build"
readonly SDKCONFIG_PATH="${BUILD_DIR}/sdkconfig"
readonly MERGED_IMAGE="${RUN_ROOT}/np2opngen-merged.bin"
readonly EMULATOR_LOG="${RUN_ROOT}/esp-emu-opngen.log"
readonly ESP_EMU="${ESP_EMU:-${HOME}/.local/bin/esp-emu}"

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    printf 'E1_OPNGEN_RUN_ROOT=%s\n' "${RUN_ROOT}" >&2
    exit 1
}

if [[ -e "${BUILD_DIR}" ]]; then
    fail "OPNGEN build directory is not fresh: ${BUILD_DIR}"
fi
if [[ ! -x "${ESP_EMU}" ]]; then
    fail "esp-emu executable not found: ${ESP_EMU}"
fi

unset NP2_VIDEO_PROFILE NP2_PRESENTATION_PROFILE
export NP2_OPNGEN_FIXTURE_PROFILE=1
source "${SCRIPT_DIR}/activate-idf.sh"
[[ -n "${IDF_PATH:-}" ]] || fail 'IDF_PATH is not set after ESP-IDF activation'
idf_version="$(idf.py --version)"
printf '%s\n' "${idf_version}"
[[ "${idf_version}" == *'v5.5.4'* ]] || fail "ESP-IDF v5.5.4 is required: ${idf_version}"
emu_version_line="$(${ESP_EMU} --version 2>&1 | sed -n '/^esp-emu /{p;q;}')"
printf '%s\n' "${emu_version_line}"
[[ "${emu_version_line}" == 'esp-emu 0.39.0' ]] || fail "esp-emu v0.39.0 is required: ${emu_version_line}"

cd -- "${FIRMWARE_DIR}"
idf.py -B "${BUILD_DIR}" \
    -D "SDKCONFIG=${SDKCONFIG_PATH}" \
    -D "SDKCONFIG_DEFAULTS=${PRODUCTION_DEFAULTS}" \
    -D "NP2_OPNGEN_FIXTURE_PROFILE=1" \
    -D "NP2_VIDEO_PROFILE=0" \
    set-target esp32p4
idf.py -B "${BUILD_DIR}" \
    -D "SDKCONFIG=${SDKCONFIG_PATH}" \
    -D "SDKCONFIG_DEFAULTS=${PRODUCTION_DEFAULTS}" \
    -D "NP2_OPNGEN_FIXTURE_PROFILE=1" \
    -D "NP2_VIDEO_PROFILE=0" \
    build

python3 "${IDF_PATH}/components/esptool_py/esptool/esptool.py" \
    --chip esp32p4 merge_bin \
    --output "${MERGED_IMAGE}" \
    --format raw \
    --flash_mode dio \
    --flash_freq 80m \
    --flash_size 8MB \
    --fill-flash-size 8MB \
    0x2000 "${BUILD_DIR}/bootloader/bootloader.bin" \
    0x10000 "${BUILD_DIR}/esp_np2kai.bin" \
    0x8000 "${BUILD_DIR}/partition_table/partition-table.bin"

set +e
timeout --foreground 150s "${ESP_EMU}" \
    --chip esp32p4 \
    --firmware "${MERGED_IMAGE}" \
    --exit-on 'E1_OPNGEN_RESULT=' \
    --timeout 120s \
    --log-color never \
    2>&1 | tee "${EMULATOR_LOG}"
emulator_status="${PIPESTATUS[0]}"
set -e
(( emulator_status == 0 )) || fail "esp-emu returned status ${emulator_status}; output: ${EMULATOR_LOG}"
python3 "${REPOSITORY_ROOT}/tools/emu/validate_opngen_fixture_log.py" \
    --log "${EMULATOR_LOG}" || fail "OPNGEN fixture log validation failed"
printf 'E1_OPNGEN_HARNESS=PASS\n'
printf 'E1_OPNGEN_RUN_ROOT=%s\n' "${RUN_ROOT}"
