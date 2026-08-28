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

## Machine-verifiable hash and digest comparison

Acceptance-critical hashes, digests, checksums, and long hexadecimal
identities must never be compared by visual inspection, manual character
counting, or an LLM deciding that displayed values look equal. Equality must
be produced by a deterministic local program or tool.

Prefer comparing original machine-produced values without retyping them. For
artifact identity, recompute from the artifact where possible (for example,
`sha256sum file`) and compare files or recorded values with `cmp`, shell
equality, Python, or another deterministic local program. For values extracted
from text, validate the expected length and hexadecimal syntax before
comparing; a SHA-256 digest is 64 hexadecimal characters (32 bytes).

For example, this dependency-free pattern validates and compares two supplied
hexadecimal values while reporting a useful mismatch index:

```python
actual_hex = input("actual: ").strip()
expected_hex = input("expected: ").strip()

try:
    actual = bytes.fromhex(actual_hex)
    expected = bytes.fromhex(expected_hex)
except ValueError:
    print("DIGEST_COMPARE=FAIL invalid_hex")
else:
    if len(actual) != len(expected):
        print(f"DIGEST_COMPARE=FAIL length {len(actual)} != {len(expected)}")
    elif actual == expected:
        print("DIGEST_COMPARE=PASS")
    else:
        first = next(i for i, (a, b) in enumerate(zip(actual, expected)) if a != b)
        print(f"DIGEST_COMPARE=FAIL first_byte={first}")
```

When useful, report lengths, the first differing character or byte index, and
the differing values mechanically. If a report includes both source strings
and a PASS/FAIL result, only the mechanically generated result is authoritative;
the model may explain or summarize it but must not replace the comparison.

This procedure applies especially to candidate freezing, exact-flash
provenance, physical app verification, golden identity checks, benchmark event
and PCM hashes, and captured-log digest comparisons. CRC32 and other short
values also require mechanical comparison when they participate in a formal
acceptance or regression gate; casual prose discussion of a number does not.

Artifact integrity (recomputing a digest from bytes) and comparison of already
emitted digest values are distinct operations, and both are mechanical
evidence.

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

An ESP-IDF build directory is bound to the Python interpreter recorded in its
`CMakeCache.txt:PYTHON`. Use the repository-supported activation in the
calling shell:

```bash
source tools/emu/activate-idf.sh
```

Do not activate a different upstream `export.sh` environment before using
`idf.py` with an already-configured build directory. ESP-IDF rejects that
mismatch before the target action; in particular, a failed flash in this state
means **FLASH DID NOT OCCUR** unless independent esptool evidence says
otherwise. Keep build and flash in one consistent IDF/Python environment and
use the same environment for post-flash image verification.

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

After an explicit flash, run the independent application identity gate before
starting monitor logging or the canonical reset:

```bash
tools/dev/p4-nano-verify-app.sh --build-dir <build-dir>
```

It derives the app path and offset from generated metadata and verifies only
that app region. A passing identity gate is a setup precondition, not a
benchmark result.

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

## P10G-B lower-refresh visual profile

The diagnostic-only P10G-B visual profiles are built independently with one
of these commands:

```bash
bash tools/emu/build-production.sh \
  --variant p4-v1x \
  --board p4-nano \
  --display-refresh-visual baseline
bash tools/emu/build-production.sh \
  --variant p4-v1x \
  --board p4-nano \
  --display-refresh-visual lower1
bash tools/emu/build-production.sh \
  --variant p4-v1x \
  --board p4-nano \
  --display-refresh-visual lower2
```

The selected profile is encoded in the build directory and in the image's
configuration log. These profiles are mutually exclusive with the normal
runtime and other display diagnostics; they do not change the production
configuration.

## P10H-B paired P10F display-refresh profiles

The P10H-B selector prepares separate P10F benchmark images from one source
revision. It is diagnostic-only, does not retune DSI at runtime, and does not
run the P10G visual runner. The standard runtime configuration is unaffected.

CONTROL:

```bash
bash tools/emu/build-production.sh \
  --variant p4-v1x \
  --board p4-nano \
  --exact2x-internal-source-benchmark \
  --benchmark-display-refresh baseline \
  --transform-opt o2
```

LOWER2:

```bash
bash tools/emu/build-production.sh \
  --variant p4-v1x \
  --board p4-nano \
  --exact2x-internal-source-benchmark \
  --benchmark-display-refresh lower2 \
  --transform-opt o2
```

Use separate fresh build directories for the paired images. P10H performance
runs retain both PSRAM-source and INTERNAL-source phases; no human visual
verdict is part of this benchmark. The pair changes both DPI refresh and DSI
lane bitrate, so results support the LOWER2 DSI/display-load configuration,
not an isolated physical PSRAM scanout-bandwidth attribution.
