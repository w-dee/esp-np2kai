#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    cat <<'EOF'
usage: tools/dev/p4-nano-monitor.sh --build-dir <path>

Starts ESP-IDF Monitor for the P4-NANO onboard CH343 at 1500000 baud.
P4_NANO_SERIAL must name the machine-local serial device. The wrapper does not
reset, flash, rebuild, inspect the serial device, or automate monitor keys.
Flash-triggered boot is setup only. Let setup output finish, then enable
logging and Ctrl+T Ctrl+R once. Only a Returned from app_main() after the
fresh canonical boot ends capture.
EOF
}

build_dir=""
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

[[ -n "${build_dir}" ]] || { printf 'ERROR: --build-dir is required\n' >&2; usage >&2; exit 2; }
[[ -d "${build_dir}" ]] || { printf 'ERROR: build directory does not exist: %s\n' "${build_dir}" >&2; exit 2; }
[[ -n "${P4_NANO_SERIAL:-}" ]] || { printf 'ERROR: P4_NANO_SERIAL is required\n' >&2; exit 2; }

printf 'Monitor ready: flash boot is setup only. Let setup output finish, then enable logging and reset once; only Returned from app_main() after the fresh canonical boot ends capture.\n'
"${SCRIPT_DIR}/idf.sh" -B "${build_dir}" -p "${P4_NANO_SERIAL}" -b 1500000 monitor --no-reset
