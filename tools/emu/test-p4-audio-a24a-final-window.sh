#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly ROOT="${P4_AUDIO_A24A_FINAL_WINDOW_ROOT:-$(mktemp -d \
    "${TMPDIR:-/tmp}/p4audio-a24a-window.XXXXXX")}"
readonly ESP_EMU="${ESP_EMU:-${HOME}/.local/bin/esp-emu}"
source "${SCRIPT_DIR}/activate-idf.sh"

[[ -x "${ESP_EMU}" ]] || { echo 'esp-emu executable not found' >&2; exit 1; }
export P4_NANO_AUDIO_FINAL_WINDOW_TEST_CASE=1
trap 'unset P4_NANO_AUDIO_FINAL_WINDOW_TEST_CASE' EXIT

mkdir -p "${ROOT}"
bash "${SCRIPT_DIR}/build-production.sh" --variant p4-v3x --board generic \
    --audio-only-benchmark --audio-opt o2 --esp-emu-test \
    --build-dir "${ROOT}/build" >/dev/null
python3 "${IDF_PATH}/components/esptool_py/esptool/esptool.py" \
    --chip esp32p4 merge_bin --output "${ROOT}/merged.bin" --format raw \
    --flash_mode dio --flash_freq 80m --flash_size 8MB --fill-flash-size 8MB \
    0x2000 "${ROOT}/build/bootloader/bootloader.bin" \
    0x10000 "${ROOT}/build/esp_np2kai.bin" \
    0x8000 "${ROOT}/build/partition_table/partition-table.bin" >/dev/null

log="${ROOT}/run.log"
timeout --foreground 60s "${ESP_EMU}" --chip esp32p4 --firmware "${ROOT}/merged.bin" \
    --exit-on 'P4_AUDIO_ONLY_BENCHMARK_RESULT=PASS' --timeout 55s \
    --log-color never 2>&1 | tee "${log}" >/dev/null

grep -q 'A2_FINAL_PUBLICATION_WINDOW_TEST=PASS' "${log}"
grep -q 'P4_AUDIO_A2_RESULT .*consumer_pacing=ESP_TIMER_5MS' "${log}"
grep -q 'P4_AUDIO_COMPUTE_SERVICE .*workload=RETROFM mode=TIMING' "${log}"
grep -q 'P4_AUDIO_COMPUTE_SERVICE .*timing_valid=YES compute_underflow_count=0' "${log}"
grep -q 'P4_AUDIO_TIMER_LIFECYCLE workload=RETROFM mode=TIMING' "${log}"
! grep -q 'stage=pacing_completion' "${log}"
grep -q 'P4_AUDIO_ONLY_BENCHMARK_RESULT=PASS' "${log}"
printf 'A2_FINAL_PUBLICATION_WINDOW_TEST=PASS log=%s\n' "${log}"
