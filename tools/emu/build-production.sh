#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly FIRMWARE_DIR="${REPOSITORY_ROOT}/firmware"

usage() {
    printf 'usage: %s --variant p4-v1x|p4-v3x [--board generic|p4-nano] [--build-dir PATH] [--i286-inline-mem-fastpath 0|1] [--transform-opt debug|o2] [--display-foundation | --display-transform-diagnostic --rotation cw|ccw | --live-display | --live-display-motion-validation | --live-display-benchmark | --live-display-transform-isolated-benchmark | --transform-isolated-compute-control-benchmark | --transform-isolated-psram-read-control-benchmark | --ppa-rotation-benchmark | --ppa-internal-tile-benchmark | --exact2x-scaler-benchmark | --exact2x-internal-source-benchmark | --psram-bandwidth-live --psram-bandwidth-op OP | --psram-bandwidth-isolated --psram-bandwidth-op OP | --real-runtime | --runtime-validation | --runtime-keyboard-validation | --usb-keyboard-validation] [--esp-emu-test]\n' \
        "${BASH_SOURCE[0]}"
}

variant=""
board="generic"
build_dir=""
i286_inline_mem_fastpath=0
transform_opt=""
display_foundation=0
display_foundation_variant=""
display_transform_diagnostic=0
display_transform_diagnostic_variant=""
display_transform_diagnostic_rotation=""
live_display=0
live_display_variant=""
live_display_motion_validation=0
live_display_motion_validation_variant=""
live_display_benchmark=0
live_display_benchmark_variant=""
live_display_transform_isolated_benchmark=0
live_display_transform_isolated_benchmark_variant=""
transform_isolated_compute_control_benchmark=0
transform_isolated_compute_control_benchmark_variant=""
transform_isolated_psram_read_control_benchmark=0
transform_isolated_psram_read_control_benchmark_variant=""
ppa_rotation_benchmark=0
ppa_rotation_benchmark_variant=""
ppa_internal_tile_benchmark=0
ppa_internal_tile_benchmark_variant=""
exact2x_scaler_benchmark=0
exact2x_scaler_benchmark_variant=""
exact2x_internal_source_benchmark=0
exact2x_internal_source_benchmark_variant=""
psram_bandwidth=0
psram_bandwidth_mode=""
psram_bandwidth_operation=""
real_runtime=0
real_runtime_variant=""
runtime_validation=0
runtime_validation_board=""
runtime_validation_variant=""
keyboard_validation=0
keyboard_validation_board=""
keyboard_validation_variant=""
usb_keyboard_validation=0
usb_keyboard_validation_board=""
usb_keyboard_validation_variant=""
runtime_emu_backend=0
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
        --i286-inline-mem-fastpath)
            (($# >= 2)) || { usage >&2; exit 2; }
            i286_inline_mem_fastpath="$2"
            shift 2
            ;;
        --i286-inline-mem-fastpath=*)
            i286_inline_mem_fastpath="${1#*=}"
            shift
            ;;
        --transform-opt)
            (($# >= 2)) || { usage >&2; exit 2; }
            transform_opt="$2"
            shift 2
            ;;
        --transform-opt=*)
            transform_opt="${1#*=}"
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
        --live-display)
            live_display=1
            shift
            ;;
        --live-display-motion-validation)
            live_display_motion_validation=1
            shift
            ;;
        --live-display-benchmark)
            live_display_benchmark=1
            shift
            ;;
        --live-display-transform-isolated-benchmark)
            live_display_transform_isolated_benchmark=1
            shift
            ;;
        --transform-isolated-compute-control-benchmark)
            transform_isolated_compute_control_benchmark=1
            shift
            ;;
        --transform-isolated-psram-read-control-benchmark)
            transform_isolated_psram_read_control_benchmark=1
            shift
            ;;
        --ppa-rotation-benchmark)
            ppa_rotation_benchmark=1
            shift
            ;;
        --ppa-internal-tile-benchmark)
            ppa_internal_tile_benchmark=1
            shift
            ;;
        --exact2x-scaler-benchmark)
            exact2x_scaler_benchmark=1
            shift
            ;;
        --exact2x-internal-source-benchmark)
            exact2x_internal_source_benchmark=1
            shift
            ;;
        --psram-bandwidth-live)
            psram_bandwidth=1
            psram_bandwidth_mode="live"
            shift
            ;;
        --psram-bandwidth-isolated)
            psram_bandwidth=1
            psram_bandwidth_mode="isolated"
            shift
            ;;
        --psram-bandwidth-op)
            (($# >= 2)) || { usage >&2; exit 2; }
            psram_bandwidth_operation="$2"
            shift 2
            ;;
        --psram-bandwidth-op=*)
            psram_bandwidth_operation="${1#*=}"
            shift
            ;;
        --real-runtime)
            real_runtime=1
            shift
            ;;
        --runtime-validation)
            runtime_validation=1
            shift
            ;;
        --runtime-keyboard-validation)
            keyboard_validation=1
            shift
            ;;
        --usb-keyboard-validation)
            usb_keyboard_validation=1
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

