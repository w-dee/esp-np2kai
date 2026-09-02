#!/usr/bin/env bash
# Deterministic 86R.5C.3-S1 P4 PCM RETRY lifecycle gate.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_root=${P4_AUDIO86_PCM_RETRY_BUILD_ROOT:-"${repo_root}/firmware"}
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/p4-audio86-pcm-retry.XXXXXX")
trap 'rm -rf "${work_dir}"' EXIT
readonly esp_emu="${ESP_EMU:-${HOME}/.local/bin/esp-emu}"

cd "${repo_root}"
[[ -x "${esp_emu}" ]] || { echo "esp-emu executable not found" >&2; exit 1; }
python3 tools/emu/test_p4_audio86_pcm_retry_validator.py
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

run_case() {
    local scenario=$1 image=$2 log=$3 batch=${4:-}
    local batch_args=()
    [[ -z "${batch}" ]] || batch_args=(--batch-size "${batch}")
    timeout --foreground 25s "${esp_emu}" --chip esp32p4 \
        --firmware "${image}" --exit-on 'main_task: Returned from app_main()' \
        --timeout 20s "${batch_args[@]}" --log-color never >"${log}" 2>&1
    python3 tools/emu/validate_p4_audio86_pcm_retry_log.py \
        --scenario "${scenario}" --log "${log}" >/dev/null
}

for scenario in retry-stop retry-fatal retry-primary-first retry-consumer-first; do
    build_dir="${build_root}/build-p4-v3x-audio86-pcm-${scenario}"
    tools/emu/build-production.sh --variant p4-v3x --board generic \
        --audio86-pcm-lifecycle "${scenario}" --esp-emu-test \
        --build-dir "${build_dir}"
    image="${work_dir}/${scenario}.bin"
    merge_image "${build_dir}" "${image}"
    first_evidence=""
    for run in $(seq 1 20); do
        log="${work_dir}/${scenario}-${run}.log"
        run_case "${scenario}" "${image}" "${log}"
        evidence=$(rg '^(P4_AUDIO86_PCM_LIFECYCLE |P4_AUDIO86_PCM_RETRY |P4_AUDIO86_REAL_GUEST_RESULT=|P4_NANO_AUDIO86_REAL_GUEST_STATUS=)' \
            "${log}" | sha256sum | awk '{print $1}')
        if [[ -z "${first_evidence}" ]]; then
            first_evidence=${evidence}
        elif [[ "${evidence}" != "${first_evidence}" ]]; then
            echo "ERROR: RETRY evidence differs: ${scenario} run ${run}" >&2
            exit 1
        fi
    done
    printf '%s_REPEAT=20/20_PASS\n' "$(tr '[:lower:]-' '[:upper:]_' <<<"${scenario}")"
    if [[ "${scenario}" == retry-stop || "${scenario}" == retry-primary-first ]]; then
        for batch in 1000 50000 500000; do
            run_case "${scenario}" "${image}" \
                "${work_dir}/${scenario}-batch-${batch}.log" "${batch}"
        done
    fi
done

printf '5C3S1_SCHEDULER_PERTURBATION=PASS\n'
printf 'PCM_RETRY_TERMINAL_MATRIX=4/4_PASS\n'
printf '5C3S1_VALIDATOR=PASS\n'
printf '5C3S1_VALIDATOR_MUTATIONS=23_ALL_REJECTED\n'
