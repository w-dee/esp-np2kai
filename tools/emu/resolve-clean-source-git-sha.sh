#!/usr/bin/env bash
set -euo pipefail

if (($# != 1)); then
    printf 'usage: %s REPOSITORY_ROOT\n' "$0" >&2
    exit 2
fi

readonly repository_root=$1
if [[ "$(git -C "${repository_root}" rev-parse --is-inside-work-tree 2>/dev/null)" != "true" ]]; then
    printf 'ERROR: physical S1 source provenance requires a Git worktree\n' >&2
    exit 1
fi

readonly source_git_sha="$(git -C "${repository_root}" rev-parse --verify 'HEAD^{commit}')"
readonly git_listing="$(mktemp "${TMPDIR:-/tmp}/p4-audio86-source-provenance.XXXXXX")"
trap 'rm -f -- "${git_listing}"' EXIT

index_flags=()
if ! git -C "${repository_root}" ls-files -v -z >"${git_listing}"; then
    printf 'ERROR: physical S1 source provenance cannot enumerate tracked files\n' >&2
    exit 1
fi
while IFS= read -r -d '' entry; do
    marker=${entry:0:1}
    if [[ "${marker}" == "S" || "${marker}" =~ [a-z] ]]; then
        index_flags+=("${entry:2}")
    fi
done <"${git_listing}"
if ((${#index_flags[@]} != 0)); then
    printf 'ERROR: physical S1 source provenance has hidden index visibility flags; first=%q\n' \
        "${index_flags[0]}" >&2
    exit 1
fi

if ! git -C "${repository_root}" diff-files --quiet --ignore-submodules=none --; then
    printf 'ERROR: physical S1 source provenance has unstaged tracked changes\n' >&2
    exit 1
fi

if ! git -C "${repository_root}" diff-index --cached --quiet \
    --ignore-submodules=none "${source_git_sha}" --; then
    printf 'ERROR: physical S1 source provenance has staged tracked changes\n' >&2
    exit 1
fi

untracked=()
if ! git -C "${repository_root}" ls-files --others --exclude-standard -z \
    >"${git_listing}"; then
    printf 'ERROR: physical S1 source provenance cannot enumerate untracked files\n' >&2
    exit 1
fi
while IFS= read -r -d '' path; do
    untracked+=("${path}")
done <"${git_listing}"
if ((${#untracked[@]} != 0)); then
    printf 'ERROR: physical S1 source provenance has non-ignored untracked files; first=%q\n' \
        "${untracked[0]}" >&2
    exit 1
fi

printf '%s\n' "${source_git_sha}"