if (( psram_bandwidth )); then
    if (( live_display_benchmark || live_display_transform_isolated_benchmark || transform_isolated_compute_control_benchmark || transform_isolated_psram_read_control_benchmark || ppa_rotation_benchmark || ppa_internal_tile_benchmark || exact2x_scaler_benchmark || exact2x_internal_source_benchmark )); then
        printf 'ERROR: PSRAM bandwidth cannot be combined with an explicit display benchmark profile\n' >&2
        exit 2
    fi
    case "${psram_bandwidth_mode}" in
        live)
            live_display_benchmark=1
            ;;
        isolated)
            live_display_transform_isolated_benchmark=1
            ;;
        *)
            printf 'ERROR: PSRAM bandwidth requires --psram-bandwidth-live or --psram-bandwidth-isolated\n' >&2
            exit 2
            ;;
    esac
    case "${psram_bandwidth_operation}" in
        read|write16|write32|memcpy|row-copy|proxy)
            ;;
        *)
            printf 'ERROR: --psram-bandwidth-op requires read, write16, write32, memcpy, row-copy, or proxy\n' >&2
            exit 2
            ;;
    esac
fi

case "${i286_inline_mem_fastpath}" in
    0|1)
        ;;
    *)
        printf 'ERROR: --i286-inline-mem-fastpath requires exactly 0 or 1\n' >&2
        exit 2
        ;;
esac

case "${transform_opt}" in
    ""|debug|o2)
        ;;
    *)
        printf 'ERROR: --transform-opt requires exactly debug or o2\n' >&2
        exit 2
        ;;
esac

if (( live_display_transform_isolated_benchmark || transform_isolated_compute_control_benchmark || transform_isolated_psram_read_control_benchmark || ppa_rotation_benchmark || ppa_internal_tile_benchmark || exact2x_scaler_benchmark || exact2x_internal_source_benchmark )) &&
   [[ "${i286_inline_mem_fastpath}" != "0" ]]; then
    printf 'ERROR: --live-display-transform-isolated-benchmark requires --i286-inline-mem-fastpath 0 (all isolated transform benchmark profiles require --i286-inline-mem-fastpath 0)\n' >&2
    exit 2
fi

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

if (( display_foundation + display_transform_diagnostic + live_display + live_display_motion_validation + live_display_benchmark + live_display_transform_isolated_benchmark + transform_isolated_compute_control_benchmark + transform_isolated_psram_read_control_benchmark + ppa_rotation_benchmark + ppa_internal_tile_benchmark + exact2x_scaler_benchmark + exact2x_internal_source_benchmark + real_runtime + runtime_validation + keyboard_validation + usb_keyboard_validation > 1 )); then
    printf 'ERROR: display, live display, and runtime composition profiles are mutually exclusive\n' >&2
    exit 2
fi

if (( live_display )); then
    if [[ "${variant}" != "p4-v1x" || "${board}" != "p4-nano" ]]; then
        printf 'ERROR: --live-display requires --variant p4-v1x --board p4-nano\n' >&2
        exit 2
    fi
    live_display_variant="${variant}"
fi

if (( live_display_motion_validation )); then
    if [[ "${variant}" != "p4-v1x" || "${board}" != "p4-nano" ]]; then
        printf 'ERROR: --live-display-motion-validation requires --variant p4-v1x --board p4-nano\n' >&2
        exit 2
    fi
    live_display_motion_validation_variant="${variant}"
fi

if (( live_display_benchmark )); then
    if [[ "${variant}" != "p4-v1x" || "${board}" != "p4-nano" ]]; then
        printf 'ERROR: --live-display-benchmark requires --variant p4-v1x --board p4-nano\n' >&2
        exit 2
    fi
    live_display_benchmark_variant="${variant}"
fi

if (( live_display_transform_isolated_benchmark )); then
    if [[ "${variant}" != "p4-v1x" || "${board}" != "p4-nano" ]]; then
        printf 'ERROR: --live-display-transform-isolated-benchmark requires --variant p4-v1x --board p4-nano\n' >&2
        exit 2
    fi
    live_display_transform_isolated_benchmark_variant="${variant}"
fi

if (( transform_isolated_compute_control_benchmark )); then
    if [[ "${variant}" != "p4-v1x" || "${board}" != "p4-nano" ]]; then
        printf 'ERROR: --transform-isolated-compute-control-benchmark requires --variant p4-v1x --board p4-nano\n' >&2
        exit 2
    fi
    transform_isolated_compute_control_benchmark_variant="${variant}"
