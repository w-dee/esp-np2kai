#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly FIRMWARE_DIR="${REPOSITORY_ROOT}/firmware"
readonly PRODUCTION_DEFAULTS="${FIRMWARE_DIR}/sdkconfig.defaults;${FIRMWARE_DIR}/sdkconfig.defaults.p4-v3x"
readonly EXPECTED_IDF_VERSION="ESP-IDF v5.5.4"
readonly EXPECTED_EMU_VERSION="esp-emu 0.39.0"
readonly EXPECTED_FIXTURE_SHA256="3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3"
readonly ESP_EMU="${ESP_EMU:-${HOME}/.local/bin/esp-emu}"
readonly RUN_ROOT="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/esp-np2kai-step6a.XXXXXX")"
readonly STEP6A_PROFILE_BUILD_MODE="${STEP6A_PROFILE_BUILD_MODE:-shared}"
readonly SHARED_PROFILE_BUILD_DIR="${RUN_ROOT}/shared-profile-build"
readonly PROFILE_SELECTOR_ENV_VARS="STORAGE_FATFS_PROBE STORAGE_FATFS_CI_BOUNDED UART_FATFS_PROFILE NP2_DOSIO_PROBE NP2_VFS_FIXTURE_PROFILE NP2_REDUCED_EXTMEM8 NP2_VIDEO_PROFILE NP2_PRESENTATION_PROFILE"

declare -a APP_SIZE_SUMMARY=()
CURRENT_PHASE="preflight"
CURRENT_LOG="${RUN_ROOT}"
TOTAL_START="$(date +%s)"
STORAGE_PROFILE_STATE_CAPTURED=0
STORAGE_PROFILE_SDKCONFIG_SHA256=""
STORAGE_PROFILE_CMAKE_CACHE_SHA256=""
STORAGE_PROFILE_COMPILE_COMMANDS_SHA256=""
STORAGE_PROFILE_APP_SIZE=""
STORAGE_PROFILE_APP_SHA256=""
FACTORY_PARTITION_OFFSET=""
FACTORY_PARTITION_SIZE=""
RAW_FIXTURE_OFFSET=""
RAW_FIXTURE_SIZE=""
RAW_FIXTURE_END=""
STORAGE_PARTITION_OFFSET=""
STORAGE_PARTITION_SIZE=""
STORAGE_PARTITION_END=""
FLASH_SIZE=""

cleanup() {
    local status=$?
    if (( status == 0 )); then
        rm -rf -- "${RUN_ROOT}"
    else
        printf 'STEP6A_TEMP_ROOT_PRESERVED=%s\n' "${RUN_ROOT}" >&2
        printf 'STEP6A_FAILURE_PHASE=%s\n' "${CURRENT_PHASE}" >&2
        printf 'STEP6A_FAILURE_PATH=%s\n' "${CURRENT_LOG}" >&2
    fi
    exit "${status}"
}
trap cleanup EXIT

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    return 1
}

validate_profile_build_mode() {
    case "${STEP6A_PROFILE_BUILD_MODE}" in
        shared|isolated)
            ;;
        *)
            fail "invalid STEP6A_PROFILE_BUILD_MODE=${STEP6A_PROFILE_BUILD_MODE}; expected shared or isolated"
            return 1
            ;;
    esac
}

selected_profile_build_dir() {
    local isolated_build_dir="$1"
    case "${STEP6A_PROFILE_BUILD_MODE}" in
        shared)
            printf '%s\n' "${SHARED_PROFILE_BUILD_DIR}"
            ;;
        isolated)
            printf '%s\n' "${isolated_build_dir}"
            ;;
    esac
}

phase_start() {
    printf '\n===== STEP6A_PHASE_START %s =====\n' "${CURRENT_PHASE}"
}

run_phase() {
    local name="$1"
    shift
    CURRENT_PHASE="${name}"
    phase_start
    local started="$(date +%s)"
    "$@"
    printf 'STEP6A_PHASE_PASS name=%s elapsed_seconds=%s\n' \
        "${name}" "$(( $(date +%s) - started ))"
}

require_file() {
    local path="$1"
    [[ -f "${path}" ]] || {
        fail "required file is missing: ${path}"
        return 1
    }
}

require_log() {
    local log_path="$1"
    local expected="$2"
    grep -Fq -- "${expected}" "${log_path}" || {
        fail "required log marker is missing: ${expected}; log=${log_path}"
        return 1
    }
}

check_app_size() {
    local profile="$1"
    local build_dir="$2"
    local app_binary="${build_dir}/esp_np2kai.bin"
    require_file "${app_binary}"
    if [[ -z "${FACTORY_PARTITION_SIZE}" ]]; then
        load_partition_geometry "${build_dir}"
    fi
    local app_size
    app_size="$(stat -c '%s' "${app_binary}")"
    if (( app_size >= FACTORY_PARTITION_SIZE )); then
        fail "${profile} app does not fit strictly below factory partition: " \
            "${app_size} >= ${FACTORY_PARTITION_SIZE}"
        return 1
    fi
    local remaining=$((FACTORY_PARTITION_SIZE - app_size))
    local summary
    summary="profile=${profile} "
    summary+="app_size_hex=$(printf '0x%x' "${app_size}") "
    summary+="partition_size=$(printf '0x%x' "${FACTORY_PARTITION_SIZE}") "
    summary+="remaining_hex=$(printf '0x%x' "${remaining}")"
    APP_SIZE_SUMMARY+=("${summary}")
    printf 'STEP6A_APP_SIZE %s\n' "${summary}"
}

