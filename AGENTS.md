# Project instructions

## Operational documentation

- This file contains stable repository invariants. Detailed procedures are in
  `docs/development/codex-runbook.md` and
  `docs/development/hardware-validation.md`.
- Read the hardware-validation runbook before physical P4-NANO work.

## Machine-verifiable identity evidence

- Never determine equality of acceptance-critical hashes, digests, checksums,
  or long hexadecimal identities by visual inspection, manual character
  comparison, or LLM comparison.
- When equality matters, use a program or tool to compare the values. This
  applies to SHA-256 and other SHA-family digests, CRC values used as formal
  gates, firmware/image and build-provenance hashes, benchmark goldens, and
  log-emitted digests.
- The model may interpret mechanically produced results, but it must not be
  the authority deciding digest equality. Formal PASS/FAIL claims that depend
  on equality must come from a mechanical comparison result.

## Privacy

- Never place developer-specific absolute home paths in tracked files.
- Use repository-relative paths, `$HOME`, `${HOME}`, `~/`, or documented
  placeholders.
- Run the repository privacy lint before committing.

## Git / sandbox

- Operations that write `.git` metadata may require elevated execution.
- After a permission denial, do not repeatedly retry the unchanged operation
  in the normal sandbox; elevate only when the operation requires it.
- The required hook path is `.githooks`; verify `core.hooksPath` before
  committing. Changing it writes `.git/config` and may require elevation.

## ESP-IDF project root

- `firmware/` is the ESP-IDF project directory. Do not run `idf.py` from the
  repository root as though it were the project root.
- Prefer `tools/dev/idf.sh` where applicable.

## Physical serial

- Serial devices may be invisible from the normal sandbox. Do not infer a
  physical disconnection from sandbox invisibility.
- Use elevated execution when physical access is necessary.
- Machine-specific serial identifiers belong in local environment/configuration,
  not tracked instructions.

## esptool retry policy

- If the SoC gives no response, retry the same operation up to 3 total
  attempts.
- Do not alter baud, wiring, erase, image, or procedure after only the first
  failure.

## Measurement gates

1. Perform a READ-ONLY audit, then stop for human review.
2. Perform implementation plus host/static/build validation, then stop for
   human review.
3. Perform real-hardware measurement, then stop for human review.

- Do not cross a gate unless a human explicitly requests the next stage.

## Commit / push

- If source or tracked implementation changes succeed, validation succeeds,
  the branch is non-main, and the result is review-ready, create one
  appropriate local commit.
- Do not commit failed validation, unintended/generated tracked files, or
  automatically on `main`.
- Push only on explicit human request.

## Validation

- Run relevant host/static/build validation for the changed scope.
- Real hardware remains a separate gate.
