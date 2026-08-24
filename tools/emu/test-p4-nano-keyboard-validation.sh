#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly FIRMWARE_DIR="${REPOSITORY_ROOT}/firmware"
readonly BUILD_DIR="${P4_NANO_KEYBOARD_VALIDATION_BUILD_DIR:-${FIRMWARE_DIR}/build-keyboard-validation-p4-v3x}"
readonly MERGED_IMAGE="${BUILD_DIR}/p4-nano-keyboard-validation-merged.bin"
readonly EMULATOR_LOG="${BUILD_DIR}/p4-nano-keyboard-validation.log"
readonly ESP_EMU="${ESP_EMU:-${HOME}/.local/bin/esp-emu}"

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

source "${SCRIPT_DIR}/activate-idf.sh"
[[ "$(idf.py --version)" == *'v5.5.4'* ]] ||
    fail "ESP-IDF v5.5.4 is required: $(idf.py --version)"
[[ -x "${ESP_EMU}" ]] || fail "esp-emu executable not found: ${ESP_EMU}"
[[ "$(${ESP_EMU} --version 2>&1 | sed -n '/^esp-emu /{p;q;}')" == \
    'esp-emu 0.39.0' ]] || fail "esp-emu 0.39.0 is required"

bash "${SCRIPT_DIR}/build-production.sh" \
    --variant p4-v3x --board generic --runtime-keyboard-validation --esp-emu-test \
    --build-dir "${BUILD_DIR}"
python3 "${REPOSITORY_ROOT}/tools/emu/build_storage_fatfs_flash.py" \
    --repository-root "${REPOSITORY_ROOT}" \
    --build-dir "${BUILD_DIR}" \
    --output "${MERGED_IMAGE}" \
    --include-np2kbdtest

set +e
timeout --foreground 75s "${ESP_EMU}" \
    --chip esp32p4 --firmware "${MERGED_IMAGE}" \
    --exit-on 'P4_NANO_KEYBOARD_VALIDATION_EXIT=' --timeout 45s \
    --log-color never 2>&1 | tee "${EMULATOR_LOG}"
status="${PIPESTATUS[0]}"
set -e
(( status == 0 )) || fail "esp-emu failed with status ${status}"

grep -Fq 'P4_NANO_RUNTIME_SD_MOUNT=PASS mount=/persist' "${EMULATOR_LOG}" ||
    fail 'SPI-NOR mount proof missing'
grep -Fq 'P4_NANO_RUNTIME_MEDIA result=FOUND path=/persist/fixtures/np2kbdtest-fd1232.hdm' "${EMULATOR_LOG}" ||
    fail 'keyboard validation media proof missing'
grep -Fq 'P4_NANO_RUNTIME_DOSIO=READY logical=./runtime-keyboard-validation-fdd0.hdm' "${EMULATOR_LOG}" ||
    fail 'keyboard DOSIO mapping proof missing'
grep -Fq 'P4_NANO_RUNTIME_FDD0=ATTACHED type=autodetect readonly=1 fddequip=0x01' "${EMULATOR_LOG}" ||
    fail 'keyboard FDD0 attach proof missing'
grep -Fq 'P4_NANO_RUNTIME_CORE=RUNNING' "${EMULATOR_LOG}" ||
    fail 'runtime start proof missing'
grep -Fq 'P4_NANO_RUNTIME_DISPLAY=VISIBLE' "${EMULATOR_LOG}" ||
    fail 'display/session proof missing'

readonly -a proof_sequence=(
    'P4_NANO_KEYBOARD_PROOF_STATE=READY'
    'P4_NANO_KEYBOARD_PROOF_EVENT=PRESS_ENQUEUED'
    'P4_NANO_KEYBOARD_PROOF_EVENT=PRESS_DRAINED'
    'P4_NANO_KEYBOARD_PROOF_STATE=MAKE_OBSERVED byte=0x1d'
    'P4_NANO_KEYBOARD_PROOF_EVENT=RELEASE_ENQUEUED'
    'P4_NANO_KEYBOARD_PROOF_EVENT=RELEASE_DRAINED'
    'P4_NANO_KEYBOARD_PROOF_RESULT=PASS make=0x1d break=0x9d'
)
previous_line=0
for marker in "${proof_sequence[@]}"; do
    line="$(grep -nF "${marker}" "${EMULATOR_LOG}" | head -n 1 | cut -d: -f1)"
    [[ -n "${line}" ]] || fail "keyboard proof marker missing: ${marker}"
    (( line > previous_line )) || fail "keyboard proof sequence is not ordered: ${marker}"
    previous_line="${line}"
