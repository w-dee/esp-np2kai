# P4-NANO hardware validation

## Scope and preconditions

This is the canonical production/benchmark procedure for the P4-NANO. It does
not replace retained bring-up diagnostics. Begin only after a human explicitly
approves the hardware gate.

Before touching hardware, record the tracked-worktree state, exact build
directory, build profile, flashed image identity, and ELF identity. Configure
the LCD and any experiment-specific peripherals as required by that experiment.

## Serial configuration

Set the machine-local serial value before the hardware command:

```bash
export P4_NANO_SERIAL=/dev/serial/by-id/<p4-nano-ch343>
```

The current production/P10 console procedure uses the onboard CH343 on UART0
at 1,500,000 baud. A normal sandbox may not see the device; that is not proof
that it is unplugged. Use elevated hardware execution when physical access is
needed.

## Flash

Use the approved build/flash command, or use the IDF wrapper with the matching
build directory:

```bash
tools/dev/idf.sh -B <build-dir> -p "$P4_NANO_SERIAL" flash
```

The build and flash for one build directory must use the same ESP-IDF/Python
environment. For this repository, activate it in the calling shell with
`source tools/emu/activate-idf.sh` and do not source a different upstream
`export.sh` before flashing an already-configured build. ESP-IDF checks the
build's `CMakeCache.txt:PYTHON`; a mismatch aborts before esptool, so the
correct status is **FLASH DID NOT OCCUR** unless separate evidence proves that
esptool completed successfully.

If the SoC does not respond, repeat the same failed esptool operation no more
than three times in total. Do not change baud, wiring, erase policy, image, or
procedure after the first failure.

## Post-flash application identity

Do not begin monitor logging, the canonical reset, or a formal measurement
until the exact application image written to flash has passed an independent
identity gate. The required sequence is:

```text
fresh build -> record host app identity -> flash exact build
-> verify physical app region -> FLASH IMAGE IDENTITY = PASS
-> drain setup output -> monitor --no-reset -> logging on
-> Ctrl+T Ctrl+R once -> canonical epoch
```

Before flashing, record the exact build directory, the application path and
flash offset from its generated `flasher_args.json`, the app byte count, and
the host app SHA-256. Then, in the same activated environment, run:

```bash
tools/dev/p4-nano-verify-app.sh --build-dir <build-dir>
```

The helper selects only the explicit `app` metadata entry, verifies its
generated offset and payload with esptool's `verify_flash`, and prints a
parseable `P4_NANO_FLASH_APP_IDENTITY ... result=PASS` line. Its SHA-256 is
the host-image identity; esptool's physical comparison is its supported MD5
flash comparison, not a device-side SHA-256 claim. A missing, malformed, or
ambiguous metadata entry fails closed. The helper does not build, flash,
erase, monitor, or select another build directory. `--print-plan` is a local
metadata-only check and never touches serial hardware.

The helper uses `python -m esptool` from the currently activated IDF Python,
at 1,500,000 baud, with the generated app offset and app file. It checks
`IDF_PATH`, `IDF_PYTHON_ENV_PATH`, the active interpreter, esptool import, and
the build cache's configured `PYTHON` before opening the serial port. If
verification reports a mismatch, print `FLASH IMAGE IDENTITY = FAIL` and stop
as a setup failure; do not enable formal logging, issue the canonical reset,
or call the benchmark. A target-no-response transport failure permits the
same verify operation to be retried, at most three total attempts. An actual
flash-content mismatch is not a transport retry case.

The verification command uses `--before default-reset --after hard-reset`.
Any reset and application boot caused by verification is **SETUP /
POST-VERIFICATION BOOT**, not the canonical measurement epoch. After a passing
gate, attach the monitor with `--no-reset`, drain setup output to quiescence,
enable logging, and issue the single canonical `Ctrl+T`, `Ctrl+R` reset.

## Measurement epoch phases

### Phase 1 — Flash / setup

`idf.py flash` may reset the ESP32-P4 and immediately boot the application.
That boot is the **SETUP / POST-FLASH BOOT**, not the canonical measurement
epoch. It does not consume the exactly-one-canonical-reset allowance. The
application may complete before IDF Monitor is attached, may still be running
when the monitor attaches, or may have output still arriving through the
serial path. All of that output is diagnostic setup output only.

### Phase 2 — Monitor preparation

After flashing has completed:

1. Start IDF Monitor with the exact matching build directory and `--no-reset`.
2. Wait until the monitor is connected.
3. Before enabling transcript logging, allow the setup application and its
   serial output to finish and become quiescent. If the setup-side
   `Returned from app_main()` appears, it is a setup terminal marker; continue
   observing until no further setup output is arriving. Do not use an arbitrary
   fixed sleep as the primary synchronization mechanism. If setup completed
   before the monitor attached and no setup output is observable, proceed once
   the monitor is connected.
