#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly FIRMWARE_DIR="${REPOSITORY_ROOT}/firmware"
readonly BUILD_DIR="${FIRMWARE_DIR}/build-reduced-extmem8"
readonly SDKCONFIG_PATH="${BUILD_DIR}/sdkconfig"
readonly MERGED_IMAGE="${BUILD_DIR}/np2fixture-reduced-merged.bin"
readonly EMULATOR_LOG="${BUILD_DIR}/esp-emu-np2-stage1-reduced.log"
readonly IDF_ACTIVATION_SCRIPT="${HOME}/.espressif/tools/activate_idf_v5.5.4.sh"
readonly ESP_EMU="${HOME}/.local/bin/esp-emu"
readonly EXPECTED_SHA256="3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3"
readonly REDUCED_PROFILE_MARKER="NP2REDUCED profile=reduced-extmem8 formal_extmem=13 effective_extmem=8"

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

if [[ ! -f "${IDF_ACTIVATION_SCRIPT}" ]]; then
    fail "ESP-IDF activation script not found: ${IDF_ACTIVATION_SCRIPT}"
fi
if [[ ! -x "${ESP_EMU}" ]]; then
    fail "esp-emu executable not found: ${ESP_EMU}"
fi

activate_idf() {
    local original_argv0="${BASH_ARGV0}"
    local activation_status
    eim() { :; }
    BASH_ARGV0=bash
    set +eu
    # shellcheck disable=SC1090
    source "${IDF_ACTIVATION_SCRIPT}"
    activation_status="$?"
    set -eu
    BASH_ARGV0="${original_argv0}"
    unset -f eim
    return "${activation_status}"
}

activate_idf
[[ -n "${IDF_PATH:-}" ]] || fail 'IDF_PATH is not set after ESP-IDF activation'
idf_version="$(idf.py --version)"
printf '%s\n' "${idf_version}"
[[ "${idf_version}" == *'v5.5.4'* ]] ||
    fail "ESP-IDF v5.5.4 is required: ${idf_version}"
emu_version_line="$(${ESP_EMU} --version 2>&1 | sed -n '/^esp-emu /{p;q;}')"
printf '%s\n' "${emu_version_line}"
[[ "${emu_version_line}" == 'esp-emu 0.39.0' ]] ||
    fail "esp-emu v0.39.0 is required: ${emu_version_line}"

python3 "${REPOSITORY_ROOT}/tools/guest/verify_np2test.py" \
    --layout "${REPOSITORY_ROOT}/tests/guest/np2test/layout.json" \
    --image "${REPOSITORY_ROOT}/tests/guest/np2test/golden/np2test-fd1232.image" \
    --expected-sha256 "${EXPECTED_SHA256}"

mkdir -p "${BUILD_DIR}"
cd -- "${FIRMWARE_DIR}"

# This cache entry is the only machine deviation. The ordinary firmware build
# and firmware/sdkconfig.defaults continue to describe formal EXTMEM=13.
# Keep the reduced profile's generated Kconfig file inside its dedicated build
# directory. SDKCONFIG is an idf.py/CMake definition; exporting it is ignored by
# this IDF version and would silently reuse firmware/sdkconfig.
if [[ ! -f "${SDKCONFIG_PATH}" ]]; then
    idf.py -B "${BUILD_DIR}" -D "SDKCONFIG=${SDKCONFIG_PATH}" set-target esp32p4
fi
idf.py -B "${BUILD_DIR}" \
    -D "SDKCONFIG=${SDKCONFIG_PATH}" \
    -D "NP2_REDUCED_EXTMEM8=1" \
    reconfigure
idf.py -B "${BUILD_DIR}" -D "SDKCONFIG=${SDKCONFIG_PATH}" build

python3 "${REPOSITORY_ROOT}/tools/emu/build_np2_fixture_flash.py" \
    --repository-root "${REPOSITORY_ROOT}" \
    --build-dir "${BUILD_DIR}" \
    --output "${MERGED_IMAGE}"

set +e
timeout --foreground 150s "${ESP_EMU}" \
    --chip esp32p4 \
    --firmware "${MERGED_IMAGE}" \
    --exit-on 'NP2REDUCED_RESULT=' \
    --timeout 120s \
    --log-color never \
    2>&1 | tee "${EMULATOR_LOG}"
emulator_status="${PIPESTATUS[0]}"
set -e

if grep -Eq 'NP2REDUCED_RESULT=PASS\r?$' "${EMULATOR_LOG}"; then
    (( emulator_status == 0 )) ||
        fail "reduced esp-emu returned status ${emulator_status}; output: ${EMULATOR_LOG}"
    grep -Fqx "${REDUCED_PROFILE_MARKER}" "${EMULATOR_LOG}" ||
        fail "reduced profile marker is missing or incorrect"
    grep -Fq 'NP2REDUCED_MEMORY extmem_mb=8 actual_bytes=8388608 ptr_external=1' \
        "${EMULATOR_LOG}" || fail "real EXTMEM=8 allocation evidence is missing"
    grep -Eq 'NP2REDUCED_PASS completed=13 passed=13 failed=0 stored_crc=0x58f5b827\r?$' \
        "${EMULATOR_LOG}" || fail "reduced PASS result block evidence is missing"
    if grep -q 'NP2TEST_RESULT=' "${EMULATOR_LOG}"; then
        fail 'reduced profile emitted the formal NP2TEST_RESULT namespace'
    fi
    grep -Eq 'NP2REDUCED_STACK configured_words=[0-9]+ high_water_words=[0-9]+\r?$' \
        "${EMULATOR_LOG}" || fail "task stack high-water evidence is missing"
    printf '%s\n' \
        'NON-FORMAL PASS: NP2TEST Stage-1 completed under explicit EXTMEM=8'
    exit 0
fi

if grep -Eq 'NP2REDUCED_RESULT=(FAIL|HARNESS_ERROR|NOT_REACHED|INVALID|RUNNING_TIMEOUT)\r?$' \
    "${EMULATOR_LOG}"; then
    printf 'REDUCED PROFILE TERMINAL FAILURE; evidence: %s\n' "${EMULATOR_LOG}" >&2
    exit 1
fi
if (( emulator_status == 124 )); then
    fail "RUNNING_TIMEOUT: external process timeout; output: ${EMULATOR_LOG}"
fi
if grep -Eiq 'panic|guru meditation|assert failed|abort' "${EMULATOR_LOG}"; then
    fail "CRASH_OR_PANIC: output: ${EMULATOR_LOG}"
fi
if (( emulator_status != 0 )); then
    fail "EMULATOR_PROCESS_ERROR status ${emulator_status}; output: ${EMULATOR_LOG}"
fi
fail "NOT_REACHED: reduced terminal marker was not observed; output: ${EMULATOR_LOG}"
