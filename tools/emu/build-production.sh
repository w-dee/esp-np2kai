#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly FIRMWARE_DIR="${REPOSITORY_ROOT}/firmware"

usage() {
    printf 'usage: %s --variant p4-v1x|p4-v3x [--board generic|p4-nano] [--build-dir PATH] [--display-foundation | --display-transform-diagnostic --rotation cw|ccw] [--esp-emu-test]\n' \
        "${BASH_SOURCE[0]}"
}

variant=""
board="generic"
build_dir=""
display_foundation=0
display_foundation_variant=""
display_transform_diagnostic=0
display_transform_diagnostic_variant=""
display_transform_diagnostic_rotation=""
esp_emu_test=0
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
        --board)
            (($# >= 2)) || { usage >&2; exit 2; }
            board="$2"
            shift 2
            ;;
        --board=*)
            board="${1#*=}"
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
        --esp-emu-test)
            esp_emu_test=1
            shift
            ;;
        --display-foundation)
            display_foundation=1
            shift
            ;;
        --display-transform-diagnostic)
            display_transform_diagnostic=1
            shift
            ;;
        --rotation)
            (($# >= 2)) || { usage >&2; exit 2; }
            display_transform_diagnostic_rotation="$2"
            shift 2
            ;;
        --rotation=*)
            display_transform_diagnostic_rotation="${1#*=}"
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

if (( display_foundation )) &&
   [[ "${variant}" != "p4-v1x" || "${board}" != "p4-nano" ]]; then
    printf 'ERROR: --display-foundation requires --variant p4-v1x --board p4-nano\n' >&2
    exit 2
fi

if (( display_foundation && display_transform_diagnostic )); then
    printf 'ERROR: --display-foundation and --display-transform-diagnostic are mutually exclusive\n' >&2
    exit 2
fi

if (( display_transform_diagnostic )); then
    if [[ "${variant}" != "p4-v1x" || "${board}" != "p4-nano" ]]; then
        printf 'ERROR: --display-transform-diagnostic requires --variant p4-v1x --board p4-nano\n' >&2
        exit 2
    fi
    case "${display_transform_diagnostic_rotation}" in
        cw|ccw)
            ;;
        *)
            printf 'ERROR: --display-transform-diagnostic requires --rotation cw|ccw\n' >&2
            exit 2
            ;;
    esac
elif [[ -n "${display_transform_diagnostic_rotation}" ]]; then
    printf 'ERROR: --rotation requires --display-transform-diagnostic\n' >&2
    exit 2
fi

case "${board}" in
    generic)
        ;;
    p4-nano)
        [[ "${variant}" == "p4-v1x" ]] || {
            printf 'ERROR: --board p4-nano requires --variant p4-v1x\n' >&2
            exit 2
        }
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac

if (( esp_emu_test )) && [[ "${variant}" != "p4-v3x" || "${board}" != "generic" ]]; then
    printf 'ERROR: --esp-emu-test requires the generic p4-v3x emulator build\n' >&2
    exit 2
fi

if [[ -z "${build_dir}" ]]; then
    if [[ "${board}" == "generic" ]]; then
        build_dir="${FIRMWARE_DIR}/build-${variant}"
    else
        build_dir="${FIRMWARE_DIR}/build-${board}-${variant}"
    fi
