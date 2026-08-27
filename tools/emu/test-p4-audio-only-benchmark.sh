#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly FIRMWARE_DIR="${REPOSITORY_ROOT}/firmware"
readonly RUN_ROOT="${P4_AUDIO_RUN_ROOT:-$(mktemp -d "${TMPDIR:-/tmp}/p4audio.XXXXXX")}"
readonly BUILD_DIR="${RUN_ROOT}/build"
readonly MERGED_IMAGE="${RUN_ROOT}/p4-audio-merged.bin"
readonly LOG="${RUN_ROOT}/p4-audio-esp-emu.log"
readonly ESP_EMU="${ESP_EMU:-${HOME}/.local/bin/esp-emu}"

fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

[[ -x "${ESP_EMU}" ]] || fail "esp-emu executable not found: ${ESP_EMU}"
[[ "$(${ESP_EMU} --version 2>&1 | sed -n '/^esp-emu /{p;q;}')" == \
   'esp-emu 0.39.0' ]] || fail 'esp-emu 0.39.0 is required'

# Build and esptool both require the reviewed ESP-IDF environment.
source "${SCRIPT_DIR}/activate-idf.sh"
bash "${SCRIPT_DIR}/build-production.sh" \
    --variant p4-v3x --board generic --audio-only-benchmark \
    --esp-emu-test --build-dir "${BUILD_DIR}"
python3 "${IDF_PATH}/components/esptool_py/esptool/esptool.py" \
    --chip esp32p4 merge_bin --output "${MERGED_IMAGE}" --format raw \
    --flash_mode dio --flash_freq 80m --flash_size 8MB --fill-flash-size 8MB \
    0x2000 "${BUILD_DIR}/bootloader/bootloader.bin" \
    0x10000 "${BUILD_DIR}/esp_np2kai.bin" \
    0x8000 "${BUILD_DIR}/partition_table/partition-table.bin"

set +e
timeout --foreground 240s "${ESP_EMU}" --chip esp32p4 --firmware "${MERGED_IMAGE}" \
    --exit-on 'P4_AUDIO_ONLY_BENCHMARK_RESULT=' --timeout 220s \
    --log-color never 2>&1 | tee "${LOG}"
status="${PIPESTATUS[0]}"
set -e
(( status == 0 )) || fail "esp-emu failed with status ${status}; log=${LOG}"
python3 "${SCRIPT_DIR}/validate_p4_audio_benchmark_log.py" --log "${LOG}"
printf 'P4_AUDIO_EMU_RUN_ROOT=%s\n' "${RUN_ROOT}"
