#!/usr/bin/env bash
# Canonical ESP-EMU correctness gate for the real PC-9801-86 guest profile.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=${P4_AUDIO86_REAL_GUEST_BUILD_DIR:-"${repo_root}/firmware/build-p4-v3x-audio86-real-guest"}
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/p4-audio86-real-guest.XXXXXX")
trap 'rm -rf "${work_dir}"' EXIT

cd "${repo_root}"
python3 tools/emu/test_p4_audio86_real_guest_validator.py
tools/emu/build-production.sh --variant p4-v3x --board generic \
    --audio86-real-guest --esp-emu-test --build-dir "${build_dir}"

# shellcheck source=tools/emu/activate-idf.sh
source tools/emu/activate-idf.sh
image="${work_dir}/real-guest.bin"
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
    python3 tools/emu/validate_p4_audio86_real_guest_log.py --log "${log}"
    evidence=$(rg '^(P4_AUDIO86_REAL_GUEST profile|P4_AUDIO86_REAL_GUEST_RESIDUAL|GUEST_IO_|AUDIO_EVENTS_|PCM86_|TIMER_PIC_|FINAL_G_STATE_|WORKER_APPLY_TRACE_|PRE_RESET_PCM_|FULL_PCM_|P4_AUDIO86_ACTION |P4_AUDIO86_REAL_GUEST_RESULT|P4_NANO_AUDIO86_REAL_GUEST_STATUS)' "${log}" | sha256sum | awk '{print $1}')
    if [[ -z "${first_evidence}" ]]; then
        first_evidence=${evidence}
    elif [[ "${evidence}" != "${first_evidence}" ]]; then
        echo "ERROR: canonical evidence differs on run ${run}" >&2
        exit 1
    fi
done
printf 'P4_REAL_GUEST_REPEATABILITY=10/10_PASS\n'
printf 'P4_REAL_GUEST_FINAL_RESIDUALS=PASS\n'
printf 'P4_AUDIO86_REAL_GUEST_IMAGE_BYTES=%s\n' "$(stat -c '%s' "${build_dir}/esp_np2kai.bin")"
