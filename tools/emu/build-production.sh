#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly FIRMWARE_DIR="${REPOSITORY_ROOT}/firmware"

usage() {
    printf 'usage: %s --variant p4-v1x|p4-v3x [--build-dir PATH]\n' "${BASH_SOURCE[0]}"
}

variant=""
build_dir=""
while (($# > 0)); do
    case "$1" in
        --variant)
            (($# >= 2)) || { usage >&2; exit 2; }
            variant="$2"
            shift 2
            ;;
        --variant=*)
            variant="${1#*=}"
            shift
            ;;
        --build-dir)
            (($# >= 2)) || { usage >&2; exit 2; }
            build_dir="$2"
            shift 2
            ;;
        --build-dir=*)
            build_dir="${1#*=}"
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

case "${variant}" in
    p4-v1x|p4-v3x)
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac

if [[ -z "${build_dir}" ]]; then
    build_dir="${FIRMWARE_DIR}/build-${variant}"
elif [[ "${build_dir}" != /* ]]; then
    build_dir="${REPOSITORY_ROOT}/${build_dir}"
fi

readonly SDKCONFIG_PATH="${build_dir}/sdkconfig"
readonly DEFAULTS="${FIRMWARE_DIR}/sdkconfig.defaults;${FIRMWARE_DIR}/sdkconfig.defaults.${variant}"
readonly VARIANT_MARKER="${build_dir}/.p4-production-variant"

source "${SCRIPT_DIR}/activate-idf.sh"
source "${SCRIPT_DIR}/check-firmware-sdkconfig.sh"

[[ -n "${IDF_PATH:-}" ]] || {
    printf 'ERROR: IDF_PATH is not set after ESP-IDF activation\n' >&2
    exit 1
}
idf_version="$(idf.py --version)"
[[ "${idf_version}" == *'v5.5.4'* ]] || {
    printf 'ERROR: ESP-IDF v5.5.4 is required; detected: %s\n' "${idf_version}" >&2
    exit 1
}
printf '%s\n' "${idf_version}"

if [[ -e "${VARIANT_MARKER}" ]]; then
    marker_value="$(<"${VARIANT_MARKER}")"
    [[ "${marker_value}" == "${variant}" ]] || {
        printf 'ERROR: build directory belongs to %s, not %s: %s\n' \
            "${marker_value}" "${variant}" "${build_dir}" >&2
        exit 1
    }
fi

if [[ -f "${SDKCONFIG_PATH}" ]]; then
    check_firmware_sdkconfig "${SDKCONFIG_PATH}" "${variant}"
elif [[ -f "${build_dir}/CMakeCache.txt" ]]; then
    printf 'ERROR: build directory has a CMake cache but no sdkconfig; refusing silent regeneration: %s\n' \
        "${build_dir}" >&2
    exit 1
fi

mkdir -p -- "${build_dir}"

cmake_args=(
    -B "${build_dir}"
    -D "SDKCONFIG=${SDKCONFIG_PATH}"
    -D "SDKCONFIG_DEFAULTS=${DEFAULTS}"
    -D IDF_TARGET=esp32p4
)

cd -- "${FIRMWARE_DIR}"
if [[ ! -f "${SDKCONFIG_PATH}" || ! -f "${build_dir}/CMakeCache.txt" ]]; then
    idf.py "${cmake_args[@]}" set-target esp32p4
fi
check_firmware_sdkconfig "${SDKCONFIG_PATH}" "${variant}"
idf.py "${cmake_args[@]}" reconfigure
check_firmware_sdkconfig "${SDKCONFIG_PATH}" "${variant}"
idf.py "${cmake_args[@]}" build
check_firmware_sdkconfig "${SDKCONFIG_PATH}" "${variant}"
printf '%s\n' "${variant}" > "${VARIANT_MARKER}"

for artifact in \
    "${build_dir}/bootloader/bootloader.bin" \
    "${build_dir}/partition_table/partition-table.bin" \
    "${build_dir}/esp_np2kai.bin" \
    "${build_dir}/esp_np2kai.map"; do
    [[ -f "${artifact}" ]] || {
        printf 'ERROR: expected %s artifact is missing: %s\n' "${variant}" "${artifact}" >&2
        exit 1
    }
done

printf 'PRODUCTION_BUILD variant=%s build_dir=%s sdkconfig=%s\n' \
    "${variant}" "${build_dir}" "${SDKCONFIG_PATH}"
printf 'PRODUCTION_ARTIFACT variant=%s bootloader=%s partition=%s app=%s map=%s\n' \
    "${variant}" "${build_dir}/bootloader/bootloader.bin" \
    "${build_dir}/partition_table/partition-table.bin" \
    "${build_dir}/esp_np2kai.bin" "${build_dir}/esp_np2kai.map"
