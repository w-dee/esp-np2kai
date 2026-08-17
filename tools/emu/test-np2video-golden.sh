#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly FIRMWARE_DIR="${REPOSITORY_ROOT}/firmware"
readonly VIDEO_RUN_ROOT="${NP2_VIDEO_RUN_ROOT:-$(mktemp -d "${TMPDIR:-/tmp}/np2video-golden.XXXXXX")}"
readonly BUILD_DIR="${VIDEO_RUN_ROOT}/build"
readonly SDKCONFIG_PATH="${BUILD_DIR}/sdkconfig"
readonly GENERATED_DIR="${BUILD_DIR}/generated"
readonly GOLDEN_HEADER="${GENERATED_DIR}/np2video_golden.h"
readonly BOOTSTRAP_HEADER="${VIDEO_RUN_ROOT}/np2video_golden.bootstrap.h"
readonly FIXTURE_DIR="${VIDEO_RUN_ROOT}/fixture"
readonly FIXTURE_IMAGE="${FIXTURE_DIR}/np2video-fd1232.image"
readonly MERGED_IMAGE="${VIDEO_RUN_ROOT}/np2video-merged.bin"
readonly EMULATOR_LOG="${VIDEO_RUN_ROOT}/esp-emu-np2video-golden.log"
readonly ESP_EMU="${ESP_EMU:-${HOME}/.local/bin/esp-emu}"
readonly APP_LIMIT=$((0x100000))

export NP2_VIDEO_PROFILE=1

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    printf 'NP2VIDEO_RUN_ROOT=%s\n' "${VIDEO_RUN_ROOT}" >&2
    exit 1
}

if [[ -e "${BUILD_DIR}" ]]; then
    fail "video build directory is not fresh: ${BUILD_DIR}"
fi
if [[ ! -x "${ESP_EMU}" ]]; then
    fail "esp-emu executable not found: ${ESP_EMU}"
fi

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

mkdir -p "${FIXTURE_DIR}"
mkdir -p "$(dirname -- "${BOOTSTRAP_HEADER}")"
python3 "${REPOSITORY_ROOT}/tools/guest/build_np2video.py" \
    --layout "${REPOSITORY_ROOT}/tests/guest/np2video/layout.json" \
    --output "${FIXTURE_IMAGE}"
python3 "${REPOSITORY_ROOT}/tools/guest/generate_np2video_golden_header.py" \
    --descriptor "${REPOSITORY_ROOT}/tests/guest/np2video/golden.json" \
    --output "${BOOTSTRAP_HEADER}"
export NP2VIDEO_GOLDEN_HEADER="${BOOTSTRAP_HEADER}"

cd -- "${FIRMWARE_DIR}"
idf.py -B "${BUILD_DIR}" \
    -D "SDKCONFIG=${SDKCONFIG_PATH}" \
    -D "NP2_VIDEO_PROFILE=1" \
    -D "NP2_REDUCED_EXTMEM8=1" \
    -D "NP2VIDEO_GOLDEN_HEADER=${BOOTSTRAP_HEADER}" \
    set-target esp32p4
check_firmware_sdkconfig "${SDKCONFIG_PATH}"
mkdir -p "${GENERATED_DIR}"
python3 "${REPOSITORY_ROOT}/tools/guest/generate_np2video_golden_header.py" \
    --descriptor "${REPOSITORY_ROOT}/tests/guest/np2video/golden.json" \
    --output "${GOLDEN_HEADER}"
export NP2VIDEO_GOLDEN_HEADER="${GOLDEN_HEADER}"
idf.py -B "${BUILD_DIR}" \
    -D "SDKCONFIG=${SDKCONFIG_PATH}" \
    -D "NP2_VIDEO_PROFILE=1" \
    -D "NP2_REDUCED_EXTMEM8=1" \
    -D "NP2VIDEO_GOLDEN_HEADER=${GOLDEN_HEADER}" \
    reconfigure
check_firmware_sdkconfig "${SDKCONFIG_PATH}"
idf.py -B "${BUILD_DIR}" \
    -D "SDKCONFIG=${SDKCONFIG_PATH}" \
    -D "NP2_VIDEO_PROFILE=1" \
    -D "NP2_REDUCED_EXTMEM8=1" \
    -D "NP2VIDEO_GOLDEN_HEADER=${GOLDEN_HEADER}" \
    build

app_bin="${BUILD_DIR}/esp_np2kai.bin"
app_size="$(stat -c '%s' "${app_bin}")"
app_headroom=$((APP_LIMIT - app_size))
printf 'NP2VIDEO_APP size=%s limit=%s headroom=%s\n' \
    "${app_size}" "${APP_LIMIT}" "${app_headroom}"
(( app_size <= APP_LIMIT )) || fail "video app exceeds factory partition"

python3 "${REPOSITORY_ROOT}/tools/emu/build_np2video_fixture_flash.py" \
    --repository-root "${REPOSITORY_ROOT}" \
    --build-dir "${BUILD_DIR}" \
    --descriptor "${REPOSITORY_ROOT}/tests/guest/np2video/golden.json" \
    --fixture "${FIXTURE_IMAGE}" \
    --output "${MERGED_IMAGE}"

set +e
timeout --foreground 150s "${ESP_EMU}" \
    --chip esp32p4 \
    --firmware "${MERGED_IMAGE}" \
    --exit-on 'NP2VIDEO_GOLDEN_RESULT=' \
    --timeout 120s \
    --log-color never \
    2>&1 | tee "${EMULATOR_LOG}"
emulator_status="${PIPESTATUS[0]}"
set -e

if (( emulator_status != 0 )); then
    fail "esp-emu returned status ${emulator_status}; output: ${EMULATOR_LOG}"
fi
result_count="$(grep -Ec '^NP2VIDEO_GOLDEN_RESULT=(PASS|FAIL|HARNESS_ERROR)' \
    "${EMULATOR_LOG}" || true)"
pass_count="$(grep -Ec '^NP2VIDEO_GOLDEN_RESULT=PASS$' \
    "${EMULATOR_LOG}" || true)"
(( result_count == 1 )) || fail "expected exactly one terminal video result"
(( pass_count == 1 )) || fail "video golden did not pass"
grep -Eq '^NP2VIDEO_MEMORY extmem_mb=8 actual_bytes=8388608 ptr_external=1 ' "${EMULATOR_LOG}" ||
    fail "external EXTMEM evidence is missing"
grep -Fqx 'NP2VIDEO_FIXTURE scene_id=1 fixture_sha256=f4ae6584339cbdb94e80e6fb48f9a27724fee7a9f350668b618d33b2794c8eca image_bytes=1261568 partition=np2test' "${EMULATOR_LOG}" ||
    fail "fixture identity evidence is missing"
grep -Fq 'NP2VIDEO_READY scene_id=1 state=SCENE_READY' "${EMULATOR_LOG}" ||
    fail "PRE-READY evidence is missing"
grep -Fq 'NP2VIDEO_FRAMEBUFFER scene_id=1 width=640 height=400 bytes=512000 format=rgb565le bpp=16 pitch=1280' "${EMULATOR_LOG}" ||
    fail "framebuffer metadata evidence is missing"
grep -Fq 'crc_algorithm=crc32_iso_hdlc crc32=0x0a280896 storage_external=1' "${EMULATOR_LOG}" ||
    fail "framebuffer CRC evidence is missing"

printf 'NP2VIDEO_RESULT=PASS\n'
printf 'NP2VIDEO_RUN_ROOT=%s\n' "${VIDEO_RUN_ROOT}"
