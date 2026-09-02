#!/usr/bin/env bash
# Fresh configuration/build isolation matrix for the physical Audio 86 profiles.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_root=$(mktemp -d "${TMPDIR:-/tmp}/p4-audio86-physical-build-matrix.XXXXXX")
trap 'rm -rf "${build_root}"' EXIT
cd "${repo_root}"

build() {
    local name=$1
    shift
    tools/emu/build-production.sh "$@" --build-dir "${build_root}/${name}"
}

build default --variant p4-v3x --board generic --esp-emu-test
build async-inactive --variant p4-v3x --board generic \
    --audio86-async-inactive --esp-emu-test
build real-guest --variant p4-v3x --board generic \
    --audio86-real-guest --esp-emu-test
build virtual-pcm --variant p4-v3x --board generic \
    --audio86-real-guest-pcm-output --esp-emu-test
build retry --variant p4-v3x --board generic \
    --audio86-pcm-lifecycle retry-stop --esp-emu-test
build terminal --variant p4-v3x --board generic \
    --audio86-pcm-lifecycle finish-fatal --esp-emu-test
build physical-i2s --variant p4-v1x --board p4-nano \
    --audio86-real-guest-physical-i2s
build physical-i2s-short --variant p4-v1x --board p4-nano \
    --audio86-real-guest-physical-i2s-short

stages=(early post-i2s post-callback post-codec)
for index in "${!stages[@]}"; do
    build "lifecycle-$((index + 1))" --variant p4-v3x --board generic \
        --audio86-physical-lifecycle-test "${stages[index]}" --esp-emu-test
done

printf '5D2_S1A_BUILD_MATRIX=12/12_PASS\n'
