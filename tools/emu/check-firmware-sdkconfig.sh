#!/usr/bin/env bash

check_firmware_sdkconfig() {
    local sdkconfig_path="${1:?sdkconfig path is required}"
    local variant="${2:-}"
    local board="${3:-generic}"
    local expected_baud_line=""
    local -a expected_board_lines=()

    if [[ ! -f "${sdkconfig_path}" ]]; then
        printf 'SDKCONFIG_PREFLIGHT absent path=%s variant=%s board=%s action=use_defaults\n' \
            "${sdkconfig_path}" "${variant:--}" "${board}"
        return 0
    fi

    if ! grep -qx 'CONFIG_IDF_TARGET="esp32p4"' "${sdkconfig_path}"; then
        printf 'ERROR: generated sdkconfig is not configured for ESP32-P4: %s\n' \
            "${sdkconfig_path}" >&2
        return 1
    fi

    if ! grep -qx 'CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y' "${sdkconfig_path}"; then
        printf '%s\n' \
            'ERROR: generated sdkconfig is stale or inconsistent with the 8 MiB production flash envelope.' \
            'ERROR: production defaults require CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y.' \
            "ERROR: local generated configuration was not modified: ${sdkconfig_path}" \
            'ERROR: explicitly reconfigure, remove, or regenerate the local generated sdkconfig, then retry.' \
            >&2
        return 1
    fi

    case "${variant}" in
        '')
            printf 'SDKCONFIG_PREFLIGHT valid path=%s flash=8MB variant=unspecified board=%s\n' \
                "${sdkconfig_path}" "${board}"
            return 0
            ;;
        p4-v1x)
            expected_lines=(
                'CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y'
                'CONFIG_ESP32P4_REV_MIN_100=y'
                'CONFIG_ESP32P4_REV_MIN_FULL=100'
                'CONFIG_ESP_REV_MIN_FULL=100'
                'CONFIG_ESP_REV_MAX_FULL=199'
            )
            ;;
        p4-v3x)
            expected_lines=(
                '# CONFIG_ESP32P4_SELECTS_REV_LESS_V3 is not set'
                'CONFIG_ESP32P4_REV_MIN_301=y'
                'CONFIG_ESP32P4_REV_MIN_FULL=301'
                'CONFIG_ESP_REV_MIN_FULL=301'
                'CONFIG_ESP_REV_MAX_FULL=399'
            )
            ;;
        *)
            printf 'ERROR: unknown production firmware variant: %s\n' "${variant}" >&2
            return 1
            ;;
    esac

    case "${board}" in
        generic)
            expected_baud_line='CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200'
            ;;
        p4-nano)
            if [[ "${variant}" != "p4-v1x" ]]; then
                printf 'ERROR: board %s requires production variant p4-v1x, got %s\n' \
                    "${board}" "${variant}" >&2
                return 1
            fi
            expected_baud_line='CONFIG_ESP_CONSOLE_UART_BAUDRATE=1500000'
            expected_board_lines=('CONFIG_ESP_CONSOLE_UART_CUSTOM=y')
            ;;
        *)
            printf 'ERROR: unknown production firmware board: %s\n' "${board}" >&2
            return 1
            ;;
    esac

    if ! grep -qxF "${expected_baud_line}" "${sdkconfig_path}"; then
        printf 'ERROR: sdkconfig does not match production board %s: missing %s\n' \
            "${board}" "${expected_baud_line}" >&2
        return 1
    fi

    local expected_board_line
    for expected_board_line in "${expected_board_lines[@]:-}"; do
        if [[ -n "${expected_board_line}" ]] && ! grep -qxF "${expected_board_line}" "${sdkconfig_path}"; then
            printf 'ERROR: sdkconfig does not match production board %s: missing %s\n' \
                "${board}" "${expected_board_line}" >&2
            return 1
        fi
    done

    local expected_line
    for expected_line in "${expected_lines[@]}"; do
        if ! grep -qxF "${expected_line}" "${sdkconfig_path}"; then
            printf 'ERROR: sdkconfig does not match production variant %s: missing %s\n' \
                "${variant}" "${expected_line}" >&2
            return 1
        fi
    done

    for expected_line in \
        'CONFIG_SPIRAM=y' \
        'CONFIG_SPIRAM_USE_MALLOC=y' \
        'CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y'; do
        if ! grep -qxF "${expected_line}" "${sdkconfig_path}"; then
            printf 'ERROR: production sdkconfig is missing required PSRAM setting: %s\n' \
                "${expected_line}" >&2
            return 1
        fi
    done

    printf 'SDKCONFIG_PREFLIGHT valid path=%s flash=8MB variant=%s board=%s baud=%s\n' \
        "${sdkconfig_path}" "${variant}" "${board}" "${expected_baud_line#*=}"
    return 0
}
