#!/usr/bin/env bash
# R11 production-path final RESET publication and partial-failure gate.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_root=${P4_AUDIO86_R11_BUILD_ROOT:-$(
    mktemp -d "${TMPDIR:-/tmp}/p4-audio86-r11-build.XXXXXX"
)}
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/p4-audio86-r11-run.XXXXXX")
readonly esp_emu="${ESP_EMU:-${HOME}/.local/bin/esp-emu}"
trap 'rm -rf "${work_dir}"; if [[ -z "${P4_AUDIO86_R11_BUILD_ROOT:-}" ]]; then rm -rf "${build_root}"; fi' EXIT

cd "${repo_root}"
[[ -x "${esp_emu}" ]] || { echo "esp-emu executable not found" >&2; exit 1; }
python3 tools/emu/test_validate_p4_audio86_terminal_publication_log.py
# shellcheck source=tools/emu/activate-idf.sh
source tools/emu/activate-idf.sh

merge_image() {
    local source_dir=$1 output=$2
    python3 "${IDF_PATH}/components/esptool_py/esptool/esptool.py" \
        --chip esp32p4 merge_bin --output "${output}" --format raw \
        --flash_mode dio --flash_freq 80m --flash_size 8MB \
        --fill-flash-size 8MB \
        0x2000 "${source_dir}/bootloader/bootloader.bin" \
        0x8000 "${source_dir}/partition_table/partition-table.bin" \
        0x10000 "${source_dir}/esp_np2kai.bin" >/dev/null
}

run_image() {
    local image=$1 log=$2
    timeout --foreground 125s "${esp_emu}" --chip esp32p4 \
        --firmware "${image}" \
        --exit-on 'main_task: Returned from app_main()' \
        --timeout 120s --log-color never >"${log}" 2>&1
}

for mode in 1 2; do
    build_dir="${build_root}/mode-${mode}"
    P4_NANO_AUDIO86_TERMINAL_PUBLICATION_TEST=${mode} \
        tools/emu/build-production.sh --variant p4-v3x --board generic \
        --audio86-real-guest-sustained-2s --esp-emu-test \
        --build-dir "${build_dir}" >"${work_dir}/build-${mode}.log" 2>&1
    image="${work_dir}/mode-${mode}.bin"
    log="${work_dir}/mode-${mode}.log"
    merge_image "${build_dir}" "${image}"
    run_image "${image}" "${log}"
    python3 tools/emu/validate_p4_audio86_terminal_publication_log.py \
        --mode "${mode}" --log "${log}"
    if [[ "${mode}" == 1 ]]; then
        python3 tools/emu/validate_p4_audio86_sustained_log.py --log "${log}"
    fi
done

printf '%s\n' \
    'PRODUCTION_PATH_TERMINAL_REGRESSION_TEST=PASS' \
    'PRE_ACK_ACTUAL_PRODUCTION_STATE_TEST=PASS' \
    'NO_FINAL_RESET_WORKER_WAKE_BEFORE_TERMINAL_RELEASE=PASS' \
    'FINAL_RESET_PARTIAL_PUBLICATION_FAILURE_TEST=PASS' \
    'TERMINAL_RETENTION_PRODUCTION_LOGIC_TEST=PASS' \
    'Q399_AND_PCM_DONE_BEFORE_PRODUCER_CONTINUATION=PASS' \
    'TERMINAL_Q399_DEADLINE_MODEL_TEST=PASS' \
    'DEADLINE_MODEL_PHYSICAL_EXEC_CLAIM=NOT_MADE' \
    'R11_PRODUCTION_PATH_TEST_FIDELITY=ACTUAL_HLT_BOARD86_RESET_ADAPTER_BINDING_WORKER_RING_CONTROLLER'