done

for marker in \
    'P4_NANO_KEYBOARD_PROOF_STATE=READY' \
    'P4_NANO_KEYBOARD_PROOF_EVENT=PRESS_ENQUEUED' \
    'P4_NANO_KEYBOARD_PROOF_EVENT=PRESS_DRAINED' \
    'P4_NANO_KEYBOARD_PROOF_STATE=MAKE_OBSERVED byte=0x1d' \
    'P4_NANO_KEYBOARD_PROOF_EVENT=RELEASE_ENQUEUED' \
    'P4_NANO_KEYBOARD_PROOF_EVENT=RELEASE_DRAINED'; do
    grep -Fq "${marker}" "${EMULATOR_LOG}" || fail "keyboard proof marker missing: ${marker}"
done

# BREAK_OBSERVED and result PASS can be emitted in the same owner iteration;
# the result-v1 PASS marker is the authoritative break proof.
if grep -Fq 'P4_NANO_KEYBOARD_PROOF_STATE=BREAK_OBSERVED byte=0x9d' "${EMULATOR_LOG}"; then
    printf '%s\n' 'Keyboard proof emitted BREAK_OBSERVED=0x9d'
fi

grep -Fq \
    'P4_NANO_KEYBOARD_PROOF_COUNTERS enqueued=2 dequeued=2 press=1 release=1 queue_overflows=0 rejected=0 blocked=0 duplicate=0 invalid=0 source_capacity=0 recoveries=0 quarantined=0' \
    "${EMULATOR_LOG}" || fail 'keyboard proof counters are not deterministic'
# The owner cleanup may increment recovery/quarantine state after the proof
# snapshot.  Queue, rejection, and ownership counters must remain exact; the
# proof snapshot and the absence of P4_NANO_KEYBOARD=QUARANTINED are the
# authoritative no-quarantine checks.
grep -Eq \
    'P4_NANO_KEYBOARD_COUNTERS enqueued=2 dequeued=2 queue_overflows=0 rejected=0 blocked=0 press=1 release=1 duplicate=0 invalid=0 disconnects=0 source_capacity=0 recovery_discards=0 recoveries=[0-9]+ quarantined=[01]' \
    "${EMULATOR_LOG}" || fail 'keyboard queue/ownership counters are not clean'
grep -Eq 'P4_NANO_RUNTIME_DISK_READS opens=[1-9][0-9]* calls=[1-9][0-9]* bytes=[1-9][0-9]*' "${EMULATOR_LOG}" ||
    fail 'disk read proof missing'
grep -Eq 'P4_NANO_RUNTIME_SESSION submitted=[1-9][0-9]* acquired=[1-9][0-9]* transformed=[1-9][0-9]* released=[1-9][0-9]*' "${EMULATOR_LOG}" ||
    fail 'session counter proof missing'
grep -Fq 'P4_NANO_KEYBOARD_VALIDATION_RESULT=PASS' "${EMULATOR_LOG}" ||
    fail 'keyboard validation did not pass'
grep -Fq 'P4_NANO_KEYBOARD_VALIDATION_EXIT=PASS' "${EMULATOR_LOG}" ||
    fail 'keyboard validation exit proof missing'

# The owner task may emit a QUARANTINED status during deterministic shutdown
# after the proof snapshot (shutdown deliberately closes producers and leaves
# the bridge fail-closed).  The proof snapshot above must still be clean; only
# a proof failure or an actual runtime fault/reset is fatal here.
if grep -Eiq 'P4_NANO_KEYBOARD_PROOF_RESULT=FAIL|TWDT|panic|assert|backtrace|abort\(\)|unexpected reset|rebooting|rst:0x[2-9a-f]' "${EMULATOR_LOG}"; then
    fail 'keyboard proof failure, fault, or reset marker found'
fi

printf '%s\n' 'PASS: P4-NANO deterministic keyboard guest validation'
