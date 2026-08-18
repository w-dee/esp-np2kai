#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly FIRMWARE_DIR="${REPOSITORY_ROOT}/firmware"
readonly EXPECTED_IDF_VERSION="ESP-IDF v5.5.4"
readonly EXPECTED_EMU_VERSION="esp-emu 0.39.0"
readonly EXPECTED_FIXTURE_SHA256="3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3"
readonly APP_PARTITION_SIZE=$((0x100000))
readonly RAW_FIXTURE_OFFSET=$((0x110000))
readonly RAW_FIXTURE_SIZE=$((0x134000))
readonly STORAGE_PARTITION_OFFSET=$((0x244000))
readonly STORAGE_PARTITION_SIZE=$((0x5bc000))
readonly ESP_EMU="${ESP_EMU:-${HOME}/.local/bin/esp-emu}"
readonly RUN_ROOT="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/esp-np2kai-step6a.XXXXXX")"

declare -a APP_SIZE_SUMMARY=()
CURRENT_PHASE="preflight"
CURRENT_LOG="${RUN_ROOT}"
TOTAL_START="$(date +%s)"

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
    local app_size
    app_size="$(stat -c '%s' "${app_binary}")"
    if (( app_size > APP_PARTITION_SIZE )); then
        fail "${profile} app exceeds factory partition: ${app_size} > ${APP_PARTITION_SIZE}"
        return 1
    fi
    local remaining=$((APP_PARTITION_SIZE - app_size))
    local summary
    summary="profile=${profile} app_size_hex=$(printf '0x%x' "${app_size}") partition_size=0x100000 remaining_hex=$(printf '0x%x' "${remaining}")"
    APP_SIZE_SUMMARY+=("${summary}")
    printf 'STEP6A_APP_SIZE %s\n' "${summary}"
}

build_profile() {
    local profile="$1"
    local build_dir="$2"
    shift 2
    local sdkconfig_path="${build_dir}/sdkconfig"
    local -a cmake_args=(
        -B "${build_dir}"
        -D "SDKCONFIG=${sdkconfig_path}"
    )
    local flag
    for flag in "$@"; do
        cmake_args+=( -D "${flag}=1" )
    done

    mkdir -p -- "${build_dir}"
    (
        cd -- "${FIRMWARE_DIR}"
        if [[ ! -f "${sdkconfig_path}" ]]; then
            idf.py -B "${build_dir}" -D "SDKCONFIG=${sdkconfig_path}" set-target esp32p4
        fi
        check_firmware_sdkconfig "${sdkconfig_path}"
        idf.py "${cmake_args[@]}" reconfigure
        check_firmware_sdkconfig "${sdkconfig_path}"
        idf.py "${cmake_args[@]}" build
    )
    check_app_size "${profile}" "${build_dir}"
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
    dd if="${image}" bs=1 skip="${offset}" count="${size}" status=none |
        sha256sum | awk '{print $1}'
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
    local build_dir="${RUN_ROOT}/storage-provider"
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
}

phase_file_transfer() {
    local build_dir="${RUN_ROOT}/uart-fatfs"
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
    console_log="${RUN_ROOT}/uart-nospace-preloaded.console.log"
    uart_log="${RUN_ROOT}/uart-nospace-preloaded.raw.bin"
    build_storage_image "${build_dir}" "${nospace_image}" --nospace-prefill-bytes $((4 * 1024 * 1024))
    run_uart_mode nospace-preloaded "${nospace_image}" "${console_log}" "${uart_log}"
    require_log "${console_log}" 'FATFS_FILE_TRANSFER_NOSPACE=PASS'
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
    require_log "${console_log}" 'FATFS_FILE_TRANSFER_HIGH_ADDRESS=PASS save_state=PASS marker='
    require_log "${console_log}" 'FATFS_MODE_SUMMARY mode=high-address-write '
    require_log "${console_log}" 'result=PASS'
}

phase_dosio() {
    local build_dir="${RUN_ROOT}/dosio"
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
    local build_dir="${RUN_ROOT}/vfs"
    local normal_image="${RUN_ROOT}/vfs-normal.bin"
    local normal_log="${RUN_ROOT}/vfs-normal.log"
    build_profile vfs "${build_dir}" NP2_REDUCED_EXTMEM8 NP2_VFS_FIXTURE_PROFILE
    build_storage_image "${build_dir}" "${normal_image}"
    run_esp_emu "${normal_image}" "${normal_log}" 'NP2REDUCED_RESULT=' 20s 30s
    assert_vfs_result "${normal_log}"

    local poison_image="${RUN_ROOT}/vfs-poisoned.bin"
    local poison_log="${RUN_ROOT}/vfs-poisoned.log"
    local raw_before raw_after storage_before storage_after
    raw_before="$(partition_sha256 "${normal_image}" "${RAW_FIXTURE_OFFSET}" "${RAW_FIXTURE_SIZE}")"
    storage_before="$(partition_sha256 "${normal_image}" "${STORAGE_PARTITION_OFFSET}" "${STORAGE_PARTITION_SIZE}")"
    [[ "${raw_before}" == "${EXPECTED_FIXTURE_SHA256}" ]] || {
        fail "normal VFS raw partition is not golden: ${raw_before}"
        return 1
    }
    cp -- "${normal_image}" "${poison_image}"
    dd if=/dev/zero of="${poison_image}" bs=4096 seek=$((RAW_FIXTURE_OFFSET / 4096)) \
        count=$((0x1000 / 4096)) conv=notrunc status=none
    raw_after="$(partition_sha256 "${poison_image}" "${RAW_FIXTURE_OFFSET}" "${RAW_FIXTURE_SIZE}")"
    storage_after="$(partition_sha256 "${poison_image}" "${STORAGE_PARTITION_OFFSET}" "${STORAGE_PARTITION_SIZE}")"
    printf 'STEP6A_POISON_RAW before=%s after=%s golden=%s\n' \
        "${raw_before}" "${raw_after}" "${EXPECTED_FIXTURE_SHA256}"
    printf 'STEP6A_POISON_STORAGE before=%s after=%s\n' \
        "${storage_before}" "${storage_after}"
    [[ "${raw_after}" != "${EXPECTED_FIXTURE_SHA256}" ]] || {
        fail 'raw poison did not change the raw fixture partition'
        return 1
    }
    [[ "${storage_after}" == "${storage_before}" ]] || {
        fail 'raw poison changed the FATFS storage partition'
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
run_phase preflight phase_preflight
run_phase raw-reduced phase_raw_reduced
run_phase storage-provider phase_storage_provider
run_phase file-transfer phase_file_transfer
run_phase dosio phase_dosio
run_phase vfs-np2-and-poison phase_vfs_np2
run_phase application-size-summary phase_size_summary

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
