#!/usr/bin/env bash
# Deterministic 86R.5C.2-F2 PCM lifecycle regression gate.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/p4-audio86-pcm-lifecycle.XXXXXX")
trap 'rm -rf "${work_dir}"' EXIT
cd "${repo_root}"

python3 tools/emu/test_p4_audio86_pcm_output_validator.py
# shellcheck source=tools/emu/activate-idf.sh
source tools/emu/activate-idf.sh

merge_image() {
    local source_dir=$1 output=$2
    python3 "${IDF_PATH}/components/esptool_py/esptool/esptool.py" --chip esp32p4 \
        merge_bin --output "${output}" --format raw --flash_mode dio \
        --flash_freq 80m --flash_size 8MB --fill-flash-size 8MB \
        0x2000 "${source_dir}/bootloader/bootloader.bin" \
        0x8000 "${source_dir}/partition_table/partition-table.bin" \
        0x10000 "${source_dir}/esp_np2kai.bin"
}

for scenario in stop-full fatal-full consumer-failure-full consumer-failure-empty; do
    build_dir="${repo_root}/firmware/build-p4-v3x-audio86-pcm-${scenario}"
    failure_args=()
    case "${scenario}" in
        stop-full) failure_args=(--audio86-failure stop);;
        fatal-full) failure_args=(--audio86-failure fatal);;
    esac
    tools/emu/build-production.sh --variant p4-v3x --board generic \
        --audio86-pcm-lifecycle "${scenario}" "${failure_args[@]}" \
        --esp-emu-test --build-dir "${build_dir}"
    image="${work_dir}/${scenario}.bin"
    merge_image "${build_dir}" "${image}"
    first_evidence=""
    for run in $(seq 1 20); do
        log="${work_dir}/${scenario}-${run}.log"
        timeout --foreground 10s "${HOME}/.local/bin/esp-emu" --chip esp32p4 \
            --firmware "${image}" --exit-on 'main_task: Returned from app_main()' \
            --timeout 8s --log-color never >"${log}" 2>&1
        python3 tools/emu/validate_p4_audio86_pcm_output_log.py \
            --lifecycle-scenario "${scenario}" --log "${log}"
        evidence=$(rg '^P4_AUDIO86_PCM_LIFECYCLE ' "${log}" | sha256sum | awk '{print $1}')
        if [[ -z "${first_evidence}" ]]; then
            first_evidence=${evidence}
        elif [[ "${evidence}" != "${first_evidence}" ]]; then
            echo "ERROR: PCM lifecycle evidence differs: ${scenario} run ${run}" >&2
            exit 1
        fi
    done
done

for kind in stop fatal; do
    build_dir="${repo_root}/firmware/build-p4-v3x-audio86-pcm-${kind}-normal"
    tools/emu/build-production.sh --variant p4-v3x --board generic \
        --audio86-real-guest-pcm-output --audio86-pressure event \
        --audio86-failure "${kind}" --esp-emu-test --build-dir "${build_dir}"
    image="${work_dir}/${kind}-normal.bin"
    merge_image "${build_dir}" "${image}"
    log="${work_dir}/${kind}-normal.log"
    timeout --foreground 10s "${HOME}/.local/bin/esp-emu" --chip esp32p4 \
        --firmware "${image}" --exit-on 'main_task: Returned from app_main()' \
        --timeout 8s --log-color never >"${log}" 2>&1
    python3 tools/emu/validate_p4_audio86_real_guest_log.py --log "${log}" \
        --pressure-scenario event --failure-kind "${kind}" --pcm-output
done

printf 'PCM_FULL_STOP_REPEAT=20/20_PASS\n'
printf 'PCM_FULL_FATAL_REPEAT=20/20_PASS\n'
printf 'PCM_FULL_CONSUMER_FAILURE_REPEAT=20/20_PASS\n'
printf 'PCM_EMPTY_CONSUMER_FAILURE_REPEAT=20/20_PASS\n'
printf '5C2_NORMAL_STOP=PASS\n'
printf '5C2_NORMAL_FATAL=PASS\n'
printf 'FIRST_ERROR_IMMUTABLE=PASS\n'
printf '5C2_LIFECYCLE_HARDENING_MATRIX=16/16_PASS\n'
