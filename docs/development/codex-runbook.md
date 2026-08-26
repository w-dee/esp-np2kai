# Codex development runbook

## Scope

`AGENTS.md` defines stable repository invariants. This runbook explains the
development procedure, and `tools/dev/` provides executable guardrails. Read
the latter rather than duplicating command-line policy in a task prompt.

Physical P4-NANO work additionally requires
[`hardware-validation.md`](hardware-validation.md). It is a separate review
gate from implementation and static validation.

## Repository preflight

Before changing tracked files, establish the intended branch, local HEAD,
remote baseline when relevant, and tracked-worktree cleanliness. Confirm that
`core.hooksPath` is `.githooks`, that `.githooks/pre-commit` exists, and that
the firmware project exists at `firmware/CMakeLists.txt`.

Use the read-only helper when suitable:

```bash
tools/dev/preflight.sh
tools/dev/preflight.sh --build-dir <build-dir>
```

Run the repository privacy lint before a commit. A pre-commit hook is a
backstop, not a replacement for reviewing the changed paths.

## Sandbox and elevation

Read-only Git commands normally need no special handling. Commands that write
Git metadata can be denied in a normal sandbox; examples include `fetch` or
`pull` when refs or `FETCH_HEAD` must be updated, `commit`, and `git config`.

When that happens, do not blindly retry the identical permission-denied command.
Use elevated execution only when the requested operation requires it. Do not
claim that every Git command requires elevation.

Changing the required hook setting writes `.git/config`:

```bash
git config core.hooksPath .githooks
```

Verify that setting before commits; changing it is a deliberate local action.

## ESP-IDF environment and project directory

The supported project baseline is ESP-IDF v5.5.4. Activate a normal ESP-IDF
environment before calling IDF tools. `IDF_PATH` and activation details are
local-environment concerns and must not be hardcoded into tracked files.

The repository root is not an ESP-IDF project. `firmware/` is the project
directory. Prefer the wrapper, which supplies the IDF project directory while
preserving every argument:

```bash
tools/dev/idf.sh -B <build-dir> build
```

ESP-IDF v5.5.4 also supports `idf.py -C firmware`, but the wrapper avoids
repeated cwd mistakes.

## Build directory identity

For a hardware run, the flashed image, ELF selected by IDF Monitor, and stated
build directory must come from the same fresh build. Record the build directory
and profile in the result. Do not monitor an image using symbols from another
build tree.

## Review gates

Keep these stages separate:

1. READ-ONLY audit — stop for human review.
2. Implementation plus host/static/build validation — stop for human review.
3. Real-hardware measurement — stop for human review.

Do not silently carry a task across a gate. The hardware procedure and its
elevation requirements are in the hardware-validation runbook.

## Local commit and push

After successful tracked implementation changes and successful relevant
validation, create one local review-ready commit only on a non-main branch.
Never automatically commit failed validation, unintended/generated tracked
files, or work on `main`. Push only when a human explicitly requests it.

## Machine-local Codex configuration

`~/.codex/AGENTS.md` is the appropriate place for workstation-specific
elevation behavior, the exact P4-NANO serial by-id value, and local ESP-IDF
activation details. Keep those literal values out of this repository.
