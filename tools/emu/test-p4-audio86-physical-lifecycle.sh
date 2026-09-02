#!/usr/bin/env bash
# Deterministic esp-emu proof for the physical-start FreeRTOS lifecycle.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/p4-audio86-physical-lifecycle.XXXXXX")
trap 'rm -rf "${work_dir}"' EXIT
cd "${repo_root}"
host_path=${PATH}

# shellcheck source=tools/emu/activate-idf.sh
source tools/emu/activate-idf.sh
python3 tools/emu/check_p4_audio86_callback_idf_barrier.py
python3 tools/emu/check_p4_audio86_project_source.py

merge_image() {
    local source_dir=$1 output=$2
    python3 "${IDF_PATH}/components/esptool_py/esptool/esptool.py" --chip esp32p4 \
        merge_bin --output "${output}" --format raw --flash_mode dio \
        --flash_freq 80m --flash_size 8MB --fill-flash-size 8MB \
        0x2000 "${source_dir}/bootloader/bootloader.bin" \
        0x8000 "${source_dir}/partition_table/partition-table.bin" \
        0x10000 "${source_dir}/esp_np2kai.bin"
}

stages=(early post-i2s post-callback post-codec)
stage_number=0
esp_logs=()
for stage in "${stages[@]}"; do
    stage_number=$((stage_number + 1))
    build_dir="${repo_root}/firmware/build-p4-v3x-audio86-physical-lifecycle-${stage_number}"
    tools/emu/build-production.sh --variant p4-v3x --board generic \
        --audio86-physical-lifecycle-test "${stage}" --esp-emu-test \
        --build-dir "${build_dir}"
    image="${work_dir}/${stage}.bin"
    log="${work_dir}/${stage}.log"
    esp_logs+=("${log}")
    merge_image "${build_dir}" "${image}"
    timeout --foreground 12s "${HOME}/.local/bin/esp-emu" --chip esp32p4 \
        --firmware "${image}" --exit-on 'main_task: Returned from app_main()' \
        --timeout 10s --log-color never >"${log}" 2>&1
    python3 tools/emu/validate_p4_audio86_physical_lifecycle_log.py \
        --stage "${stage_number}" --log "${log}"
    if (( stage_number == 1 )); then
        python3 tools/emu/test_validate_p4_audio86_physical_lifecycle_log.py \
            --stage "${stage_number}" --log "${log}"
    fi
done

env PATH="${host_path}" make -C host build-p4-audio86-physical-sink
host_log="${work_dir}/host.log"
host/build/phase2/tests/p4_nano_audio86_physical_sink_test >"${host_log}"
python3 tools/emu/validate_p4_audio86_physical_sink_log.py "${host_log}"
manifest_args=(--host-log "${host_log}" --require-all-exec)
for log in "${esp_logs[@]}"; do
    manifest_args+=(--esp-log "${log}")
done
python3 tools/emu/test_p4_audio86_physical_sink_manifest.py \
    "${manifest_args[@]}"

printf '5D1_ESP_EMU_LIFECYCLE_PROFILE=PASS\n'
printf 'ESP_EMU_FAKE_BACKEND_SCOPE=LOW_LEVEL_ONLY\n'
printf 'START_FATAL_EARLY_ESP_EMU=PASS\n'
printf 'START_FATAL_POST_I2S_ESP_EMU=PASS\n'
printf 'START_FATAL_POST_CALLBACK_ESP_EMU=PASS\n'
printf 'START_FATAL_POST_CODEC_ESP_EMU=PASS\n'
printf '5D1_START_FAILURE_MATRIX=4/4_PASS\n'
printf '5D1F2_ESP_EMU_LIFECYCLE=PASS scenarios=4 exit_code=0\n'
