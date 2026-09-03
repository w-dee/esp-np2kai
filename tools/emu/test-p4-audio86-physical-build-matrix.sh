#!/usr/bin/env bash
# Fresh configuration/build isolation matrix for the physical Audio 86 profiles.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_root=${P4_AUDIO86_MATRIX_OUTPUT_DIR:-$(
    mktemp -d "${TMPDIR:-/tmp}/p4-audio86-physical-build-matrix.XXXXXX"
)}
mkdir -p "${build_root}"
cd "${repo_root}"

source_sha=$(tools/emu/resolve-clean-source-git-sha.sh "${repo_root}")
printf 'source_git_sha=%s\nparallelism=3\n' "${source_sha}" \
    >"${build_root}/provenance.txt"

build() {
    local name=$1
    shift
    tools/emu/build-production.sh "$@" --build-dir "${build_root}/${name}"
}

declare -a matrix_pids=()
declare -a matrix_names=()

wait_batch() {
    local failed=0
    local index
    for index in "${!matrix_pids[@]}"; do
        if wait "${matrix_pids[index]}"; then
            printf 'F3_MATRIX_PROFILE=%s PASS\n' "${matrix_names[index]}"
        else
            printf 'F3_MATRIX_PROFILE=%s FAIL log=%s\n' \
                "${matrix_names[index]}" \
                "${build_root}/${matrix_names[index]}.log" >&2
            tail -n 80 "${build_root}/${matrix_names[index]}.log" >&2
            failed=1
        fi
    done
    matrix_pids=()
    matrix_names=()
    return "${failed}"
}

launch() {
    local name=$1
    shift
    build "${name}" "$@" >"${build_root}/${name}.log" 2>&1 &
    matrix_pids+=("$!")
    matrix_names+=("${name}")
    if ((${#matrix_pids[@]} == 3)); then
        wait_batch
    fi
}

launch real-guest --variant p4-v3x --board generic \
    --audio86-real-guest --esp-emu-test
launch virtual-pcm --variant p4-v3x --board generic \
    --audio86-real-guest-pcm-output --esp-emu-test
launch sustained-2s --variant p4-v3x --board generic \
    --audio86-real-guest-sustained-2s --esp-emu-test
launch retry --variant p4-v3x --board generic \
    --audio86-pcm-lifecycle retry-stop --esp-emu-test
launch terminal --variant p4-v3x --board generic \
    --audio86-pcm-lifecycle finish-fatal --esp-emu-test
launch physical-i2s --variant p4-v1x --board p4-nano \
    --audio86-real-guest-physical-i2s
launch physical-i2s-short --variant p4-v1x --board p4-nano \
    --audio86-real-guest-physical-i2s-short
launch sustained-2s-physical-i2s --variant p4-v1x --board p4-nano \
    --audio86-real-guest-sustained-2s-physical-i2s

stages=(early post-i2s post-callback post-codec)
for index in "${!stages[@]}"; do
    launch "lifecycle-$((index + 1))" --variant p4-v3x --board generic \
        --audio86-physical-lifecycle-test "${stages[index]}" --esp-emu-test
done
if ((${#matrix_pids[@]} > 0)); then
    wait_batch
fi

printf 'F3_MATRIX_SOURCE_GIT_SHA=%s\n' "${source_sha}"
printf 'F3_MATRIX_EVIDENCE_DIR=%s\n' "${build_root}"
printf 'F3_FINAL_BUILD_MATRIX=12/12_PASS\n'