build_profile() {
    local profile="$1"
    local build_dir="$2"
    shift 2
    # Shared mode reuses only the incremental build tree; binaries and
    # emulator/test lifecycles remain profile-specific. The selector values
    # are reset on every call, environment variables are cleared before
    # CMake, and the round-trip phase guards against stale profile state.
    # Isolated mode remains the fallback for diagnosis.
    if [[ "${STEP6A_PROFILE_BUILD_MODE}" == shared ]]; then
        build_dir="${SHARED_PROFILE_BUILD_DIR}"
    fi
    local sdkconfig_path="${build_dir}/sdkconfig"
    local storage_fatfs_probe=0
    local storage_fatfs_ci_bounded=0
    local uart_fatfs_profile=0
    local np2_dosio_probe=0
    local np2_vfs_fixture_profile=0
    local np2_reduced_extmem8=0
    local np2_video_profile=0
    local np2_presentation_profile=0
    local flag
    for flag in "$@"; do
        case "${flag}" in
            STORAGE_FATFS_PROBE) storage_fatfs_probe=1 ;;
            STORAGE_FATFS_CI_BOUNDED) storage_fatfs_ci_bounded=1 ;;
            UART_FATFS_PROFILE) uart_fatfs_profile=1 ;;
            NP2_DOSIO_PROBE) np2_dosio_probe=1 ;;
            NP2_VFS_FIXTURE_PROFILE) np2_vfs_fixture_profile=1 ;;
            NP2_REDUCED_EXTMEM8) np2_reduced_extmem8=1 ;;
            NP2_VIDEO_PROFILE) np2_video_profile=1 ;;
            NP2_PRESENTATION_PROFILE) np2_presentation_profile=1 ;;
            *)
                fail "unknown Step 6A profile selector: ${flag}"
                return 1
                ;;
        esac
    done

    local -a cmake_args=(
        -B "${build_dir}"
        -D "SDKCONFIG=${sdkconfig_path}"
        -D "SDKCONFIG_DEFAULTS=${PRODUCTION_DEFAULTS}"
        -D CCache_ENABLE=0
        -D IDF_TARGET=esp32p4
        -D "STORAGE_FATFS_PROBE=${storage_fatfs_probe}"
        -D "STORAGE_FATFS_CI_BOUNDED=${storage_fatfs_ci_bounded}"
        -D "UART_FATFS_PROFILE=${uart_fatfs_profile}"
        -D "NP2_DOSIO_PROBE=${np2_dosio_probe}"
        -D "NP2_VFS_FIXTURE_PROFILE=${np2_vfs_fixture_profile}"
        -D "NP2_REDUCED_EXTMEM8=${np2_reduced_extmem8}"
        -D "NP2_VIDEO_PROFILE=${np2_video_profile}"
        -D "NP2_PRESENTATION_PROFILE=${np2_presentation_profile}"
    )
    mkdir -p -- "${build_dir}"
    printf 'STEP6A_PROFILE_BUILD mode=%s profile=%s build_dir=%s\n' \
        "${STEP6A_PROFILE_BUILD_MODE}" "${profile}" "${build_dir}"
    printf 'STEP6A_PROFILE_STATE mode=%s profile=%s STORAGE_FATFS_PROBE=%s STORAGE_FATFS_CI_BOUNDED=%s UART_FATFS_PROFILE=%s NP2_DOSIO_PROBE=%s NP2_VFS_FIXTURE_PROFILE=%s NP2_REDUCED_EXTMEM8=%s NP2_VIDEO_PROFILE=%s NP2_PRESENTATION_PROFILE=%s\n' \
        "${STEP6A_PROFILE_BUILD_MODE}" "${profile}" \
        "${storage_fatfs_probe}" "${storage_fatfs_ci_bounded}" \
        "${uart_fatfs_profile}" "${np2_dosio_probe}" \
        "${np2_vfs_fixture_profile}" "${np2_reduced_extmem8}" \
        "${np2_video_profile}" "${np2_presentation_profile}"
    (
        cd -- "${FIRMWARE_DIR}"
        unset STORAGE_FATFS_PROBE STORAGE_FATFS_CI_BOUNDED UART_FATFS_PROFILE \
            NP2_DOSIO_PROBE NP2_VFS_FIXTURE_PROFILE NP2_REDUCED_EXTMEM8 \
            NP2_VIDEO_PROFILE NP2_PRESENTATION_PROFILE
        local set_target_started=0
        local set_target_finished=0
        local reconfigure_started
        local reconfigure_finished
        local build_started
        local build_finished
        if [[ ! -f "${sdkconfig_path}" || ! -f "${build_dir}/CMakeCache.txt" ]]; then
            set_target_started="$(date +%s%N)"
            idf.py -B "${build_dir}" \
                -D "SDKCONFIG=${sdkconfig_path}" \
                -D "SDKCONFIG_DEFAULTS=${PRODUCTION_DEFAULTS}" \
                -D CCache_ENABLE=0 \
                -D IDF_TARGET=esp32p4 set-target esp32p4
            set_target_finished="$(date +%s%N)"
        fi
        check_firmware_sdkconfig "${sdkconfig_path}" p4-v3x
        reconfigure_started="$(date +%s%N)"
        idf.py "${cmake_args[@]}" reconfigure
        reconfigure_finished="$(date +%s%N)"
        check_firmware_sdkconfig "${sdkconfig_path}" p4-v3x
        build_started="$(date +%s%N)"
        idf.py "${cmake_args[@]}" build
        build_finished="$(date +%s%N)"
        local set_target_seconds=0
        if (( set_target_started != 0 )); then
            set_target_seconds="$(awk -v start="${set_target_started}" -v end="${set_target_finished}" 'BEGIN { printf "%.3f", (end - start) / 1000000000 }')"
        fi
        local reconfigure_seconds
        local build_seconds
        local total_seconds
        reconfigure_seconds="$(awk -v start="${reconfigure_started}" -v end="${reconfigure_finished}" 'BEGIN { printf "%.3f", (end - start) / 1000000000 }')"
        build_seconds="$(awk -v start="${build_started}" -v end="${build_finished}" 'BEGIN { printf "%.3f", (end - start) / 1000000000 }')"
        local total_started="${set_target_started}"
        if (( total_started == 0 )); then
            total_started="${reconfigure_started}"
        fi
        total_seconds="$(awk -v start="${total_started}" -v end="${build_finished}" 'BEGIN { printf "%.3f", (end - start) / 1000000000 }')"
        printf 'STEP6A_PROFILE_BUILD_TIMING mode=%s profile=%s set_target_seconds=%s reconfigure_seconds=%s build_seconds=%s total_seconds=%s\n' \
            "${STEP6A_PROFILE_BUILD_MODE}" "${profile}" \
            "${set_target_seconds}" "${reconfigure_seconds}" \
            "${build_seconds}" "${total_seconds}"
    )
    check_app_size "${profile}" "${build_dir}"
    emit_profile_artifact_diagnostics "${profile}" "${build_dir}"
}

