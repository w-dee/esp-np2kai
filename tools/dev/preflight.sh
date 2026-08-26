#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"

usage() {
    cat <<'EOF'
usage: tools/dev/preflight.sh [--build-dir <path>] [--hardware]

Performs read-only repository and environment checks. --hardware reports only
the visibility of P4_NANO_SERIAL from this process: UNCONFIGURED, VISIBLE, or
NOT_VISIBLE_IN_CURRENT_CONTEXT. Nonvisibility is not treated as a hardware
failure because a normal sandbox may not expose serial devices.
EOF
}

build_dir=""
hardware=0
while (($# > 0)); do
    case "$1" in
        --build-dir)
            (($# >= 2)) || { usage >&2; exit 2; }
            build_dir="$2"
            shift 2
            ;;
        --build-dir=*)
            build_dir="${1#*=}"
            shift
            ;;
        --hardware)
            hardware=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'ERROR: unsupported argument: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

status=0
printf 'REPOSITORY_ROOT=%s\n' "${REPOSITORY_ROOT}"

if ! git -C "${REPOSITORY_ROOT}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    printf 'REPOSITORY=INVALID\n'
    exit 1
fi

printf 'BRANCH=%s\n' "$(git -C "${REPOSITORY_ROOT}" branch --show-current)"
printf 'HEAD=%s\n' "$(git -C "${REPOSITORY_ROOT}" rev-parse HEAD)"

if [[ -z "$(git -C "${REPOSITORY_ROOT}" status --porcelain=v1 --untracked-files=no)" ]]; then
    printf 'TRACKED_WORKTREE=CLEAN\n'
else
    printf 'TRACKED_WORKTREE=DIRTY\n'
    status=1
fi

hooks_path="$(git -C "${REPOSITORY_ROOT}" config --get core.hooksPath || true)"
if [[ "${hooks_path}" == ".githooks" ]]; then
    printf 'HOOKS_PATH=OK\n'
else
    printf 'HOOKS_PATH=EXPECTED_.githooks_ACTUAL=%s\n' "${hooks_path:-UNSET}"
    status=1
fi

if [[ -x "${REPOSITORY_ROOT}/.githooks/pre-commit" ]]; then
    printf 'PRE_COMMIT_HOOK=PRESENT\n'
else
    printf 'PRE_COMMIT_HOOK=MISSING\n'
    status=1
fi

if [[ -f "${REPOSITORY_ROOT}/tools/privacy_lint.py" ]]; then
    printf 'PRIVACY_LINT=PRESENT\n'
else
    printf 'PRIVACY_LINT=MISSING\n'
    status=1
fi

if [[ -f "${REPOSITORY_ROOT}/firmware/CMakeLists.txt" ]]; then
    printf 'FIRMWARE_PROJECT=PRESENT\n'
else
    printf 'FIRMWARE_PROJECT=MISSING\n'
    status=1
fi

idf_tool=""
if command -v idf.py >/dev/null 2>&1; then
    idf_tool="$(command -v idf.py)"
elif [[ -n "${IDF_PATH:-}" && -x "${IDF_PATH}/tools/idf.py" ]]; then
    idf_tool="${IDF_PATH}/tools/idf.py"
fi

if [[ -n "${idf_tool}" ]]; then
    printf 'IDF_PY=AVAILABLE\n'
    if ! "${idf_tool}" --version; then
        printf 'IDF_VERSION=UNAVAILABLE\n'
        status=1
    fi
else
    printf 'IDF_PY=UNAVAILABLE (activate ESP-IDF v5.5.4 to use IDF commands)\n'
fi

if [[ -n "${build_dir}" ]]; then
    if [[ ! -d "${build_dir}" ]]; then
        printf 'BUILD_DIR=MISSING path=%s\n' "${build_dir}"
        status=1
    else
        printf 'BUILD_DIR=PRESENT path=%s\n' "${build_dir}"
        for extension in elf bin map; do
            if compgen -G "${build_dir}/*.${extension}" >/dev/null; then
                printf 'ARTIFACT_%s=PRESENT\n' "${extension^^}"
            else
                printf 'ARTIFACT_%s=ABSENT\n' "${extension^^}"
            fi
        done
    fi
fi

if (( hardware )); then
    if [[ -z "${P4_NANO_SERIAL:-}" ]]; then
        printf 'SERIAL=UNCONFIGURED\n'
    elif [[ -e "${P4_NANO_SERIAL}" ]]; then
        printf 'SERIAL=VISIBLE\n'
    else
        printf 'SERIAL=NOT_VISIBLE_IN_CURRENT_CONTEXT\n'
        printf 'SERIAL_GUIDANCE=an elevated hardware action may still see this device\n'
    fi
fi

exit "${status}"