fi
if (( transform_isolated_psram_read_control_benchmark )); then
    if [[ "${variant}" != "p4-v1x" || "${board}" != "p4-nano" ]]; then
        printf 'ERROR: --transform-isolated-psram-read-control-benchmark requires --variant p4-v1x --board p4-nano\n' >&2
        exit 2
    fi
    transform_isolated_psram_read_control_benchmark_variant="${variant}"
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_PSRAM_READ_CONTROL_BENCHMARK_PROFILE=1
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_PSRAM_READ_CONTROL_BENCHMARK_BOARD=1
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_PSRAM_READ_CONTROL_BENCHMARK_VARIANT="${transform_isolated_psram_read_control_benchmark_variant}"
    # The ESP-IDF early component-requirements pass only preserves the
    # established isolated-profile environment contract.  P8 remains a
    # distinct compile-time selector, but exposes the same lifecycle metadata
    # during that pass so board/variant validation is deterministic.
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE=1
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_BOARD=1
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_VARIANT="${transform_isolated_psram_read_control_benchmark_variant}"
    export NP2VIDEO_BENCHMARK_PROFILE=1
    export NP2VIDEO_GOLDEN_HEADER
else
    unset P4_NANO_LIVE_DISPLAY_TRANSFORM_PSRAM_READ_CONTROL_BENCHMARK_PROFILE
    unset P4_NANO_LIVE_DISPLAY_TRANSFORM_PSRAM_READ_CONTROL_BENCHMARK_BOARD
    unset P4_NANO_LIVE_DISPLAY_TRANSFORM_PSRAM_READ_CONTROL_BENCHMARK_VARIANT
    if (( ! live_display_transform_isolated_benchmark )); then
        unset P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE
        unset P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_BOARD
        unset P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_VARIANT
    fi
fi
if (( ppa_rotation_benchmark )); then
    if [[ "${variant}" != "p4-v1x" || "${board}" != "p4-nano" ]]; then
        printf 'ERROR: --ppa-rotation-benchmark requires --variant p4-v1x --board p4-nano\n' >&2
        exit 2
    fi
    ppa_rotation_benchmark_variant="${variant}"
    export P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE=1
    export P4_NANO_PPA_ROTATION_BENCHMARK_BOARD=1
    export P4_NANO_PPA_ROTATION_BENCHMARK_VARIANT="${ppa_rotation_benchmark_variant}"
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE=1
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_BOARD=1
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_VARIANT="${ppa_rotation_benchmark_variant}"
    export NP2VIDEO_BENCHMARK_PROFILE=1
    export NP2VIDEO_GOLDEN_HEADER
else
    unset P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE
    unset P4_NANO_PPA_ROTATION_BENCHMARK_BOARD
    unset P4_NANO_PPA_ROTATION_BENCHMARK_VARIANT
fi
if (( ppa_internal_tile_benchmark )); then
    if [[ "${variant}" != "p4-v1x" || "${board}" != "p4-nano" ]]; then
        printf 'ERROR: --ppa-internal-tile-benchmark requires --variant p4-v1x --board p4-nano\n' >&2
        exit 2
    fi
    ppa_internal_tile_benchmark_variant="${variant}"
    export P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_PROFILE=1
    export P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_BOARD=1
    export P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_VARIANT="${ppa_internal_tile_benchmark_variant}"
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE=1
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_BOARD=1
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_VARIANT="${ppa_internal_tile_benchmark_variant}"
    export NP2VIDEO_BENCHMARK_PROFILE=1
    export NP2VIDEO_GOLDEN_HEADER
else
    unset P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_PROFILE
    unset P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_BOARD
    unset P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_VARIANT
fi
if (( exact2x_scaler_benchmark )); then
    if [[ "${variant}" != "p4-v1x" || "${board}" != "p4-nano" ]]; then
        printf 'ERROR: --exact2x-scaler-benchmark requires --variant p4-v1x --board p4-nano\n' >&2
        exit 2
    fi
    exact2x_scaler_benchmark_variant="${variant}"
    export P4_NANO_EXACT2X_SCALER_BENCHMARK_PROFILE=1
    export P4_NANO_EXACT2X_SCALER_BENCHMARK_BOARD=1
    export P4_NANO_EXACT2X_SCALER_BENCHMARK_VARIANT="${exact2x_scaler_benchmark_variant}"
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE=1
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_BOARD=1
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_VARIANT="${exact2x_scaler_benchmark_variant}"
    export NP2VIDEO_BENCHMARK_PROFILE=1
    export NP2VIDEO_GOLDEN_HEADER
else
    unset P4_NANO_EXACT2X_SCALER_BENCHMARK_PROFILE
    unset P4_NANO_EXACT2X_SCALER_BENCHMARK_BOARD
    unset P4_NANO_EXACT2X_SCALER_BENCHMARK_VARIANT
fi
if (( exact2x_internal_source_benchmark )); then
    if [[ "${variant}" != "p4-v1x" || "${board}" != "p4-nano" ]]; then
        printf 'ERROR: --exact2x-internal-source-benchmark requires --variant p4-v1x --board p4-nano\n' >&2
        exit 2
    fi
    exact2x_internal_source_benchmark_variant="${variant}"
    export P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_PROFILE=1
    export P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_BOARD=1
    export P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_VARIANT="${exact2x_internal_source_benchmark_variant}"
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE=1
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_BOARD=1
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_VARIANT="${exact2x_internal_source_benchmark_variant}"
    export NP2VIDEO_BENCHMARK_PROFILE=1
    export NP2VIDEO_GOLDEN_HEADER