emit_profile_artifact_diagnostics() {
    local profile="$1"
    local build_dir="$2"
    local app_binary="${build_dir}/esp_np2kai.bin"
    local app_size
    local app_sha256
    local sdkconfig_sha256
    local cmake_cache_sha256
    local compile_commands_sha256
    local partition_sha256
    local bootloader_size
    local profile_definitions
    app_size="$(stat -c '%s' "${app_binary}")"
    app_sha256="$(sha256sum "${app_binary}" | awk '{print $1}')"
    sdkconfig_sha256="$(sha256sum "${build_dir}/sdkconfig" | awk '{print $1}')"
    cmake_cache_sha256="$(sha256sum "${build_dir}/CMakeCache.txt" | awk '{print $1}')"
    compile_commands_sha256="$(sha256sum "${build_dir}/compile_commands.json" | awk '{print $1}')"
    partition_sha256="$(sha256sum "${build_dir}/partition_table/partition-table.bin" | awk '{print $1}')"
    bootloader_size="$(stat -c '%s' "${build_dir}/bootloader/bootloader.bin")"
    profile_definitions="$(rg -o -- '-D(STORAGE_FATFS_PROBE|STORAGE_FATFS_CI_BOUNDED|UART_FATFS_PROFILE|NP2_DOSIO_PROBE|NP2_VFS_FIXTURE_PROFILE|NP2_REDUCED_EXTMEM8|NP2_VIDEO_PROFILE|NP2_PRESENTATION_PROFILE|STORAGE_FATFS_TEST_HOOKS)(=[^ ]+)?' "${build_dir}/compile_commands.json" | sort -u | paste -sd, - || true)"
    printf 'STEP6A_PROFILE_ARTIFACT mode=%s profile=%s app_size=%s app_sha256=%s sdkconfig_sha256=%s cmake_cache_sha256=%s compile_commands_sha256=%s partition_sha256=%s bootloader_size=%s profile_definitions=%s\n' \
        "${STEP6A_PROFILE_BUILD_MODE}" "${profile}" "${app_size}" \
        "${app_sha256}" "${sdkconfig_sha256}" "${cmake_cache_sha256}" \
        "${compile_commands_sha256}" "${partition_sha256}" \
        "${bootloader_size}" "${profile_definitions:--}"
}

capture_storage_profile_state() {
    local build_dir="$1"
    STORAGE_PROFILE_STATE_CAPTURED=1
    STORAGE_PROFILE_SDKCONFIG_SHA256="$(sha256sum "${build_dir}/sdkconfig" | awk '{print $1}')"
    STORAGE_PROFILE_CMAKE_CACHE_SHA256="$(sha256sum "${build_dir}/CMakeCache.txt" | awk '{print $1}')"
    STORAGE_PROFILE_COMPILE_COMMANDS_SHA256="$(sha256sum "${build_dir}/compile_commands.json" | awk '{print $1}')"
    STORAGE_PROFILE_APP_SIZE="$(stat -c '%s' "${build_dir}/esp_np2kai.bin")"
    STORAGE_PROFILE_APP_SHA256="$(sha256sum "${build_dir}/esp_np2kai.bin" | awk '{print $1}')"
}

