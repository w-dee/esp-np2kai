#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/check-firmware-sdkconfig.sh"

readonly TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/step6a1-sdkconfig-test.XXXXXX")"
trap 'rm -rf -- "${TEST_ROOT}"' EXIT

check_firmware_sdkconfig "${TEST_ROOT}/missing-sdkconfig"

cat > "${TEST_ROOT}/valid-sdkconfig" <<'EOF'
CONFIG_IDF_TARGET="esp32p4"
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_100=y
CONFIG_ESP32P4_REV_MIN_FULL=100
CONFIG_ESP_REV_MIN_FULL=100
CONFIG_ESP_REV_MAX_FULL=199
CONFIG_SPIRAM=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
EOF
check_firmware_sdkconfig "${TEST_ROOT}/valid-sdkconfig" p4-v1x

cat > "${TEST_ROOT}/stale-sdkconfig" <<'EOF'
CONFIG_IDF_TARGET="esp32p4"
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
EOF
if check_firmware_sdkconfig "${TEST_ROOT}/stale-sdkconfig"; then
    printf '%s\n' 'ERROR: stale sdkconfig unexpectedly passed preflight' >&2
    exit 1
fi
grep -qx 'CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y' "${TEST_ROOT}/stale-sdkconfig"

cat > "${TEST_ROOT}/valid-v3-sdkconfig" <<'EOF'
CONFIG_IDF_TARGET="esp32p4"
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200
# CONFIG_ESP32P4_SELECTS_REV_LESS_V3 is not set
CONFIG_ESP32P4_REV_MIN_301=y
CONFIG_ESP32P4_REV_MIN_FULL=301
CONFIG_ESP_REV_MIN_FULL=301
CONFIG_ESP_REV_MAX_FULL=399
CONFIG_SPIRAM=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
EOF
check_firmware_sdkconfig "${TEST_ROOT}/valid-v3-sdkconfig" p4-v3x

if check_firmware_sdkconfig "${TEST_ROOT}/valid-v3-sdkconfig" p4-v1x; then
    printf '%s\n' 'ERROR: revision-mismatched sdkconfig unexpectedly passed preflight' >&2
    exit 1
fi

if check_firmware_sdkconfig "${TEST_ROOT}/valid-sdkconfig" p4-v1x p4-nano; then
    printf '%s\n' 'ERROR: generic 115200 sdkconfig unexpectedly passed as p4-nano' >&2
    exit 1
fi

cat > "${TEST_ROOT}/valid-p4-nano-sdkconfig" <<'EOF'
CONFIG_IDF_TARGET="esp32p4"
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
CONFIG_ESP_CONSOLE_UART_CUSTOM=y
CONFIG_ESP_CONSOLE_UART_BAUDRATE=1500000
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_100=y
CONFIG_ESP32P4_REV_MIN_FULL=100
CONFIG_ESP_REV_MIN_FULL=100
CONFIG_ESP_REV_MAX_FULL=199
CONFIG_SPIRAM=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
EOF
check_firmware_sdkconfig "${TEST_ROOT}/valid-p4-nano-sdkconfig" p4-v1x p4-nano

if check_firmware_sdkconfig "${TEST_ROOT}/valid-v3-sdkconfig" p4-v3x p4-nano; then
    printf '%s\n' 'ERROR: p4-nano unexpectedly passed with p4-v3x' >&2
    exit 1
fi

printf '%s\n' 'PASS: firmware sdkconfig preflight cases'