elif [[ "${build_dir}" != /* ]]; then
    build_dir="${REPOSITORY_ROOT}/${build_dir}"
fi

readonly SDKCONFIG_PATH="${build_dir}/sdkconfig"
defaults="${FIRMWARE_DIR}/sdkconfig.defaults;${FIRMWARE_DIR}/sdkconfig.defaults.${variant}"
if [[ "${board}" == "p4-nano" ]]; then
    defaults+=";${FIRMWARE_DIR}/sdkconfig.defaults.p4-nano"
fi
readonly DEFAULTS="${defaults}"
readonly VARIANT_MARKER="${build_dir}/.p4-production-variant"
readonly BOARD_MARKER="${build_dir}/.p4-production-board"

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

if [[ -e "${BOARD_MARKER}" ]]; then
    marker_value="$(<"${BOARD_MARKER}")"
    [[ "${marker_value}" == "${board}" ]] || {
        printf 'ERROR: build directory belongs to board %s, not %s: %s\n' \
            "${marker_value}" "${board}" "${build_dir}" >&2
        exit 1
    }
fi

if [[ -f "${SDKCONFIG_PATH}" ]]; then
    check_firmware_sdkconfig "${SDKCONFIG_PATH}" "${variant}" "${board}"
elif [[ -f "${build_dir}/CMakeCache.txt" ]]; then
    printf 'ERROR: build directory has a CMake cache but no sdkconfig; refusing silent regeneration: %s\n' \
        "${build_dir}" >&2
    exit 1
fi

mkdir -p -- "${build_dir}"

if (( display_foundation )); then
    display_foundation_variant="${variant}"
fi
if (( display_transform_diagnostic )); then
    display_transform_diagnostic_variant="${variant}"
fi

cmake_args=(
    -B "${build_dir}"
    -D "SDKCONFIG=${SDKCONFIG_PATH}"
    -D "SDKCONFIG_DEFAULTS=${DEFAULTS}"
    -D IDF_TARGET=esp32p4
    -D "NP2_EMU_TEST=${esp_emu_test}"
    -D "P4_NANO_DISPLAY_FOUNDATION_PROFILE=${display_foundation}"
    -D "P4_NANO_DISPLAY_FOUNDATION_BOARD=${display_foundation}"
    -D "P4_NANO_DISPLAY_FOUNDATION_VARIANT=${display_foundation_variant}"
    -D "P4_NANO_DISPLAY_TRANSFORM_DIAGNOSTIC_PROFILE=${display_transform_diagnostic}"
    -D "P4_NANO_DISPLAY_TRANSFORM_DIAGNOSTIC_BOARD=${display_transform_diagnostic}"
    -D "P4_NANO_DISPLAY_TRANSFORM_DIAGNOSTIC_VARIANT=${display_transform_diagnostic_variant}"
    -D "P4_NANO_DISPLAY_TRANSFORM_DIAGNOSTIC_ROTATION=${display_transform_diagnostic_rotation}"
)
if (( display_foundation )); then
    export P4_NANO_DISPLAY_FOUNDATION_PROFILE=1
    export P4_NANO_DISPLAY_FOUNDATION_BOARD=1
    export P4_NANO_DISPLAY_FOUNDATION_VARIANT="${variant}"
else
    unset P4_NANO_DISPLAY_FOUNDATION_PROFILE
    unset P4_NANO_DISPLAY_FOUNDATION_BOARD
    unset P4_NANO_DISPLAY_FOUNDATION_VARIANT
fi
if (( display_transform_diagnostic )); then
    export P4_NANO_DISPLAY_TRANSFORM_DIAGNOSTIC_PROFILE=1
    export P4_NANO_DISPLAY_TRANSFORM_DIAGNOSTIC_BOARD=1
    export P4_NANO_DISPLAY_TRANSFORM_DIAGNOSTIC_VARIANT="${display_transform_diagnostic_variant}"
    export P4_NANO_DISPLAY_TRANSFORM_DIAGNOSTIC_ROTATION="${display_transform_diagnostic_rotation}"
else
    unset P4_NANO_DISPLAY_TRANSFORM_DIAGNOSTIC_PROFILE
    unset P4_NANO_DISPLAY_TRANSFORM_DIAGNOSTIC_BOARD
    unset P4_NANO_DISPLAY_TRANSFORM_DIAGNOSTIC_VARIANT
    unset P4_NANO_DISPLAY_TRANSFORM_DIAGNOSTIC_ROTATION
fi

cd -- "${FIRMWARE_DIR}"
if [[ ! -f "${SDKCONFIG_PATH}" || ! -f "${build_dir}/CMakeCache.txt" ]]; then
    idf.py "${cmake_args[@]}" set-target esp32p4
fi
check_firmware_sdkconfig "${SDKCONFIG_PATH}" "${variant}" "${board}"
idf.py "${cmake_args[@]}" reconfigure
check_firmware_sdkconfig "${SDKCONFIG_PATH}" "${variant}" "${board}"
idf.py "${cmake_args[@]}" build
check_firmware_sdkconfig "${SDKCONFIG_PATH}" "${variant}" "${board}"
printf '%s\n' "${variant}" > "${VARIANT_MARKER}"
printf '%s\n' "${board}" > "${BOARD_MARKER}"

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

printf 'PRODUCTION_BUILD variant=%s board=%s display_foundation=%s display_transform_diagnostic=%s rotation=%s build_dir=%s sdkconfig=%s\n' \
    "${variant}" "${board}" "${display_foundation}" \
    "${display_transform_diagnostic}" "${display_transform_diagnostic_rotation}" \
    "${build_dir}" "${SDKCONFIG_PATH}"
printf 'PRODUCTION_ARTIFACT variant=%s board=%s bootloader=%s partition=%s app=%s map=%s\n' \
    "${variant}" "${board}" "${build_dir}/bootloader/bootloader.bin" \
    "${build_dir}/partition_table/partition-table.bin" \
    "${build_dir}/esp_np2kai.bin" "${build_dir}/esp_np2kai.map"