phase_profile_roundtrip() {
    if [[ "${STEP6A_PROFILE_BUILD_MODE}" != shared ]]; then
        printf 'STEP6A_PROFILE_ROUNDTRIP mode=%s result=SKIP\n' "${STEP6A_PROFILE_BUILD_MODE}"
        return 0
    fi
    if (( STORAGE_PROFILE_STATE_CAPTURED != 1 )); then
        fail 'shared profile round-trip has no captured initial storage state'
        return 1
    fi
    local build_dir="${SHARED_PROFILE_BUILD_DIR}"
    printf 'STEP6A_PROFILE_ROUNDTRIP start=storage-provider end=storage-provider\n'
    build_profile storage-provider-roundtrip "${build_dir}" \
        STORAGE_FATFS_PROBE STORAGE_FATFS_CI_BOUNDED
    local sdkconfig_sha256
    local cmake_cache_sha256
    local compile_commands_sha256
    local app_size
    local app_sha256
    sdkconfig_sha256="$(sha256sum "${build_dir}/sdkconfig" | awk '{print $1}')"
    cmake_cache_sha256="$(sha256sum "${build_dir}/CMakeCache.txt" | awk '{print $1}')"
    compile_commands_sha256="$(sha256sum "${build_dir}/compile_commands.json" | awk '{print $1}')"
    app_size="$(stat -c '%s' "${build_dir}/esp_np2kai.bin")"
    app_sha256="$(sha256sum "${build_dir}/esp_np2kai.bin" | awk '{print $1}')"
    [[ "${sdkconfig_sha256}" == "${STORAGE_PROFILE_SDKCONFIG_SHA256}" ]] || {
        fail 'shared profile round-trip changed sdkconfig state'
        return 1
    }
    [[ "${cmake_cache_sha256}" == "${STORAGE_PROFILE_CMAKE_CACHE_SHA256}" ]] || {
        fail 'shared profile round-trip changed CMake cache state'
        return 1
    }
    [[ "${compile_commands_sha256}" == "${STORAGE_PROFILE_COMPILE_COMMANDS_SHA256}" ]] || {
        fail 'shared profile round-trip changed compile command state'
        return 1
    }
    [[ "${app_size}" == "${STORAGE_PROFILE_APP_SIZE}" ]] || {
        fail 'shared profile round-trip changed app size'
        return 1
    }
    [[ "${app_sha256}" == "${STORAGE_PROFILE_APP_SHA256}" ]] || {
        fail 'shared profile round-trip changed app binary state'
        return 1
    }
    printf 'STEP6A_PROFILE_ROUNDTRIP mode=shared result=PASS sdkconfig_sha256=%s cmake_cache_sha256=%s compile_commands_sha256=%s app_size=%s app_sha256=%s\n' \
        "${sdkconfig_sha256}" "${cmake_cache_sha256}" \
        "${compile_commands_sha256}" "${app_size}" "${app_sha256}"
}

build_storage_image() {
    local build_dir="$1"
    local output="$2"
    shift 2
    local -a builder_args=(
        python3 "${REPOSITORY_ROOT}/tools/emu/build_storage_fatfs_flash.py"
        --repository-root "${REPOSITORY_ROOT}"
        --build-dir "${build_dir}"
        --output "${output}"
    )
    builder_args+=( "$@" )
    "${builder_args[@]}"
}

run_esp_emu() {
    local image="$1"
    local log_path="$2"
    local exit_marker="$3"
    local emulator_timeout="$4"
    local outer_timeout="$5"
    CURRENT_LOG="${log_path}"
    require_file "${image}"
    mkdir -p -- "$(dirname -- "${log_path}")"
    set +e
    timeout --foreground "${outer_timeout}" "${ESP_EMU}" \
        --chip esp32p4 \
        --firmware "${image}" \
        --exit-on "${exit_marker}" \
        --timeout "${emulator_timeout}" \
        --log-color never \
        2>&1 | tee "${log_path}"
    local emulator_status="${PIPESTATUS[0]}"
    set -e
    if (( emulator_status != 0 )); then
        fail "esp-emu failed with status ${emulator_status}; log=${log_path}"
        return 1
    fi
}

run_uart_mode() {
    local mode="$1"
    local image="$2"
    local console_log="$3"
    local uart_log="$4"
    shift 4
    CURRENT_LOG="${console_log}"
    require_file "${image}"
    mkdir -p -- "$(dirname -- "${console_log}")" "$(dirname -- "${uart_log}")"
    local started="$(date +%s)"
    set +e
    timeout --foreground 660s python3 \
        "${REPOSITORY_ROOT}/tools/emu/file_transfer_fatfs_test.py" \
        --firmware "${image}" \
        --log "${console_log%.console.log}.emu.log" \
        --uart-log "${uart_log}" \
        --mode "${mode}" \
        --esp-emu "${ESP_EMU}" \
        "$@" \
        2>&1 | tee "${console_log}"
    local test_status="${PIPESTATUS[0]}"
    set -e
    printf 'STEP6A_UART_MODE mode=%s elapsed_seconds=%s status=%s\n' \
        "${mode}" "$(( $(date +%s) - started ))" "${test_status}"
    if (( test_status != 0 )); then
        fail "FATFS File Transfer ${mode} failed; log=${console_log}"
        return 1
    fi
}

