#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly FIRMWARE_DIR="${REPOSITORY_ROOT}/firmware"
readonly RUN_ROOT="${NP2_PRESENTATION_RUN_ROOT:-$(mktemp -d "${TMPDIR:-/tmp}/np2presentation.XXXXXX")}"
readonly BUILD_DIR="${RUN_ROOT}/build"
readonly SDKCONFIG_PATH="${BUILD_DIR}/sdkconfig"
readonly MERGED_IMAGE="${RUN_ROOT}/np2presentation-merged.bin"
readonly EMULATOR_LOG="${RUN_ROOT}/esp-emu-np2presentation.log"
readonly ESP_EMU="${ESP_EMU:-${HOME}/.local/bin/esp-emu}"

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    printf 'NP2PRESENT_RUN_ROOT=%s\n' "${RUN_ROOT}" >&2
    exit 1
}

load_factory_geometry() {
    local geometry_output
    geometry_output="$(python3 "${REPOSITORY_ROOT}/tools/emu/partition_geometry.py" \
        --idf-path "${IDF_PATH}" \
        --partition-table "${BUILD_DIR}/partition_table/partition-table.bin")" ||
        fail 'cannot extract generated partition geometry'

    FACTORY_OFFSET="$(printf '%s\n' "${geometry_output}" |
        sed -n 's/^FACTORY_OFFSET=//p')"
    FACTORY_SIZE="$(printf '%s\n' "${geometry_output}" |
        sed -n 's/^FACTORY_SIZE=//p')"
    [[ -n "${FACTORY_OFFSET}" && -n "${FACTORY_SIZE}" ]] ||
        fail 'generated partition geometry has no factory size'
}

if [[ -e "${BUILD_DIR}" ]]; then
    fail "presentation build directory is not fresh: ${BUILD_DIR}"
fi
if [[ ! -x "${ESP_EMU}" ]]; then
    fail "esp-emu executable not found: ${ESP_EMU}"
fi

unset NP2_VIDEO_PROFILE NP2VIDEO_GOLDEN_HEADER
export NP2_PRESENTATION_PROFILE=1
source "${SCRIPT_DIR}/activate-idf.sh"
source "${SCRIPT_DIR}/check-firmware-sdkconfig.sh"
[[ -n "${IDF_PATH:-}" ]] || fail 'IDF_PATH is not set after ESP-IDF activation'
idf_version="$(idf.py --version)"
printf '%s\n' "${idf_version}"
[[ "${idf_version}" == *'v5.5.4'* ]] ||
    fail "ESP-IDF v5.5.4 is required: ${idf_version}"
emu_version_line="$(${ESP_EMU} --version 2>&1 | sed -n '/^esp-emu /{p;q;}')"
printf '%s\n' "${emu_version_line}"
[[ "${emu_version_line}" == 'esp-emu 0.39.0' ]] ||
    fail "esp-emu v0.39.0 is required: ${emu_version_line}"

cd -- "${FIRMWARE_DIR}"
idf.py -B "${BUILD_DIR}" \
    -D "SDKCONFIG=${SDKCONFIG_PATH}" \
    -D "NP2_PRESENTATION_PROFILE=1" \
    -D "NP2_VIDEO_PROFILE=0" \
    set-target esp32p4
check_firmware_sdkconfig "${SDKCONFIG_PATH}"
idf.py -B "${BUILD_DIR}" \
    -D "SDKCONFIG=${SDKCONFIG_PATH}" \
    -D "NP2_PRESENTATION_PROFILE=1" \
    -D "NP2_VIDEO_PROFILE=0" \
    reconfigure
check_firmware_sdkconfig "${SDKCONFIG_PATH}"
idf.py -B "${BUILD_DIR}" \
    -D "SDKCONFIG=${SDKCONFIG_PATH}" \
    -D "NP2_PRESENTATION_PROFILE=1" \
    -D "NP2_VIDEO_PROFILE=0" \
    build

app_bin="${BUILD_DIR}/esp_np2kai.bin"
app_size="$(stat -c '%s' "${app_bin}")"
load_factory_geometry
(( FACTORY_OFFSET == 0x10000 )) ||
    fail "presentation factory offset changed: 0x$(printf '%x' "${FACTORY_OFFSET}")"
app_headroom=$((FACTORY_SIZE - app_size))
printf 'NP2PRESENT_APP size=%s limit=%s headroom=%s factory_offset=0x%x factory_size=0x%x\n' \
    "${app_size}" "${FACTORY_SIZE}" "${app_headroom}" \
    "${FACTORY_OFFSET}" "${FACTORY_SIZE}"
(( app_size < FACTORY_SIZE )) || fail "presentation app does not fit factory partition"

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
    --exit-on 'NP2PRESENT_RESULT=' \
    --timeout 120s \
    --log-color never \
    2>&1 | tee "${EMULATOR_LOG}"
emulator_status="${PIPESTATUS[0]}"
set -e

if (( emulator_status != 0 )); then
    fail "esp-emu returned status ${emulator_status}; output: ${EMULATOR_LOG}"
fi
python3 "${REPOSITORY_ROOT}/tools/emu/validate_np2presentation_log.py" \
    --log "${EMULATOR_LOG}" || fail "presentation log validation failed"
printf 'NP2PRESENT_HARNESS=PASS\n'
printf 'NP2PRESENT_RUN_ROOT=%s\n' "${RUN_ROOT}"
