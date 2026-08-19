#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly FIRMWARE_DIR="${REPOSITORY_ROOT}/firmware"
readonly PRODUCTION_DEFAULTS="${FIRMWARE_DIR}/sdkconfig.defaults;${FIRMWARE_DIR}/sdkconfig.defaults.p4-v3x"
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

export NP2_VIDEO_PROFILE=1

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    printf 'NP2VIDEO_RUN_ROOT=%s\n' "${VIDEO_RUN_ROOT}" >&2
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

usage() {
    printf 'usage: %s [--fixture text|gfx-vram|gdc]\n' "${BASH_SOURCE[0]}"
}

fixture_kind=text
while (($# > 0)); do
    case "$1" in
        --fixture)
            if (($# < 2)); then
                usage >&2
                exit 2
            fi
            fixture_kind="$2"
            shift 2
            ;;
        --fixture=*)
            fixture_kind="${1#*=}"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

case "${fixture_kind}" in
    text)
        readonly FIXTURE_DESCRIPTOR="${REPOSITORY_ROOT}/tests/guest/np2video/golden.json"
        readonly FIXTURE_LAYOUT="${REPOSITORY_ROOT}/tests/guest/np2video/layout.json"
        readonly FIXTURE_BUILDER="${REPOSITORY_ROOT}/tools/guest/build_np2video.py"
        ;;
    gfx-vram)
        readonly FIXTURE_DESCRIPTOR="${REPOSITORY_ROOT}/tests/guest/np2video-gfx-vram/golden.json"
        readonly FIXTURE_LAYOUT="${REPOSITORY_ROOT}/tests/guest/np2video-gfx-vram/layout.json"
        readonly FIXTURE_BUILDER="${REPOSITORY_ROOT}/tools/guest/build_np2video_stage2.py"
        ;;
    gdc)
        readonly FIXTURE_DESCRIPTOR="${REPOSITORY_ROOT}/tests/guest/np2video-gdc/golden.json"
        readonly FIXTURE_LAYOUT="${REPOSITORY_ROOT}/tests/guest/np2video-gdc/layout.json"
        readonly FIXTURE_BUILDER="${REPOSITORY_ROOT}/tools/guest/build_np2video_stage2.py"
        ;;
    *)
        fail "unsupported fixture: ${fixture_kind}"
        ;;
esac

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
python3 "${FIXTURE_BUILDER}" \
    --layout "${FIXTURE_LAYOUT}" \
    --output "${FIXTURE_IMAGE}"
python3 "${REPOSITORY_ROOT}/tools/guest/generate_np2video_golden_header.py" \
    --descriptor "${FIXTURE_DESCRIPTOR}" \
    --output "${BOOTSTRAP_HEADER}"
export NP2VIDEO_GOLDEN_HEADER="${BOOTSTRAP_HEADER}"

cd -- "${FIRMWARE_DIR}"
idf.py -B "${BUILD_DIR}" \
    -D "SDKCONFIG=${SDKCONFIG_PATH}" \
    -D "SDKCONFIG_DEFAULTS=${PRODUCTION_DEFAULTS}" \
    -D "NP2_VIDEO_PROFILE=1" \
    -D "NP2_REDUCED_EXTMEM8=1" \
    -D "NP2VIDEO_GOLDEN_HEADER=${BOOTSTRAP_HEADER}" \
    set-target esp32p4
check_firmware_sdkconfig "${SDKCONFIG_PATH}" p4-v3x
mkdir -p "${GENERATED_DIR}"
python3 "${REPOSITORY_ROOT}/tools/guest/generate_np2video_golden_header.py" \
    --descriptor "${FIXTURE_DESCRIPTOR}" \
    --output "${GOLDEN_HEADER}"
export NP2VIDEO_GOLDEN_HEADER="${GOLDEN_HEADER}"
idf.py -B "${BUILD_DIR}" \
    -D "SDKCONFIG=${SDKCONFIG_PATH}" \
    -D "SDKCONFIG_DEFAULTS=${PRODUCTION_DEFAULTS}" \
    -D "NP2_VIDEO_PROFILE=1" \
    -D "NP2_REDUCED_EXTMEM8=1" \
    -D "NP2VIDEO_GOLDEN_HEADER=${GOLDEN_HEADER}" \
    reconfigure
check_firmware_sdkconfig "${SDKCONFIG_PATH}" p4-v3x
idf.py -B "${BUILD_DIR}" \
    -D "SDKCONFIG=${SDKCONFIG_PATH}" \
    -D "SDKCONFIG_DEFAULTS=${PRODUCTION_DEFAULTS}" \
    -D "NP2_VIDEO_PROFILE=1" \
    -D "NP2_REDUCED_EXTMEM8=1" \
    -D "NP2VIDEO_GOLDEN_HEADER=${GOLDEN_HEADER}" \
    build

app_bin="${BUILD_DIR}/esp_np2kai.bin"
app_size="$(stat -c '%s' "${app_bin}")"
load_factory_geometry
app_headroom=$((FACTORY_SIZE - app_size))
printf 'NP2VIDEO_APP size=%s limit=%s headroom=%s factory_offset=0x%x factory_size=0x%x\n' \
    "${app_size}" "${FACTORY_SIZE}" "${app_headroom}" \
    "${FACTORY_OFFSET}" "${FACTORY_SIZE}"
(( app_size < FACTORY_SIZE )) || fail "video app does not fit factory partition"

python3 "${REPOSITORY_ROOT}/tools/emu/build_np2video_fixture_flash.py" \
    --repository-root "${REPOSITORY_ROOT}" \
    --build-dir "${BUILD_DIR}" \
    --descriptor "${FIXTURE_DESCRIPTOR}" \
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
python3 "${REPOSITORY_ROOT}/tools/guest/validate_np2video_esp_log.py" \
    --descriptor "${FIXTURE_DESCRIPTOR}" \
    --log "${EMULATOR_LOG}" ||
    fail "ESP np2video log validation failed"

printf 'NP2VIDEO_RESULT=PASS\n'
if [[ "${fixture_kind}" == gfx-vram ]]; then
    printf 'NP2VIDEO_GFX_VRAM_ESP_GOLDEN_RESULT=PASS\n'
elif [[ "${fixture_kind}" == gdc ]]; then
    printf 'NP2VIDEO_GDC_ESP_GOLDEN_RESULT=PASS\n'
fi
printf 'NP2VIDEO_RUN_ROOT=%s\n' "${VIDEO_RUN_ROOT}"