partition_sha256() {
    local image="$1"
    local offset="$2"
    local size="$3"
    local image_size
    image_size="$(stat -c '%s' -- "${image}")" || {
        fail "cannot stat image for hashing: ${image}"
        return 1
    }
    if (( offset < 0 || size <= 0 || offset > image_size ||
          size > image_size - offset )); then
        fail "hash range is outside image: image=${image} size=${image_size} offset=${offset} range=${size}"
        return 1
    fi
    dd if="${image}" bs=1 skip="${offset}" count="${size}" status=none |
        sha256sum | awk '{print $1}'
}

load_partition_geometry() {
    local build_dir="$1"
    local geometry_output
    geometry_output="$(python3 "${REPOSITORY_ROOT}/tools/emu/partition_geometry.py" \
        --idf-path "${IDF_PATH}" \
        --partition-table "${build_dir}/partition_table/partition-table.bin")" || {
        fail "cannot extract generated partition geometry from ${build_dir}"
        return 1
    }
    local key value
    while IFS='=' read -r key value; do
        case "${key}" in
            FACTORY_OFFSET) FACTORY_PARTITION_OFFSET="${value}" ;;
            FACTORY_SIZE) FACTORY_PARTITION_SIZE="${value}" ;;
            NP2TEST_OFFSET) RAW_FIXTURE_OFFSET="${value}" ;;
            NP2TEST_SIZE) RAW_FIXTURE_SIZE="${value}" ;;
            NP2TEST_END) RAW_FIXTURE_END="${value}" ;;
            STORAGE_OFFSET) STORAGE_PARTITION_OFFSET="${value}" ;;
            STORAGE_SIZE) STORAGE_PARTITION_SIZE="${value}" ;;
            STORAGE_END) STORAGE_PARTITION_END="${value}" ;;
            FLASH_SIZE) FLASH_SIZE="${value}" ;;
            *)
                fail "unexpected partition geometry field: ${key}"
                return 1
                ;;
        esac
    done <<< "${geometry_output}"
    local variable
    for variable in FACTORY_PARTITION_OFFSET FACTORY_PARTITION_SIZE \
        RAW_FIXTURE_OFFSET RAW_FIXTURE_SIZE RAW_FIXTURE_END \
        STORAGE_PARTITION_OFFSET STORAGE_PARTITION_SIZE STORAGE_PARTITION_END \
        FLASH_SIZE; do
        if [[ -z "${!variable}" ]]; then
            fail "partition geometry field is missing: ${variable}"
            return 1
        fi
    done
    printf 'STEP6A_PARTITION_GEOMETRY factory=[0x%x,0x%x) np2test=[0x%x,0x%x) storage=[0x%x,0x%x) flash_size=0x%x\n' \
        "${FACTORY_PARTITION_OFFSET}" \
        "$((FACTORY_PARTITION_OFFSET + FACTORY_PARTITION_SIZE))" \
        "${RAW_FIXTURE_OFFSET}" "${RAW_FIXTURE_END}" \
        "${STORAGE_PARTITION_OFFSET}" "${STORAGE_PARTITION_END}" \
        "${FLASH_SIZE}"
}

assert_vfs_result() {
    local log_path="$1"
    require_log "${log_path}" 'NP2REDUCED_VFS_MOUNT'
    require_log "${log_path}" \
        'NP2REDUCED profile=reduced-extmem8 formal_extmem=13 effective_extmem=8'
    require_log "${log_path}" \
        'NP2REDUCED_DISK_SOURCE kind=vfs logical=./np2test-fd1232.hdm physical=/persist/fixtures/np2test-fd1232.hdm'
    require_log "${log_path}" \
        'NP2REDUCED_MEMORY extmem_mb=8 actual_bytes=8388608 ptr_external=1'
    require_log "${log_path}" 'NP2REDUCED_FDD_READY'
    require_log "${log_path}" \
        'NP2REDUCED_PASS completed=13 passed=13 failed=0 stored_crc=0x58f5b827'
    require_log "${log_path}" 'NP2REDUCED_RESULT=PASS'
    if grep -Fq -- 'NP2TEST_RESULT=' "${log_path}"; then
        fail "non-formal VFS run emitted formal NP2TEST_RESULT namespace: ${log_path}"
        return 1
    fi
}

phase_preflight() {
    printf 'STEP6A_PROFILE_BUILD_MODE mode=%s selector_env_reset=%s\n' \
        "${STEP6A_PROFILE_BUILD_MODE}" "${PROFILE_SELECTOR_ENV_VARS}"
    if ! command -v idf.py >/dev/null 2>&1; then
        # ESP-IDF export.sh defines idf.py as a shell function. A workflow
        # invokes this helper as a child shell, so re-source the existing
        # activation wrapper when that function is not inherited.
        source "${SCRIPT_DIR}/activate-idf.sh"
    fi
    [[ -n "${IDF_PATH:-}" && -f "${IDF_PATH}/export.sh" ]] || {
        fail 'IDF_PATH/export.sh is not available; source the pinned ESP-IDF environment first'
        return 1
    }
    command -v idf.py >/dev/null 2>&1 || {
        fail 'idf.py is not available in the active ESP-IDF environment'
        return 1
    }
    [[ -x "${ESP_EMU}" ]] || {
        fail "esp-emu is not executable: ${ESP_EMU}"
        return 1
    }
    [[ "$(idf.py --version)" == "${EXPECTED_IDF_VERSION}" ]] || {
        fail "unexpected ESP-IDF version: $(idf.py --version)"
        return 1
    }
    local emu_version
    emu_version="$(${ESP_EMU} --version 2>&1 | sed -n '/^esp-emu /{p;q;}')"
    [[ "${emu_version}" == "${EXPECTED_EMU_VERSION}" ]] || {
        fail "unexpected esp-emu version: ${emu_version}"
        return 1
    }
    source "${SCRIPT_DIR}/check-firmware-sdkconfig.sh"
    python3 "${REPOSITORY_ROOT}/tools/guest/verify_np2test.py" \
        --layout "${REPOSITORY_ROOT}/tests/guest/np2test/layout.json" \
        --image "${REPOSITORY_ROOT}/tests/guest/np2test/golden/np2test-fd1232.image" \
        --expected-sha256 "${EXPECTED_FIXTURE_SHA256}"
    printf 'STEP6A_TEMP_ROOT=%s\n' "${RUN_ROOT}"
}

