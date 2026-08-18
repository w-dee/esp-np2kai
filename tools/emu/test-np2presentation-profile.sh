#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly FIRMWARE_DIR="${REPOSITORY_ROOT}/firmware"
readonly RUN_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/np2presentation-profile.XXXXXX")"

source "${SCRIPT_DIR}/activate-idf.sh"
cd -- "${FIRMWARE_DIR}"

set +e
conflict_output="$(idf.py -B "${RUN_ROOT}/video-build" \
    -D "SDKCONFIG=${RUN_ROOT}/video-build/sdkconfig" \
    -D "NP2_PRESENTATION_PROFILE=1" \
    -D "NP2_VIDEO_PROFILE=1" set-target esp32p4 2>&1)"
conflict_status=$?
set -e
printf '%s\n' "${conflict_output}"
if (( conflict_status == 0 )) ||
    [[ "${conflict_output}" != *"NP2_PRESENTATION_PROFILE and NP2_VIDEO_PROFILE are mutually exclusive"* ]]; then
    printf 'NP2PRESENT_PROFILE_SELFTEST=FAIL reason=video_conflict\n'
    exit 1
fi

set +e
storage_output="$(idf.py -B "${RUN_ROOT}/storage-build" \
    -D "SDKCONFIG=${RUN_ROOT}/storage-build/sdkconfig" \
    -D "NP2_PRESENTATION_PROFILE=1" \
    -D "STORAGE_FATFS_PROBE=1" set-target esp32p4 2>&1)"
storage_status=$?
set -e
printf '%s\n' "${storage_output}"
if (( storage_status == 0 )) ||
    [[ "${storage_output}" != *"video/presentation profiles are mutually exclusive with probe/storage"* ]]; then
    printf 'NP2PRESENT_PROFILE_SELFTEST=FAIL reason=storage_conflict\n'
    exit 1
fi

printf 'NP2PRESENT_PROFILE_SELFTEST=PASS\n'
