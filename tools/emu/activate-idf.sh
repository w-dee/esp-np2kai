#!/usr/bin/env bash

# Source this helper from emulator scripts. An already-active ESP-IDF
# environment wins; local development falls back to the pinned activation
# script used by the existing setup.
if [[ -n "${IDF_PATH:-}" ]] &&
   [[ -f "${IDF_PATH}/export.sh" ]] &&
   command -v idf.py >/dev/null 2>&1; then
    return 0
fi

np2_idf_activation_script="${NP2_IDF_ACTIVATION_SCRIPT:-${HOME}/.espressif/tools/activate_idf_v5.5.4.sh}"
if [[ ! -f "${np2_idf_activation_script}" ]]; then
    printf 'ERROR: ESP-IDF activation script not found: %s\n' \
        "${np2_idf_activation_script}" >&2
    return 1
fi

np2_activate_idf() {
    local original_argv0="${BASH_ARGV0}"
    local activation_status

    eim() { :; }
    BASH_ARGV0=bash
    set +eu
    # shellcheck disable=SC1090
    source "${np2_idf_activation_script}"
    activation_status="$?"
    set -eu
    BASH_ARGV0="${original_argv0}"
    unset -f eim
    return "${activation_status}"
}

np2_activate_idf
unset -f np2_activate_idf
unset np2_idf_activation_script