phase_raw_reduced() {
    local build_dir="${RUN_ROOT}/raw-reduced"
    local reduced_build_dir="${NP2_REDUCED_PREBUILT_BUILD_DIR:-${build_dir}}"
    CURRENT_LOG="${reduced_build_dir}/esp-emu-np2-stage1-reduced.log"
    NP2_REDUCED_BUILD_DIR="${build_dir}" \
        bash "${REPOSITORY_ROOT}/tools/emu/test-np2-stage1-reduced.sh"
    check_app_size raw-reduced "${reduced_build_dir}"
}

phase_storage_provider() {
    local build_dir
    build_dir="$(selected_profile_build_dir "${RUN_ROOT}/storage-provider")"
    local image="${RUN_ROOT}/storage-provider.bin"
    local log_path="${RUN_ROOT}/storage-provider.log"
    build_profile storage-provider "${build_dir}" STORAGE_FATFS_PROBE STORAGE_FATFS_CI_BOUNDED
    build_storage_image "${build_dir}" "${image}"
    run_esp_emu "${image}" "${log_path}" 'STORAGEFATFS_RESULT=' 120s 150s
    require_log "${log_path}" 'STORAGEFATFS_PROFILE bounded-ci'
    require_log "${log_path}" 'STORAGEFATFS_MOUNT base=/persist partition=storage format_if_mount_failed=0'
    require_log "${log_path}" 'STORAGEFATFS_CAPACITY '
    require_log "${log_path}" "STORAGEFATFS_FIXTURE size=1261568 sha256=${EXPECTED_FIXTURE_SHA256}"
    require_log "${log_path}" 'STORAGEFATFS_REPLACEMENT backup_rename_failure=preserved'
    require_log "${log_path}" 'STORAGEFATFS_REPLACEMENT install_failure_rollback=restored'
    require_log "${log_path}" 'STORAGEFATFS_REPLACEMENT rollback_failure=backup_preserved'
    require_log "${log_path}" 'STORAGEFATFS_REPLACEMENT backup_cleanup_failure=content_committed'
    require_log "${log_path}" 'STORAGEFATFS_STAGING_CLEANUP'
    require_log "${log_path}" 'STORAGEFATFS_MOUNT_CLEANUP unmount_success=clean'
    require_log "${log_path}" 'STORAGEFATFS_MOUNT_CLEANUP unmount_failure=state_retained'
    require_log "${log_path}" 'STORAGEFATFS_REMOUNT persisted=1 sha256_match=1'
    require_log "${log_path}" 'STORAGEFATFS_RESULT=PASS'
    if [[ "${STEP6A_PROFILE_BUILD_MODE}" == shared ]]; then
        capture_storage_profile_state "${build_dir}"
    fi
}

