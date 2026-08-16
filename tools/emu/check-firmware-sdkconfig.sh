#!/usr/bin/env bash

check_firmware_sdkconfig() {
    local sdkconfig_path="${1:?sdkconfig path is required}"

    if [[ ! -f "${sdkconfig_path}" ]]; then
        printf 'SDKCONFIG_PREFLIGHT absent path=%s action=use_defaults\n' "${sdkconfig_path}"
        return 0
    fi

    if grep -qx 'CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y' "${sdkconfig_path}"; then
        printf 'SDKCONFIG_PREFLIGHT valid path=%s flash=8MB\n' "${sdkconfig_path}"
        return 0
    fi

    printf '%s\n' \
        'ERROR: generated sdkconfig is stale or inconsistent with the Step 6A.1 flash envelope.' \
        'ERROR: tracked firmware/sdkconfig.defaults requires CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y.' \
        "ERROR: local generated configuration was not modified: ${sdkconfig_path}" \
        'ERROR: explicitly reconfigure, remove, or regenerate the local generated sdkconfig, then retry.' \
        >&2
    return 1
}