else
    unset P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_PROFILE
    unset P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_BOARD
    unset P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_VARIANT
fi

if (( real_runtime )); then
    if [[ "${variant}" != "p4-v1x" || "${board}" != "p4-nano" ]]; then
        printf 'ERROR: --real-runtime requires --variant p4-v1x --board p4-nano\n' >&2
        exit 2
    fi
    real_runtime_variant="${variant}"
fi

if (( runtime_validation )); then
    runtime_validation_board=1
    runtime_validation_variant="${variant}"
    if (( esp_emu_test )); then
        if [[ "${variant}" != "p4-v3x" || "${board}" != "generic" ]]; then
            printf 'ERROR: --runtime-validation --esp-emu-test requires generic p4-v3x\n' >&2
            exit 2
        fi
        runtime_emu_backend=1
    elif [[ "${variant}" != "p4-v1x" || "${board}" != "p4-nano" ]]; then
        printf 'ERROR: hardware --runtime-validation requires p4-v1x p4-nano\n' >&2
        exit 2
    fi
fi

if (( keyboard_validation )); then
    keyboard_validation_board=1
    keyboard_validation_variant="${variant}"
    if (( esp_emu_test )); then
        if [[ "${variant}" != "p4-v3x" || "${board}" != "generic" ]]; then
            printf 'ERROR: --runtime-keyboard-validation --esp-emu-test requires generic p4-v3x\n' >&2
            exit 2
        fi
        runtime_emu_backend=1
    elif [[ "${variant}" != "p4-v1x" || "${board}" != "p4-nano" ]]; then
        printf 'ERROR: hardware --runtime-keyboard-validation requires p4-v1x p4-nano\n' >&2
        exit 2
    fi
fi

if (( usb_keyboard_validation )); then
    usb_keyboard_validation_board=1
    usb_keyboard_validation_variant="${variant}"
    if (( esp_emu_test )); then
        printf 'ERROR: --usb-keyboard-validation is hardware-only and cannot use --esp-emu-test\n' >&2
        exit 2
    elif [[ "${variant}" != "p4-v1x" || "${board}" != "p4-nano" ]]; then
        printf 'ERROR: --usb-keyboard-validation requires p4-v1x p4-nano\n' >&2
        exit 2
    fi
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

if (( esp_emu_test && live_display )); then
    printf 'ERROR: --live-display cannot be combined with --esp-emu-test\n' >&2
    exit 2
fi
if (( esp_emu_test && live_display_motion_validation )); then
    printf 'ERROR: --live-display-motion-validation cannot be combined with --esp-emu-test\n' >&2
    exit 2
fi
if (( esp_emu_test && live_display_benchmark )); then
    printf 'ERROR: --live-display-benchmark cannot be combined with --esp-emu-test\n' >&2
    exit 2
fi
if (( esp_emu_test && live_display_transform_isolated_benchmark )); then
    printf 'ERROR: --live-display-transform-isolated-benchmark cannot be combined with --esp-emu-test\n' >&2
    exit 2
fi
if (( esp_emu_test && transform_isolated_compute_control_benchmark )); then
    printf 'ERROR: --transform-isolated-compute-control-benchmark cannot be combined with --esp-emu-test\n' >&2
    exit 2
fi
if (( esp_emu_test && transform_isolated_psram_read_control_benchmark )); then
    printf 'ERROR: --transform-isolated-psram-read-control-benchmark cannot be combined with --esp-emu-test\n' >&2
    exit 2
fi
if (( esp_emu_test && ppa_rotation_benchmark )); then
    printf 'ERROR: --ppa-rotation-benchmark cannot be combined with --esp-emu-test\n' >&2
    exit 2
fi
if (( esp_emu_test && ppa_internal_tile_benchmark )); then
    printf 'ERROR: --ppa-internal-tile-benchmark cannot be combined with --esp-emu-test\n' >&2
    exit 2
fi
if (( esp_emu_test && (exact2x_scaler_benchmark || exact2x_internal_source_benchmark) )); then
    printf 'ERROR: --exact2x-scaler-benchmark cannot be combined with --esp-emu-test\n' >&2
    exit 2
fi

transform_profile=0
if (( display_transform_diagnostic || live_display || live_display_motion_validation || live_display_benchmark ||
      live_display_transform_isolated_benchmark || transform_isolated_compute_control_benchmark ||
      transform_isolated_psram_read_control_benchmark || ppa_rotation_benchmark ||
      ppa_internal_tile_benchmark ||
      exact2x_scaler_benchmark ||
      exact2x_internal_source_benchmark )); then
    transform_profile=1
fi
if [[ -z "${transform_opt}" ]]; then
    if (( transform_profile )); then
        transform_opt=o2
    else
        transform_opt=debug
    fi