4. Press `Ctrl+T`, then `Ctrl+L`, and confirm that transcript logging is
   enabled.

No formal measurement has started yet. A post-flash boot that occurred before
logging does not prohibit the canonical reset below.

### Phase 3 — Canonical epoch

After logging is confirmed enabled, press `Ctrl+T`, then `Ctrl+R` exactly once.
This deliberate reset begins the canonical measurement epoch. The formal
transcript is expected to cover this logged epoch, including its meaningful
boot/application sequence, correctness/performance evidence, and terminal
completion marker.

Maintain the logical state `canonical_boot_seen = false` after issuing the
reset. Do not accept any terminal marker as canonical until a fresh
post-reset bootloader/boot sequence has been observed (for example, the normal
ESP-IDF second-stage boot progression); then set `canonical_boot_seen = true`.
Only application evidence after that fresh boot belongs to the canonical
epoch.

Once this canonical epoch has begun, do not issue another reset or rerun in the
same measurement task. If logging, capture, or results fail after the
canonical reset, preserve the evidence and stop as `INVALID`.

### Boundary cases

- **Case A — setup completed before monitor attach:** connect with `--no-reset`,
  confirm that no setup output is observable, then enable logging and issue the
  one canonical reset.
- **Case B — monitor attaches during setup:** keep logging disabled and wait for
  setup completion/quiescence; a setup-side `Returned from app_main()` is not
  canonical.
- **Case C — setup output leaks into a transcript prefix:** retain the
  transcript, identify the fresh post-reset boot, and analyze only the complete
  canonical subrange. A prefix alone is not invalidating.
- **Case D — canonical capture fails before completion:** stop without another
  reset or rerun and report `INVALID`.

## Monitor

`tools/dev/p4-nano-monitor.sh` prepares the canonical monitor invocation. It
does not automate interactive monitor menu keys, logging, reset, flashing, or
rebuilds.

After the canonical reset, first qualify the fresh boot sequence and then
observe the canonical HELLO/application and benchmark evidence through the
expected terminal marker, normally `Returned from app_main()`. A terminal
marker is canonical only when the canonical reset was issued, a fresh boot was
observed after that reset, and the marker occurs after that boot. Only that
ordered terminal marker ends formal capture. Then press `Ctrl+T`, `Ctrl+L` to
stop logging and exit with `Ctrl+]`.

Output from a post-flash setup boot before logging may appear interactively in
the terminal, but it is not formal transcript evidence and does not invalidate
the future canonical epoch. A setup-side `Returned from app_main()` must never
stop canonical capture. The no-second-run rule applies only after the logged
`Ctrl+T`, `Ctrl+R` reset has begun the canonical epoch.

## Transcript artifacts

IDF Monitor creates `log.*` local artifacts. They are ignored and must not be
committed. A formal result records the transcript path, byte count, and
SHA-256.

The preferred formal transcript starts after setup output has drained and
contains only the canonical epoch. A small pre-canonical prefix does not by
itself invalidate a run when the fresh post-reset boot and the complete
canonical epoch can be unambiguously identified. In that case, formal
analysis begins at the fresh canonical boot; do not use setup-prefix timings,
CRCs, PASS markers, VSYNC values, or terminal markers as evidence.

The capture is **INVALID** if no fresh canonical boot can be identified, the
canonical scalar/PIE evidence is incomplete, the canonical terminal marker is
missing, setup/canonical ordering is ambiguous, or another reset occurs after
the canonical epoch begins.

## Leading NUL preamble

Leading NUL bytes have appeared in some captures and have been absent in
others. Their physical cause is **NOT ESTABLISHED**. Treat them as a
**transport preamble artifact**, not as proof of a particular UART
initialization behavior.

An otherwise coherent canonical subrange may be reported as:

```text
BOOT-TO-COMPLETION CAPTURE = PASS WITH PREAMBLE ARTIFACT
```

only when it contains one meaningful post-reset boot/application sequence, no
post-HELLO panic or reset, expected terminal evidence, and
`Returned from app_main()` after that boot. A setup prefix, including residual
setup lines or a documented leading NUL transport artifact, must be excluded
from formal analysis.

## External USB-UART and GPIO20

The canonical production/P10 procedure uses the onboard CH343. External
USB-UART and GPIO20 are not part of the current canonical P10 measurement
procedure. Retained P9 and bring-up diagnostics may use GPIO20; do not apply
those diagnostic routes to this procedure.
