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
EOF
check_firmware_sdkconfig "${TEST_ROOT}/valid-sdkconfig"

cat > "${TEST_ROOT}/stale-sdkconfig" <<'EOF'
CONFIG_IDF_TARGET="esp32p4"
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
EOF
if check_firmware_sdkconfig "${TEST_ROOT}/stale-sdkconfig"; then
    printf '%s\n' 'ERROR: stale sdkconfig unexpectedly passed preflight' >&2
    exit 1
fi
grep -qx 'CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y' "${TEST_ROOT}/stale-sdkconfig"

printf '%s\n' 'PASS: firmware sdkconfig preflight cases'