fi
if [[ "${transform_opt}" == "o2" ]] && (( ! transform_profile )); then
    printf 'ERROR: --transform-opt o2 requires a P4-NANO transform profile\n' >&2
    exit 2
fi

if [[ -z "${build_dir}" ]]; then
    if (( usb_keyboard_validation )); then
        build_dir="${FIRMWARE_DIR}/build-usb-keyboard-validation-${board}-${variant}"
    elif (( keyboard_validation )); then
        build_dir="${FIRMWARE_DIR}/build-keyboard-validation-${board}-${variant}"
    elif [[ "${board}" == "generic" ]]; then
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

readonly NP2VIDEO_GOLDEN_HEADER="${build_dir}/generated/np2video_golden.h"
np2video_descriptor="${REPOSITORY_ROOT}/tests/guest/np2video/golden.json"
if (( live_display_motion_validation || live_display_benchmark || live_display_transform_isolated_benchmark || transform_isolated_compute_control_benchmark || transform_isolated_psram_read_control_benchmark || ppa_rotation_benchmark || ppa_internal_tile_benchmark || exact2x_scaler_benchmark || exact2x_internal_source_benchmark )); then
    np2video_descriptor="${REPOSITORY_ROOT}/tests/guest/np2video-live/golden.json"
fi

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
    -D "P4_NANO_LIVE_DISPLAY_PROFILE=${live_display}"
    -D "P4_NANO_LIVE_DISPLAY_BOARD=${live_display}"
    -D "P4_NANO_LIVE_DISPLAY_VARIANT=${live_display_variant}"
    -D "P4_NANO_LIVE_DISPLAY_MOTION_VALIDATION_PROFILE=${live_display_motion_validation}"
    -D "P4_NANO_LIVE_DISPLAY_MOTION_VALIDATION_BOARD=${live_display_motion_validation}"
    -D "P4_NANO_LIVE_DISPLAY_MOTION_VALIDATION_VARIANT=${live_display_motion_validation_variant}"
    -D "P4_NANO_LIVE_DISPLAY_BENCHMARK_PROFILE=${live_display_benchmark}"
    -D "P4_NANO_LIVE_DISPLAY_BENCHMARK_BOARD=${live_display_benchmark}"
    -D "P4_NANO_LIVE_DISPLAY_BENCHMARK_VARIANT=${live_display_benchmark_variant}"
    -D "P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE=$((live_display_transform_isolated_benchmark || ppa_internal_tile_benchmark || exact2x_scaler_benchmark || exact2x_internal_source_benchmark))"
    -D "P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_BOARD=$((live_display_transform_isolated_benchmark || ppa_internal_tile_benchmark || exact2x_scaler_benchmark || exact2x_internal_source_benchmark))"
    -D "P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_VARIANT=${live_display_transform_isolated_benchmark_variant:-${ppa_internal_tile_benchmark_variant:-${exact2x_scaler_benchmark_variant:-${exact2x_internal_source_benchmark_variant}}}}"
    -D "P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE=${ppa_rotation_benchmark}"
    -D "P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_PROFILE=${ppa_internal_tile_benchmark}"
    -D "P4_NANO_EXACT2X_SCALER_BENCHMARK_PROFILE=${exact2x_scaler_benchmark}"
    -D "P4_NANO_EXACT2X_SCALER_BENCHMARK_BOARD=${exact2x_scaler_benchmark}"
    -D "P4_NANO_EXACT2X_SCALER_BENCHMARK_VARIANT=${exact2x_scaler_benchmark_variant}"
    -D "P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_PROFILE=${exact2x_internal_source_benchmark}"
    -D "P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_BOARD=${exact2x_internal_source_benchmark}"
    -D "P4_NANO_EXACT2X_INTERNAL_SOURCE_BENCHMARK_VARIANT=${exact2x_internal_source_benchmark_variant}"
    -D "P4_NANO_LIVE_DISPLAY_TRANSFORM_COMPUTE_CONTROL_BENCHMARK_PROFILE=${transform_isolated_compute_control_benchmark}"
    -D "P4_NANO_LIVE_DISPLAY_TRANSFORM_COMPUTE_CONTROL_BENCHMARK_BOARD=${transform_isolated_compute_control_benchmark}"
    -D "P4_NANO_LIVE_DISPLAY_TRANSFORM_COMPUTE_CONTROL_BENCHMARK_VARIANT=${transform_isolated_compute_control_benchmark_variant}"
    -D "P4_NANO_LIVE_DISPLAY_TRANSFORM_PSRAM_READ_CONTROL_BENCHMARK_PROFILE=${transform_isolated_psram_read_control_benchmark}"
    -D "P4_NANO_LIVE_DISPLAY_TRANSFORM_PSRAM_READ_CONTROL_BENCHMARK_BOARD=${transform_isolated_psram_read_control_benchmark}"
    -D "P4_NANO_LIVE_DISPLAY_TRANSFORM_PSRAM_READ_CONTROL_BENCHMARK_VARIANT=${transform_isolated_psram_read_control_benchmark_variant}"
    -D "P4_NANO_PSRAM_BANDWIDTH_BENCHMARK_PROFILE=${psram_bandwidth}"
    -D "P4_NANO_PSRAM_BANDWIDTH_OPERATION=${psram_bandwidth_operation}"
    -D "P4_NANO_REAL_RUNTIME_PROFILE=${real_runtime}"
    -D "P4_NANO_REAL_RUNTIME_BOARD=${real_runtime}"
    -D "P4_NANO_REAL_RUNTIME_VARIANT=${real_runtime_variant}"
    -D "P4_NANO_RUNTIME_VALIDATION_PROFILE=${runtime_validation}"
    -D "P4_NANO_RUNTIME_VALIDATION_BOARD=${runtime_validation_board}"
    -D "P4_NANO_RUNTIME_VALIDATION_VARIANT=${runtime_validation_variant}"
    -D "P4_NANO_KEYBOARD_VALIDATION_PROFILE=${keyboard_validation}"
    -D "P4_NANO_KEYBOARD_VALIDATION_BOARD=${keyboard_validation_board}"
    -D "P4_NANO_KEYBOARD_VALIDATION_VARIANT=${keyboard_validation_variant}"
    -D "P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE=${usb_keyboard_validation}"
    -D "P4_NANO_USB_KEYBOARD_VALIDATION_BOARD=${usb_keyboard_validation_board}"
    -D "P4_NANO_USB_KEYBOARD_VALIDATION_VARIANT=${usb_keyboard_validation_variant}"
    -D "P4_NANO_RUNTIME_EMU_BACKEND=${runtime_emu_backend}"
    -D "NP2VIDEO_CONTINUOUS_PROFILE=$((live_display_motion_validation || live_display_benchmark || live_display_transform_isolated_benchmark || transform_isolated_compute_control_benchmark || transform_isolated_psram_read_control_benchmark || ppa_rotation_benchmark || ppa_internal_tile_benchmark || exact2x_scaler_benchmark || exact2x_internal_source_benchmark))"
    -D "NP2VIDEO_BENCHMARK_PROFILE=$((live_display_benchmark || live_display_transform_isolated_benchmark || transform_isolated_compute_control_benchmark || transform_isolated_psram_read_control_benchmark || ppa_rotation_benchmark || ppa_internal_tile_benchmark || exact2x_scaler_benchmark || exact2x_internal_source_benchmark))"
    -D "NP2_I286C_INLINE_MEM_FASTPATH=${i286_inline_mem_fastpath}"
    -D "P4_NANO_DISPLAY_TRANSFORM_OPT=${transform_opt}"
    -D "NP2VIDEO_GOLDEN_HEADER=${NP2VIDEO_GOLDEN_HEADER}"
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
if (( real_runtime )); then
    export P4_NANO_REAL_RUNTIME_PROFILE=1
    export P4_NANO_REAL_RUNTIME_BOARD=1
    export P4_NANO_REAL_RUNTIME_VARIANT="${real_runtime_variant}"