phase_file_transfer() {
    local build_dir
    build_dir="$(selected_profile_build_dir "${RUN_ROOT}/uart-fatfs")"
    local base_image="${RUN_ROOT}/uart-fatfs-base.bin"
    build_profile uart-fatfs "${build_dir}" UART_FATFS_PROFILE
    build_storage_image "${build_dir}" "${base_image}"

    local mode_image console_log uart_log
    mode_image="${RUN_ROOT}/uart-basic.bin"
    console_log="${RUN_ROOT}/uart-basic.console.log"
    uart_log="${RUN_ROOT}/uart-basic.raw.bin"
    cp -- "${base_image}" "${mode_image}"
    run_uart_mode basic "${mode_image}" "${console_log}" "${uart_log}"
    require_log "${console_log}" 'FATFS_FILE_TRANSFER_BASIC=PASS'
    require_log "${console_log}" 'FATFS_MODE_SUMMARY mode=basic '
    require_log "${console_log}" 'result=PASS'

    mode_image="${RUN_ROOT}/uart-large.bin"
    console_log="${RUN_ROOT}/uart-large.console.log"
    uart_log="${RUN_ROOT}/uart-large.raw.bin"
    cp -- "${base_image}" "${mode_image}"
    run_uart_mode large "${mode_image}" "${console_log}" "${uart_log}"
    require_log "${console_log}" 'FATFS_FILE_TRANSFER_LARGE=PASS size=262145'
    require_log "${console_log}" 'boundary_ranges=PASS'
    require_log "${console_log}" 'FATFS_MODE_SUMMARY mode=large '
    require_log "${console_log}" 'result=PASS'

    local nospace_image="${RUN_ROOT}/uart-nospace-preloaded.bin"
    local nospace_metadata="${RUN_ROOT}/uart-nospace-preloaded.geometry.json"
    console_log="${RUN_ROOT}/uart-nospace-preloaded.console.log"
    uart_log="${RUN_ROOT}/uart-nospace-preloaded.raw.bin"
    build_storage_image "${build_dir}" "${nospace_image}" \
        --nospace-derived --metadata-output "${nospace_metadata}"
    require_file "${nospace_metadata}"
    run_uart_mode nospace-preloaded "${nospace_image}" "${console_log}" "${uart_log}" \
        --nospace-metadata "${nospace_metadata}"
    require_log "${console_log}" 'FATFS_FILE_TRANSFER_NOSPACE=PASS'
    require_log "${console_log}" 'failure_phase=begin payload_frames=0'
    require_log "${console_log}" 'preexisting_intact=1 endpoint_idle=1 endpoint_recoverable=1'
    require_log "${console_log}" 'FATFS_MODE_SUMMARY mode=nospace-preloaded '
    require_log "${console_log}" 'result=PASS'

    local persistence_image="${RUN_ROOT}/uart-persistence.bin"
    cp -- "${base_image}" "${persistence_image}"
    console_log="${RUN_ROOT}/uart-persistence-write.console.log"
    uart_log="${RUN_ROOT}/uart-persistence-write.raw.bin"
    run_uart_mode persistence-write "${persistence_image}" "${console_log}" "${uart_log}" \
        --save-state --persistence-size 4097
    require_log "${console_log}" 'FATFS_FILE_TRANSFER_PERSISTENCE created=1 size=4097'
    require_log "${console_log}" 'FATFS_MODE_SUMMARY mode=persistence-write '
    require_log "${console_log}" 'result=PASS'
    console_log="${RUN_ROOT}/uart-persistence-read.console.log"
    uart_log="${RUN_ROOT}/uart-persistence-read.raw.bin"
    run_uart_mode persistence-read "${persistence_image}" "${console_log}" "${uart_log}" \
        --persistence-size 4097
    require_log "${console_log}" 'FATFS_FILE_TRANSFER_PERSISTENCE created=0 reused=1 size=4097'
    require_log "${console_log}" 'FATFS_MODE_SUMMARY mode=persistence-read '
    require_log "${console_log}" 'result=PASS'

    local high_image="${RUN_ROOT}/uart-high-address.bin"
    build_storage_image "${build_dir}" "${high_image}" --high-address-prefill-bytes $((2 * 1024 * 1024))
    console_log="${RUN_ROOT}/uart-high-address.console.log"
    uart_log="${RUN_ROOT}/uart-high-address.raw.bin"
    run_uart_mode high-address-write "${high_image}" "${console_log}" "${uart_log}" \
        --save-state --verify-state "${high_image}" --minimum-offset 0x400000
    require_log "${console_log}" 'FATFS_FILE_TRANSFER_HIGH_ADDRESS upload_size=65536'
    require_log "${console_log}" 'STORAGEFATFS_HIGH_ADDRESS_RAW=PASS marker='
    require_log "${console_log}" 'FATFS_FILE_TRANSFER_HIGH_ADDRESS=PASS save_state=PASS marker='
    require_log "${console_log}" 'FATFS_MODE_SUMMARY mode=high-address-write '
    require_log "${console_log}" 'result=PASS'
}

phase_dosio() {
    local build_dir
    build_dir="$(selected_profile_build_dir "${RUN_ROOT}/dosio")"
    local image="${RUN_ROOT}/dosio.bin"
    local log_path="${RUN_ROOT}/dosio.log"
    build_profile dosio "${build_dir}" NP2_DOSIO_PROBE
    build_storage_image "${build_dir}" "${image}"
    run_esp_emu "${image}" "${log_path}" 'NP2DOSIO_RESULT=' 20s 30s
    require_log "${log_path}" 'NP2DOSIO_ATTACH=PASS'
    require_log "${log_path}" 'NP2DOSIO_ATTR=PASS'
    require_log "${log_path}" 'NP2DOSIO_OPEN=PASS'
    require_log "${log_path}" 'NP2DOSIO_SIZE=PASS bytes=1261568'
    require_log "${log_path}" 'NP2DOSIO_READ=PASS'
    require_log "${log_path}" 'NP2DOSIO_SEEK=PASS'
    require_log "${log_path}" 'NP2DOSIO_EOF=PASS'
    require_log "${log_path}" 'NP2DOSIO_CLOSE=PASS'
    require_log "${log_path}" 'NP2DOSIO_DETACH=PASS'
    require_log "${log_path}" "NP2DOSIO_SHA256 expected=${EXPECTED_FIXTURE_SHA256} actual=${EXPECTED_FIXTURE_SHA256}"
    require_log "${log_path}" 'NP2DOSIO_RESULT=PASS'
}

