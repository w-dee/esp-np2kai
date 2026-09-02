#!/usr/bin/env bash
# Canonical ESP-EMU gate for the 86R.5C.2 virtual PCM output consumer.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=${P4_AUDIO86_PCM_OUTPUT_BUILD_DIR:-"${repo_root}/firmware/build-p4-v3x-audio86-pcm-output"}
partial_build_dir=${P4_AUDIO86_PCM_PARTIAL_BUILD_DIR:-"${repo_root}/firmware/build-p4-v3x-audio86-pcm-output-partial"}
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/p4-audio86-pcm-output.XXXXXX")
trap 'rm -rf "${work_dir}"' EXIT

cd "${repo_root}"
python3 tools/emu/test_p4_audio86_pcm_output_validator.py
tools/emu/build-production.sh --variant p4-v3x --board generic \
    --audio86-real-guest-pcm-output --esp-emu-test --build-dir "${build_dir}"

# shellcheck source=tools/emu/activate-idf.sh
source tools/emu/activate-idf.sh
merge_image() {
    local source_dir=$1 output=$2
    python3 "${IDF_PATH}/components/esptool_py/esptool/esptool.py" --chip esp32p4 \
        merge_bin --output "${output}" --format raw --flash_mode dio --flash_freq 80m \
        --flash_size 8MB --fill-flash-size 8MB \
        0x2000 "${source_dir}/bootloader/bootloader.bin" \
        0x8000 "${source_dir}/partition_table/partition-table.bin" \
        0x10000 "${source_dir}/esp_np2kai.bin"
}

image="${work_dir}/pcm-output.bin"
merge_image "${build_dir}" "${image}"
first_evidence=""
for run in $(seq 1 10); do
    log="${work_dir}/run-${run}.log"
    timeout --foreground 10s "${HOME}/.local/bin/esp-emu" --chip esp32p4 \
        --firmware "${image}" --exit-on 'main_task: Returned from app_main()' \
        --timeout 8s --log-color never >"${log}" 2>&1
    python3 tools/emu/validate_p4_audio86_pcm_output_log.py --log "${log}"
    evidence=$(rg '^(RING_|P4_AUDIO86_PCM_|FULL_PCM_|PRE_RESET_PCM_)' "${log}" | sha256sum | awk '{print $1}')
    if [[ -z "${first_evidence}" ]]; then
        first_evidence=${evidence}
    elif [[ "${evidence}" != "${first_evidence}" ]]; then
        echo "ERROR: PCM output evidence differs on run ${run}" >&2
        exit 1
    fi
done
printf '5C2_CANONICAL_REPEATABILITY=10/10_PASS\n'
printf 'PCM_PREFILL_4=PASS\n'
printf 'RESET_INSIDE_Q240_REAL_PATH=PASS\n'

tools/emu/build-production.sh --variant p4-v3x --board generic \
    --audio86-real-guest-pcm-output-partial --esp-emu-test \
    --build-dir "${partial_build_dir}"
partial_image="${work_dir}/partial.bin"
partial_log="${work_dir}/partial.log"
merge_image "${partial_build_dir}" "${partial_image}"
timeout --foreground 10s "${HOME}/.local/bin/esp-emu" --chip esp32p4 \
    --firmware "${partial_image}" --exit-on 'main_task: Returned from app_main()' \
    --timeout 8s --log-color never >"${partial_log}" 2>&1
python3 tools/emu/validate_p4_audio86_pcm_output_log.py --partial --log "${partial_log}"
printf '5C2_PCM_OUTPUT_ESP_EMU=PASS\n'
