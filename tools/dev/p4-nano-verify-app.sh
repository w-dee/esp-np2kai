#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly VERIFY_BAUD=1500000

usage() {
    cat <<'EOF'
usage: tools/dev/p4-nano-verify-app.sh --build-dir <path> [--print-plan]

Verifies the application payload already present in P4-NANO flash. The
application path and offset come from the exact build directory's generated
flasher_args.json; no build, flash, erase, monitor, or benchmark reset is
performed. P4_NANO_SERIAL is required for physical verification. --print-plan
parses and reports the image identity and esptool command without touching
serial hardware or requiring P4_NANO_SERIAL.

Before a physical invocation, activate the repository-supported ESP-IDF
environment in the calling shell, for example:

  source tools/emu/activate-idf.sh
EOF
}

error() {
    printf 'ERROR: %s\n' "$1" >&2
}

build_dir=""
print_plan=0
while (($# > 0)); do
    case "$1" in
        --build-dir)
            (($# >= 2)) || { error '--build-dir requires a path'; usage >&2; exit 2; }
            build_dir="$2"
            shift 2
            ;;
        --build-dir=*)
            build_dir="${1#*=}"
            shift
            ;;
        --print-plan|--dry-run)
            print_plan=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            error "unsupported argument: $1"
            usage >&2
            exit 2
            ;;
    esac
done

if [[ -z "${build_dir}" ]]; then
    error '--build-dir is required'
    usage >&2
    exit 2
fi
if [[ ! -d "${build_dir}" ]]; then
    error "build directory does not exist: ${build_dir}"
    exit 2
fi

build_dir="$(cd -- "${build_dir}" && pwd -P)"
metadata_path="${build_dir}/flasher_args.json"
if [[ ! -f "${metadata_path}" ]]; then
    error "generated metadata is missing: ${metadata_path}"
    exit 2
fi

# Keep metadata interpretation in one small shared parser used by the exact
# writer as well. Physical verification below uses the activated IDF Python.
parser_python=""
if command -v python3 >/dev/null 2>&1; then
    parser_python="$(command -v python3)"
elif command -v python >/dev/null 2>&1; then
    parser_python="$(command -v python)"
else
    error 'python3/python is required to parse flasher_args.json'
    exit 2
fi

metadata_values="$(${parser_python} "${SCRIPT_DIR}/p4_nano_flash_metadata.py" \
    --build-dir "${build_dir}" --app-identity)" || {
    error "unable to resolve application identity from ${metadata_path}"
    exit 2
}

IFS=$'\t' read -r app_path app_offset app_bytes app_sha256 esptool_chip <<< "${metadata_values}"
if [[ -z "${app_path}" || -z "${app_offset}" || -z "${app_bytes}" || -z "${app_sha256}" || -z "${esptool_chip}" ]]; then
    error 'metadata parser returned an incomplete application identity'
    exit 2
fi
if ! [[ "${app_offset}" =~ ^[0-9]+$ ]]; then
    error 'metadata parser returned a non-decimal application offset'
    exit 2
fi

identity_result='PENDING'
if (( print_plan )); then
    identity_result='PLAN'
fi
printf 'P4_NANO_FLASH_APP_IDENTITY build_dir=%q app=%q offset=0x%x bytes=%s sha256=%s result=%s\n' \
    "${build_dir}" "${app_path}" "${app_offset}" "${app_bytes}" "${app_sha256}" "${identity_result}"

if (( print_plan )); then
    printf 'P4_NANO_FLASH_VERIFY_PLAN command='
    printf '%q ' python -m esptool --chip "${esptool_chip}" -p '<P4_NANO_SERIAL>' -b "${VERIFY_BAUD}" \
        --before default_reset --after hard_reset verify_flash "$(printf '0x%x' "${app_offset}")" "${app_path}"
    printf '\nhardware_touched=NO\n'
    exit 0
fi

if [[ -z "${P4_NANO_SERIAL:-}" ]]; then
    error 'P4_NANO_SERIAL is required for physical verification'
    error 'activate ESP-IDF first with: source tools/emu/activate-idf.sh'
    exit 2
fi
if [[ -z "${IDF_PATH:-}" || ! -d "${IDF_PATH}" || ! -f "${IDF_PATH}/tools/idf.py" ]]; then
    error 'IDF_PATH is not an active ESP-IDF installation; source tools/emu/activate-idf.sh'
    exit 2
fi
if [[ -z "${IDF_PYTHON_ENV_PATH:-}" || ! -x "${IDF_PYTHON_ENV_PATH}/bin/python" ]]; then
    error 'IDF_PYTHON_ENV_PATH is not an active ESP-IDF Python environment; source tools/emu/activate-idf.sh'
    exit 2
fi
if ! command -v python >/dev/null 2>&1; then
    error 'active python is unavailable; source tools/emu/activate-idf.sh'
    exit 2
fi

active_python="$(python -c 'import os, sys; print(os.path.realpath(sys.executable))')" || {
    error 'unable to inspect the active Python interpreter; source tools/emu/activate-idf.sh'
    exit 2
}
expected_python="$(python -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "${IDF_PYTHON_ENV_PATH}/bin/python")" || {
    error 'unable to inspect IDF_PYTHON_ENV_PATH; source tools/emu/activate-idf.sh'
    exit 2
}
if [[ "${active_python}" != "${expected_python}" ]]; then
    error "active Python (${active_python}) does not match IDF_PYTHON_ENV_PATH (${expected_python})"
    error 'source tools/emu/activate-idf.sh in a fresh shell before verification'
    exit 2
fi
if ! python -c 'import esptool' >/dev/null 2>&1; then
    error 'esptool is unavailable in the active ESP-IDF Python; source tools/emu/activate-idf.sh'
    exit 2
fi

cache_path="${build_dir}/CMakeCache.txt"
if [[ -f "${cache_path}" ]]; then
    configured_python="$(sed -n 's/^PYTHON:[^=]*=//p' "${cache_path}")"
    if [[ -n "${configured_python}" ]]; then
        configured_python="$(python -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "${configured_python}")" || {
            error 'unable to inspect CMakeCache.txt PYTHON; source tools/emu/activate-idf.sh'
            exit 2
        }
        if [[ "${configured_python}" != "${active_python}" ]]; then
            error "build directory PYTHON (${configured_python}) does not match active Python (${active_python})"
            error 'use source tools/emu/activate-idf.sh and the same environment used to build this directory'
            exit 2
        fi
    fi
fi

printf 'P4_NANO_FLASH_VERIFY command='
printf '%q ' python -m esptool --chip "${esptool_chip}" -p "${P4_NANO_SERIAL}" -b "${VERIFY_BAUD}" \
    --before default_reset --after hard_reset verify_flash "$(printf '0x%x' "${app_offset}")" "${app_path}"
printf '\n'

set +e
python -m esptool --chip "${esptool_chip}" -p "${P4_NANO_SERIAL}" -b "${VERIFY_BAUD}" \
    --before default_reset --after hard_reset verify_flash "$(printf '0x%x' "${app_offset}")" "${app_path}"
verify_status=$?
set -e
if (( verify_status != 0 )); then
    printf 'P4_NANO_FLASH_APP_IDENTITY build_dir=%q app=%q offset=0x%x bytes=%s sha256=%s result=FAIL\n' \
        "${build_dir}" "${app_path}" "${app_offset}" "${app_bytes}" "${app_sha256}" >&2
    printf 'FLASH IMAGE IDENTITY = FAIL\n' >&2
    exit "${verify_status}"
fi

printf 'P4_NANO_FLASH_APP_IDENTITY build_dir=%q app=%q offset=0x%x bytes=%s sha256=%s result=PASS\n' \
    "${build_dir}" "${app_path}" "${app_offset}" "${app_bytes}" "${app_sha256}"
printf 'FLASH IMAGE IDENTITY = PASS\n'
