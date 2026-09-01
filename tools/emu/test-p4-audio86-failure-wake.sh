#!/usr/bin/env bash
# Deterministic C3 STOP/FATAL gate using the C2 pressure rendezvous.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
kind=${1:?"usage: $0 stop|fatal event|byte|horizon|reset-ack"}
scenario=${2:?"usage: $0 stop|fatal event|byte|horizon|reset-ack"}
case "${kind}" in stop|fatal) ;; *) exit 2;; esac
case "${scenario}" in event|byte|horizon|reset-ack) ;; *) exit 2;; esac
tag="${kind}-${scenario}"
build_dir=${P4_AUDIO86_FAILURE_BUILD_DIR:-"${repo_root}/firmware/build-p4-v3x-audio86-failure-${tag}"}
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/p4-audio86-failure-${tag}.XXXXXX")
trap 'rm -rf "${work_dir}"' EXIT

cd "${repo_root}"
python3 tools/emu/test_p4_audio86_real_guest_validator.py
tools/emu/build-production.sh --variant p4-v3x --board generic \
    --audio86-pressure "${scenario}" --audio86-failure "${kind}" \
    --esp-emu-test --build-dir "${build_dir}"

source tools/emu/activate-idf.sh
image="${work_dir}/failure.bin"
python3 "${IDF_PATH}/components/esptool_py/esptool/esptool.py" --chip esp32p4 \
    merge_bin --output "${image}" --format raw --flash_mode dio --flash_freq 80m \
    --flash_size 8MB --fill-flash-size 8MB \
    0x2000 "${build_dir}/bootloader/bootloader.bin" \
    0x8000 "${build_dir}/partition_table/partition-table.bin" \
    0x10000 "${build_dir}/esp_np2kai.bin"

first_evidence=""
for run in $(seq 1 10); do
    log="${work_dir}/run-${run}.log"
    timeout --foreground 10s "${HOME}/.local/bin/esp-emu" --chip esp32p4 \
        --firmware "${image}" --exit-on 'main_task: Returned from app_main()' \
        --timeout 8s --log-color never >"${log}" 2>&1
    python3 tools/emu/validate_p4_audio86_real_guest_log.py --log "${log}" \
        --pressure-scenario "${scenario}" --failure-kind "${kind}"
    evidence=$(rg '^(P4_AUDIO86_FAILURE|P4_AUDIO86_REAL_GUEST_RESULT|P4_NANO_AUDIO86_REAL_GUEST_STATUS)' \
        "${log}" | sha256sum | awk '{print $1}')
    if [[ -z "${first_evidence}" ]]; then
        first_evidence=${evidence}
    elif [[ "${evidence}" != "${first_evidence}" ]]; then
        echo "ERROR: failure evidence differs on run ${run}" >&2
        exit 1
    fi
done
scenario_upper=${scenario//-/_}
if [[ "${scenario}" == "reset-ack" ]]; then
    scenario_upper=RESET
fi
printf '%s_%s_REPEAT=10/10_PASS\n' "${kind^^}" "${scenario_upper^^}"
printf 'FAILURE_WAKE_TIMING_FLAKES=0\n'
printf 'FAILURE_RAW_FATAL_SCAN=PASS\n'