else
    unset P4_NANO_REAL_RUNTIME_PROFILE
    unset P4_NANO_REAL_RUNTIME_BOARD
    unset P4_NANO_REAL_RUNTIME_VARIANT
fi
if (( runtime_validation )); then
    export P4_NANO_RUNTIME_VALIDATION_PROFILE=1
    export P4_NANO_RUNTIME_VALIDATION_BOARD=1
    export P4_NANO_RUNTIME_VALIDATION_VARIANT="${runtime_validation_variant}"
else
    unset P4_NANO_RUNTIME_VALIDATION_PROFILE
    unset P4_NANO_RUNTIME_VALIDATION_BOARD
    unset P4_NANO_RUNTIME_VALIDATION_VARIANT
fi
if (( keyboard_validation )); then
    export P4_NANO_KEYBOARD_VALIDATION_PROFILE=1
    export P4_NANO_KEYBOARD_VALIDATION_BOARD=1
    export P4_NANO_KEYBOARD_VALIDATION_VARIANT="${keyboard_validation_variant}"
else
    unset P4_NANO_KEYBOARD_VALIDATION_PROFILE
    unset P4_NANO_KEYBOARD_VALIDATION_BOARD
    unset P4_NANO_KEYBOARD_VALIDATION_VARIANT
fi
if (( usb_keyboard_validation )); then
    export P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE=1
    export P4_NANO_USB_KEYBOARD_VALIDATION_BOARD=1
    export P4_NANO_USB_KEYBOARD_VALIDATION_VARIANT="${usb_keyboard_validation_variant}"
else
    unset P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE
    unset P4_NANO_USB_KEYBOARD_VALIDATION_BOARD
    unset P4_NANO_USB_KEYBOARD_VALIDATION_VARIANT
fi
if (( runtime_emu_backend )); then
    export P4_NANO_RUNTIME_EMU_BACKEND=1
else
    unset P4_NANO_RUNTIME_EMU_BACKEND