phase_vfs_np2() {
    local build_dir
    build_dir="$(selected_profile_build_dir "${RUN_ROOT}/vfs")"
    local normal_image="${RUN_ROOT}/vfs-normal.bin"
    local normal_log="${RUN_ROOT}/vfs-normal.log"
    build_profile vfs "${build_dir}" NP2_REDUCED_EXTMEM8 NP2_VFS_FIXTURE_PROFILE
    load_partition_geometry "${build_dir}"
    build_storage_image "${build_dir}" "${normal_image}"
    run_esp_emu "${normal_image}" "${normal_log}" 'NP2REDUCED_RESULT=' 20s 30s
    assert_vfs_result "${normal_log}"

    local poison_image="${RUN_ROOT}/vfs-poisoned.bin"
    local poison_log="${RUN_ROOT}/vfs-poisoned.log"
    local raw_before raw_after storage_before storage_after
    local factory_before factory_after
    local poison_offset="${RAW_FIXTURE_OFFSET}"
    local poison_size=$((0x1000))
    if (( poison_offset < RAW_FIXTURE_OFFSET ||
          poison_offset + poison_size > RAW_FIXTURE_END ||
          poison_offset + poison_size > FLASH_SIZE )); then
        fail "VFS poison range is outside NP2TEST: offset=${poison_offset} size=${poison_size}"
        return 1
    fi
    raw_before="$(partition_sha256 "${normal_image}" "${RAW_FIXTURE_OFFSET}" "${RAW_FIXTURE_SIZE}")"
    storage_before="$(partition_sha256 "${normal_image}" "${STORAGE_PARTITION_OFFSET}" "${STORAGE_PARTITION_SIZE}")"
    factory_before="$(partition_sha256 "${normal_image}" "${FACTORY_PARTITION_OFFSET}" "${FACTORY_PARTITION_SIZE}")"
    [[ "${raw_before}" == "${EXPECTED_FIXTURE_SHA256}" ]] || {
        fail "normal VFS raw partition is not golden: ${raw_before}"
        return 1
    }
    cp -- "${normal_image}" "${poison_image}"
    dd if=/dev/zero of="${poison_image}" bs=4096 \
        seek=$((poison_offset / 4096)) count=$((poison_size / 4096)) \
        conv=notrunc status=none
    raw_after="$(partition_sha256 "${poison_image}" "${RAW_FIXTURE_OFFSET}" "${RAW_FIXTURE_SIZE}")"
    storage_after="$(partition_sha256 "${poison_image}" "${STORAGE_PARTITION_OFFSET}" "${STORAGE_PARTITION_SIZE}")"
    factory_after="$(partition_sha256 "${poison_image}" "${FACTORY_PARTITION_OFFSET}" "${FACTORY_PARTITION_SIZE}")"
    printf 'STEP6A_POISON_GEOMETRY np2test=[0x%x,0x%x) poison=[0x%x,0x%x) storage=[0x%x,0x%x) flash_size=0x%x\n' \
        "${RAW_FIXTURE_OFFSET}" "${RAW_FIXTURE_END}" \
        "${poison_offset}" "$((poison_offset + poison_size))" \
        "${STORAGE_PARTITION_OFFSET}" "${STORAGE_PARTITION_END}" "${FLASH_SIZE}"
    printf 'STEP6A_POISON_RAW before=%s after=%s golden=%s\n' \
        "${raw_before}" "${raw_after}" "${EXPECTED_FIXTURE_SHA256}"
    printf 'STEP6A_POISON_STORAGE before=%s after=%s\n' \
        "${storage_before}" "${storage_after}"
    [[ "${raw_after}" != "${raw_before}" ]] || {
        fail 'raw poison did not change the raw fixture partition'
        return 1
    }
    [[ "${storage_after}" == "${storage_before}" ]] || {
        fail 'raw poison changed the FATFS storage partition'
        return 1
    }
    [[ "${factory_after}" == "${factory_before}" ]] || {
        fail 'raw poison changed the factory application partition'
        return 1
    }
    run_esp_emu "${poison_image}" "${poison_log}" 'NP2REDUCED_RESULT=' 20s 30s
    assert_vfs_result "${poison_log}"
}

phase_size_summary() {
    local summary
    for summary in "${APP_SIZE_SUMMARY[@]}"; do
        printf 'STEP6A_APP_SIZE_SUMMARY %s\n' "${summary}"
    done
}

cd -- "${REPOSITORY_ROOT}"
validate_profile_build_mode
run_phase preflight phase_preflight
run_phase raw-reduced phase_raw_reduced
run_phase storage-provider phase_storage_provider
run_phase file-transfer phase_file_transfer
run_phase dosio phase_dosio
run_phase vfs-np2-and-poison phase_vfs_np2
run_phase profile-roundtrip phase_profile_roundtrip
run_phase application-size-summary phase_size_summary

printf 'STEP6A_TOTAL mode=%s elapsed_seconds=%s\n' \
    "${STEP6A_PROFILE_BUILD_MODE}" "$(( $(date +%s) - TOTAL_START ))"

printf '\nSTEP6A_CI_RESULT=PASS\n'
printf 'raw_reduced=PASS\n'
printf 'storage_provider=PASS\n'
printf 'file_transfer_basic=PASS\n'
printf 'file_transfer_large=PASS\n'
printf 'file_transfer_nospace=PASS\n'
printf 'file_transfer_persistence=PASS\n'
printf 'file_transfer_high_address=PASS\n'
printf 'dosio=PASS\n'
printf 'vfs_np2=PASS\n'
printf 'poisoned_vfs_np2=PASS\n'
printf 'total_elapsed_seconds=%s\n' "$(( $(date +%s) - TOTAL_START ))"
