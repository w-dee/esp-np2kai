#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly RUN_ROOT="${P4_AUDIO_A23A_FAULT_ROOT:-$(mktemp -d "${TMPDIR:-/tmp}/p4audio-a23a-faults.XXXXXX")}"
readonly ESP_EMU="${ESP_EMU:-${HOME}/.local/bin/esp-emu}"
source "${SCRIPT_DIR}/activate-idf.sh"

[[ -x "${ESP_EMU}" ]] || { echo 'esp-emu executable not found' >&2; exit 1; }
for case_id in 1 2 3 4; do
    case_root="${RUN_ROOT}/case-${case_id}"
    mkdir -p "${case_root}"
    export P4_NANO_AUDIO_LIFECYCLE_FAULT_CASE="${case_id}"
    bash "${SCRIPT_DIR}/build-production.sh" --variant p4-v3x --board generic \
        --audio-only-benchmark --audio-opt o2 --esp-emu-test \
        --build-dir "${case_root}/build" >/dev/null
    python3 "${IDF_PATH}/components/esptool_py/esptool/esptool.py" \
        --chip esp32p4 merge_bin --output "${case_root}/merged.bin" --format raw \
        --flash_mode dio --flash_freq 80m --flash_size 8MB --fill-flash-size 8MB \
        0x2000 "${case_root}/build/bootloader/bootloader.bin" \
        0x10000 "${case_root}/build/esp_np2kai.bin" \
        0x8000 "${case_root}/build/partition_table/partition-table.bin" >/dev/null
    log="${case_root}/run.log"
    if [[ "${case_id}" == 4 ]]; then
        set +e
        timeout --foreground 8s "${ESP_EMU}" --chip esp32p4 \
            --firmware "${case_root}/merged.bin" --timeout 6s --log-color never 2>&1 | tee "${log}" >/dev/null
        set -e
        grep -q 'P4_AUDIO_A2_QUIESCENCE result=FAIL' "${log}"
        grep -q 'consumer=MISSING resources_retained=YES coordinator=FAIL_STOP' "${log}"
        ! grep -q 'P4_AUDIO_RESULT .*identity=PASS' "${log}"
    else
        timeout --foreground 30s "${ESP_EMU}" --chip esp32p4 \
            --firmware "${case_root}/merged.bin" \
            --exit-on 'P4_AUDIO_ONLY_BENCHMARK_RESULT=' --timeout 25s \
            --log-color never 2>&1 | tee "${log}" >/dev/null
        grep -q 'P4_AUDIO_A2_QUIESCENCE result=PASS' "${log}"
        grep -q 'P4_AUDIO_FAILURE workload=' "${log}"
        ! grep -q 'P4_AUDIO_A2_QUIESCENCE result=FAIL' "${log}"
    fi
    printf 'P4_AUDIO_A23A_FAULT_CASE=%s PASS\n' "${case_id}"
done
unset P4_NANO_AUDIO_LIFECYCLE_FAULT_CASE