fi
if (( live_display )); then
    export P4_NANO_LIVE_DISPLAY_PROFILE=1
    export P4_NANO_LIVE_DISPLAY_BOARD=1
    export P4_NANO_LIVE_DISPLAY_VARIANT="${live_display_variant}"
    export NP2VIDEO_GOLDEN_HEADER
else
    unset P4_NANO_LIVE_DISPLAY_PROFILE
    unset P4_NANO_LIVE_DISPLAY_BOARD
    unset P4_NANO_LIVE_DISPLAY_VARIANT
fi
if (( live_display_motion_validation )); then
    export P4_NANO_LIVE_DISPLAY_MOTION_VALIDATION_PROFILE=1
    export P4_NANO_LIVE_DISPLAY_MOTION_VALIDATION_BOARD=1
    export P4_NANO_LIVE_DISPLAY_MOTION_VALIDATION_VARIANT="${live_display_motion_validation_variant}"
    export NP2VIDEO_CONTINUOUS_PROFILE=1
    export NP2VIDEO_GOLDEN_HEADER
else
    unset P4_NANO_LIVE_DISPLAY_MOTION_VALIDATION_PROFILE
    unset P4_NANO_LIVE_DISPLAY_MOTION_VALIDATION_BOARD
    unset P4_NANO_LIVE_DISPLAY_MOTION_VALIDATION_VARIANT
fi
if (( live_display_benchmark )); then
    export P4_NANO_LIVE_DISPLAY_BENCHMARK_PROFILE=1
    export P4_NANO_LIVE_DISPLAY_BENCHMARK_BOARD=1
    export P4_NANO_LIVE_DISPLAY_BENCHMARK_VARIANT="${live_display_benchmark_variant}"
    export NP2VIDEO_BENCHMARK_PROFILE=1
    export NP2VIDEO_GOLDEN_HEADER
else
    unset P4_NANO_LIVE_DISPLAY_BENCHMARK_PROFILE
    unset P4_NANO_LIVE_DISPLAY_BENCHMARK_BOARD
    unset P4_NANO_LIVE_DISPLAY_BENCHMARK_VARIANT
    unset NP2VIDEO_BENCHMARK_PROFILE
fi
if (( live_display_transform_isolated_benchmark || ppa_rotation_benchmark || ppa_internal_tile_benchmark || exact2x_scaler_benchmark || exact2x_internal_source_benchmark )); then
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE=1
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_BOARD=1
    if (( ppa_rotation_benchmark )); then
        export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_VARIANT="${ppa_rotation_benchmark_variant}"
    elif (( ppa_internal_tile_benchmark )); then
        export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_VARIANT="${ppa_internal_tile_benchmark_variant}"
    elif (( exact2x_scaler_benchmark )); then
        export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_VARIANT="${exact2x_scaler_benchmark_variant}"
    elif (( exact2x_internal_source_benchmark )); then
        export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_VARIANT="${exact2x_internal_source_benchmark_variant}"
    else
        export P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_VARIANT="${live_display_transform_isolated_benchmark_variant}"
    fi
    export NP2VIDEO_BENCHMARK_PROFILE=1
    export NP2VIDEO_GOLDEN_HEADER
else
    unset P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_PROFILE
    unset P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_BOARD
    unset P4_NANO_LIVE_DISPLAY_TRANSFORM_ISOLATED_BENCHMARK_VARIANT
fi
if (( ppa_rotation_benchmark )); then
    export P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE=1
    export P4_NANO_PPA_ROTATION_BENCHMARK_BOARD=1
    export P4_NANO_PPA_ROTATION_BENCHMARK_VARIANT="${ppa_rotation_benchmark_variant}"
else
    unset P4_NANO_PPA_ROTATION_BENCHMARK_PROFILE
    unset P4_NANO_PPA_ROTATION_BENCHMARK_BOARD
    unset P4_NANO_PPA_ROTATION_BENCHMARK_VARIANT
fi
if (( ppa_internal_tile_benchmark )); then
    export P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_PROFILE=1
    export P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_BOARD=1
    export P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_VARIANT="${ppa_internal_tile_benchmark_variant}"
else
    unset P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_PROFILE
    unset P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_BOARD
    unset P4_NANO_PPA_INTERNAL_TILE_BENCHMARK_VARIANT
fi
if (( transform_isolated_compute_control_benchmark )); then
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_COMPUTE_CONTROL_BENCHMARK_PROFILE=1
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_COMPUTE_CONTROL_BENCHMARK_BOARD=1
    export P4_NANO_LIVE_DISPLAY_TRANSFORM_COMPUTE_CONTROL_BENCHMARK_VARIANT="${transform_isolated_compute_control_benchmark_variant}"
    export NP2VIDEO_BENCHMARK_PROFILE=1
    export NP2VIDEO_GOLDEN_HEADER
else
    unset P4_NANO_LIVE_DISPLAY_TRANSFORM_COMPUTE_CONTROL_BENCHMARK_PROFILE
    unset P4_NANO_LIVE_DISPLAY_TRANSFORM_COMPUTE_CONTROL_BENCHMARK_BOARD
    unset P4_NANO_LIVE_DISPLAY_TRANSFORM_COMPUTE_CONTROL_BENCHMARK_VARIANT
