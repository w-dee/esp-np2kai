#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly FIRMWARE_DIR="${REPOSITORY_ROOT}/firmware"
readonly BUILD_DIR="${P4_V3X_BUILD_DIR:-${FIRMWARE_DIR}/build-p4-v3x}"
readonly MERGED_IMAGE="${BUILD_DIR}/merged-binary.bin"
readonly EMULATOR_LOG="${BUILD_DIR}/esp-emu-hello-world.log"
readonly SUCCESS_MARKER="ESP-NP2KAI HELLO WORLD OK"
readonly ESP_EMU="${ESP_EMU:-${HOME}/.local/bin/esp-emu}"

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

source "${SCRIPT_DIR}/activate-idf.sh"
source "${SCRIPT_DIR}/check-firmware-sdkconfig.sh"

if [[ -z "${IDF_PATH:-}" ]]; then
    fail 'IDF_PATH is not set after ESP-IDF activation'
fi

case "${IDF_PATH}" in
    *'/.platformio/'*|*'/.platformio')
        fail "PlatformIO ESP-IDF environment is not supported: ${IDF_PATH}"
        ;;
esac

idf_command_path="$(command -v idf.py || true)"
if [[ -z "${idf_command_path}" ]]; then
    fail 'idf.py is not available after ESP-IDF activation'
fi

case "${idf_command_path}" in
    *'/.platformio/'*|*'/.platformio')
        fail "PlatformIO idf.py is not supported: ${idf_command_path}"
        ;;
esac

idf_version="$(idf.py --version)"
printf '%s\n' "${idf_version}"
if [[ "${idf_version}" != *'v5.5.4'* ]]; then
    fail "ESP-IDF v5.5.4 is required; detected: ${idf_version}"
fi

if [[ ! -x "${ESP_EMU}" ]]; then
    fail "esp-emu executable not found or not executable: ${ESP_EMU}"
fi

emu_version_output="$("${ESP_EMU}" --version 2>&1)"
emu_version_line="$(printf '%s\n' "${emu_version_output}" | sed -n '/^esp-emu /{p;q;}')"
printf '%s\n' "${emu_version_line}"
if [[ "${emu_version_line}" != 'esp-emu 0.39.0' ]]; then
    fail "esp-emu v0.39.0 is required; detected: ${emu_version_line:-version unavailable}"
fi

bash "${SCRIPT_DIR}/build-production.sh" \
    --variant p4-v3x \
    --esp-emu-test \
    --build-dir "${BUILD_DIR}"

cd -- "${FIRMWARE_DIR}"
idf.py -B "${BUILD_DIR}" merge-bin -f raw -o "${MERGED_IMAGE}"

if [[ ! -f "${MERGED_IMAGE}" ]]; then
    fail "merged image was not created: ${MERGED_IMAGE}"
fi

mkdir -p -- "${BUILD_DIR}"
set +e
"${ESP_EMU}" \
    --chip esp32p4 \
    --firmware "${MERGED_IMAGE}" \
    --exit-on "${SUCCESS_MARKER}" \
    --timeout 15s \
    --log-color never \
    2>&1 | tee "${EMULATOR_LOG}"
emulator_status="${PIPESTATUS[0]}"
set -e

if (( emulator_status != 0 )); then
    printf 'ERROR: esp-emu failed with status %d. Output: %s\n' "${emulator_status}" "${EMULATOR_LOG}" >&2
    exit "${emulator_status}"
fi

if ! grep -Fq -- "${SUCCESS_MARKER}" "${EMULATOR_LOG}"; then
    printf 'ERROR: success marker was not found in %s\n' "${EMULATOR_LOG}" >&2
    exit 1
fi

printf '%s\n' 'PASS: ESP-NP2KAI HELLO WORLD OK'
