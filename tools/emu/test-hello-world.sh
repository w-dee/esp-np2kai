#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly FIRMWARE_DIR="${REPOSITORY_ROOT}/firmware"
readonly BUILD_DIR="${FIRMWARE_DIR}/build"
readonly MERGED_IMAGE="${BUILD_DIR}/merged-binary.bin"
readonly EMULATOR_LOG="${BUILD_DIR}/esp-emu-hello-world.log"
readonly SUCCESS_MARKER="ESP-NP2KAI HELLO WORLD OK"
readonly IDF_ACTIVATION_SCRIPT="${HOME}/.espressif/tools/activate_idf_v5.5.4.sh"
readonly ESP_EMU="${HOME}/.local/bin/esp-emu"

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

if [[ ! -f "${IDF_ACTIVATION_SCRIPT}" ]]; then
    fail "ESP-IDF activation script not found: ${IDF_ACTIVATION_SCRIPT}"
fi

# shellcheck disable=SC1090
source "${IDF_ACTIVATION_SCRIPT}"

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

if [[ -f "${FIRMWARE_DIR}/sdkconfig" ]]; then
    if ! grep -qx 'CONFIG_IDF_TARGET="esp32p4"' "${FIRMWARE_DIR}/sdkconfig"; then
        configured_target="$(grep -E '^CONFIG_IDF_TARGET=' "${FIRMWARE_DIR}/sdkconfig" || true)"
        fail "firmware/sdkconfig is not configured for esp32p4: ${configured_target:-target is unset}. Run 'idf.py set-target esp32p4' once explicitly."
    fi
else
    printf '%s\n' 'firmware/sdkconfig is absent; the build will use sdkconfig.defaults.'
fi

cd -- "${FIRMWARE_DIR}"
idf.py build

if ! grep -qx 'CONFIG_IDF_TARGET="esp32p4"' sdkconfig; then
    configured_target="$(grep -E '^CONFIG_IDF_TARGET=' sdkconfig || true)"
    fail "generated firmware/sdkconfig is not configured for esp32p4: ${configured_target:-target is unset}"
fi

idf.py merge-bin -f raw -o merged-binary.bin

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