fi
if (( psram_bandwidth )); then
    export P4_NANO_PSRAM_BANDWIDTH_BENCHMARK_PROFILE=1
    export P4_NANO_PSRAM_BANDWIDTH_OPERATION="${psram_bandwidth_operation}"
else
    unset P4_NANO_PSRAM_BANDWIDTH_BENCHMARK_PROFILE
    unset P4_NANO_PSRAM_BANDWIDTH_OPERATION
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
needs_initial_config=0
if [[ ! -f "${SDKCONFIG_PATH}" || ! -f "${build_dir}/CMakeCache.txt" ]]; then
    needs_initial_config=1
fi
if (( needs_initial_config )); then
    initial_cmake_args=("${cmake_args[@]}")
    initial_golden_header=""
    if (( live_display || live_display_motion_validation || live_display_benchmark || live_display_transform_isolated_benchmark || transform_isolated_compute_control_benchmark || transform_isolated_psram_read_control_benchmark || ppa_rotation_benchmark || ppa_internal_tile_benchmark || exact2x_scaler_benchmark || exact2x_internal_source_benchmark )); then
        initial_golden_header="$(mktemp "${TMPDIR:-/tmp}/np2video-golden-header.XXXXXX")"
        python3 "${REPOSITORY_ROOT}/tools/guest/generate_np2video_golden_header.py" \
            --descriptor "${np2video_descriptor}" \
            --output "${initial_golden_header}"
        initial_cmake_args+=(
            -D "NP2VIDEO_GOLDEN_HEADER=${initial_golden_header}"
        )
    fi
    idf.py "${initial_cmake_args[@]}" set-target esp32p4
    if [[ -n "${initial_golden_header}" ]]; then
        rm -f -- "${initial_golden_header}"
        mkdir -p -- "$(dirname -- "${NP2VIDEO_GOLDEN_HEADER}")"
        python3 "${REPOSITORY_ROOT}/tools/guest/generate_np2video_golden_header.py" \
            --descriptor "${np2video_descriptor}" \
            --output "${NP2VIDEO_GOLDEN_HEADER}"
    fi
fi
if (( live_display || live_display_motion_validation || live_display_benchmark || live_display_transform_isolated_benchmark || transform_isolated_compute_control_benchmark || transform_isolated_psram_read_control_benchmark || ppa_rotation_benchmark || ppa_internal_tile_benchmark || exact2x_scaler_benchmark || exact2x_internal_source_benchmark )); then
    mkdir -p -- "$(dirname -- "${NP2VIDEO_GOLDEN_HEADER}")"
    python3 "${REPOSITORY_ROOT}/tools/guest/generate_np2video_golden_header.py" \
        --descriptor "${np2video_descriptor}" \
        --output "${NP2VIDEO_GOLDEN_HEADER}"
    [[ -f "${NP2VIDEO_GOLDEN_HEADER}" ]] || {
        printf 'ERROR: generated NP2 video golden header is missing: %s\n' \
            "${NP2VIDEO_GOLDEN_HEADER}" >&2
        exit 1
    }
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

printf 'PRODUCTION_BUILD variant=%s board=%s i286_inline_mem_fastpath=%s transform_opt=%s display_foundation=%s display_transform_diagnostic=%s live_display=%s live_display_motion_validation=%s live_display_benchmark=%s live_display_transform_isolated_benchmark=%s transform_isolated_compute_control_benchmark=%s transform_isolated_psram_read_control_benchmark=%s ppa_rotation_benchmark=%s ppa_internal_tile_benchmark=%s exact2x_internal_source_benchmark=%s real_runtime=%s runtime_validation=%s keyboard_validation=%s rotation=%s build_dir=%s sdkconfig=%s\n' \
    "${variant}" "${board}" "${i286_inline_mem_fastpath}" "${transform_opt}" "${display_foundation}" \
    "${display_transform_diagnostic}" "${live_display}" "${live_display_motion_validation}" "${live_display_benchmark}" \
    "${live_display_transform_isolated_benchmark}" "${transform_isolated_compute_control_benchmark}" "${transform_isolated_psram_read_control_benchmark}" "${ppa_rotation_benchmark}" "${ppa_internal_tile_benchmark}" "${exact2x_internal_source_benchmark}" "${real_runtime}" "${runtime_validation}" "${keyboard_validation}" "${display_transform_diagnostic_rotation}" \
    "${build_dir}" "${SDKCONFIG_PATH}"
printf 'PRODUCTION_ARTIFACT variant=%s board=%s bootloader=%s partition=%s app=%s map=%s\n' \
    "${variant}" "${board}" "${build_dir}/bootloader/bootloader.bin" \
    "${build_dir}/partition_table/partition-table.bin" \
    "${build_dir}/esp_np2kai.bin" "${build_dir}/esp_np2kai.map"
