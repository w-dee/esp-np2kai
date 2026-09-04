#!/usr/bin/env bash
# Slice D: normal sustained success plus READY-then-noncompletion timeout.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/p4-audio86-outer-timeout.XXXXXX")
normal_build=${P4_AUDIO86_SLICE_D_NORMAL_BUILD:-"${repo_root}/firmware/build-generic-p4-v3x-audio86-sustained-2s"}
timeout_build=${P4_AUDIO86_SLICE_D_TIMEOUT_BUILD:-"${repo_root}/firmware/build-generic-p4-v3x-audio86-outer-timeout-test"}
esp_emu=${ESP_EMU:-"${HOME}/.local/bin/esp-emu"}
trap 'rm -rf "${work_dir}"' EXIT

cd "${repo_root}"
[[ -x "${esp_emu}" ]] || { echo "esp-emu executable not found" >&2; exit 1; }
[[ "$("${esp_emu}" --version 2>&1 | sed -n '/^esp-emu /{p;q;}')" == \
   'esp-emu 0.39.0' ]] || { echo 'esp-emu 0.39.0 is required' >&2; exit 1; }

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
    local image=$1 log=$2 limit=$3
    timeout --foreground "$((limit + 5))s" "${esp_emu}" --chip esp32p4 \
        --firmware "${image}" \
        --exit-on 'main_task: Returned from app_main()' \
        --timeout "${limit}s" --log-color never >"${log}" 2>&1
    sed -i 's/\r$//' "${log}"
}

tools/emu/build-production.sh --variant p4-v3x --board generic \
    --audio86-real-guest-sustained-2s --esp-emu-test \
    --build-dir "${normal_build}" >/dev/null
merge_image "${normal_build}" "${work_dir}/normal.bin"
run_image "${work_dir}/normal.bin" "${work_dir}/normal.log" 120
python3 tools/emu/validate_p4_audio86_sustained_log.py \
    --log "${work_dir}/normal.log"

tools/emu/build-production.sh --variant p4-v3x --board generic \
    --audio86-outer-timeout-test --build-dir "${timeout_build}" >/dev/null
merge_image "${timeout_build}" "${work_dir}/timeout.bin"
run_image "${work_dir}/timeout.bin" "${work_dir}/timeout.log" 12

timeout_log=${work_dir}/timeout.log
[[ $(rg -c '^P4_AUDIO86_PHYSICAL_5D3_S1_TERMINAL=FAILED$' "${timeout_log}") == 1 ]]
rg -q '^P4_AUDIO86_OUTER_TIMEOUT_TEST=READY_THEN_STALL$' "${timeout_log}"
rg -q '^P4_AUDIO86_OUTER_TIMEOUT class=OUTER_COMPLETION_TIMEOUT inner_result=INDETERMINATE .*guard_ms=30000$' "${timeout_log}"
rg -q '^P4_AUDIO86_TIMEOUT_SNAPSHOT coherence=PASS .*service_observable=0 ' "${timeout_log}"
rg -q '^P4_AUDIO86_TIMEOUT_OWNER_PROGRESS coherence=PASS history_depth=4 history_count=0 subphase=UNAVAILABLE progress_pattern=INSUFFICIENT_HISTORY checkpoint_calls=0 checkpoint_success=0 ' "${timeout_log}"
rg -q '^P4_AUDIO86_TIMEOUT_OWNER_BACKPRESSURE transaction_active=4294967295 reserved_event_slots=4294967295 reserved_byte_count=4294967295 horizon_owned=4294967295 horizon_mailbox_state=4294967295 transaction_waiting=4294967295 progress_checkpoint_retrying=4294967295 current_checkpoint_retry_count=4294967295 max_checkpoint_retry_count=4294967295$' "${timeout_log}"
[[ $(rg -c '^P4_AUDIO86_TIMEOUT_CHECKPOINT index=[0-3] valid=0 ' "${timeout_log}") == 4 ]]
[[ $(rg -c '^P4_AUDIO86_TIMEOUT_PROGRESS_DELTA index=[0-2] valid=0 ' "${timeout_log}") == 3 ]]
rg -q '^P4_AUDIO86_TIMEOUT_STOP attempted=NO result=-1 quiescence=UNPROVEN owner_deleted=NO resources_reclaimed=NO$' "${timeout_log}"
rg -q '^P4_AUDIO86_REAL_GUEST_RESULT=FAIL reason=OUTER_COMPLETION_TIMEOUT inner_result=INDETERMINATE$' "${timeout_log}"
rg -q '^P4_NANO_AUDIO86_REAL_GUEST_STATUS=FAIL$' "${timeout_log}"
! rg -q 'P4_NANO_AUDIO86_REAL_GUEST_STATUS=PASS|Guru Meditation|Task watchdog|assert failed|panic' "${timeout_log}"

if python3 tools/emu/validate_p4_audio86_sustained_log.py \
       --log "${timeout_log}" >/dev/null 2>&1; then
    echo 'timeout diagnostic capture unexpectedly passed acceptance validator' >&2
    exit 1
fi

printf '%s\n' \
    'AUDIO86_OUTER_NORMAL_SUCCESS=PASS' \
    'AUDIO86_OUTER_TIMEOUT_INJECTION=PASS' \
    'AUDIO86_OUTER_TIMEOUT_FORMAL_TERMINAL=1_FAILED' \
    'AUDIO86_OUTER_TIMEOUT_OWNER_PROGRESS_DIAGNOSTIC=PASS' \
    'AUDIO86_OUTER_TIMEOUT_FORCED_DELETE=NO' \
    'AUDIO86_OUTER_TIMEOUT_VALIDATOR_REJECTION=PASS'
