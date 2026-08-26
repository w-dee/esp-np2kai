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

If the SoC does not respond, repeat the same failed esptool operation no more
than three times in total. Do not change baud, wiring, erase policy, image, or
procedure after the first failure.

## Monitor

`tools/dev/p4-nano-monitor.sh` prepares the canonical monitor invocation. It
does not automate interactive monitor menu keys, logging, reset, flashing, or
rebuilds.

1. Start monitor with the exact matching build directory and `--no-reset`.
2. Wait until it is fully connected.
3. Press `Ctrl+T`, then `Ctrl+L` to start transcript logging.
4. Press `Ctrl+T`, then `Ctrl+R` exactly once for the canonical reset.
5. Observe through the expected terminal marker, normally `Returned from app_main()`.
6. Press `Ctrl+T`, then `Ctrl+L` to stop logging.
7. Exit with `Ctrl+]`.

For a formal measurement epoch, there is exactly one canonical reset and no
second reset unless separately approved. Preserve one coherent HELLO/application
epoch and its transcript identity.

## Transcript artifacts

IDF Monitor creates `log.*` local artifacts. They are ignored and must not be
committed. A formal result records the transcript path, byte count, and
SHA-256.

## Leading NUL preamble

Leading NUL bytes have appeared in some captures and have been absent in
others. Their physical cause is **NOT ESTABLISHED**. Treat them as a
**transport preamble artifact**, not as proof of a particular UART
initialization behavior.

An otherwise coherent capture may be reported as:

```text
BOOT-TO-COMPLETION CAPTURE = PASS WITH PREAMBLE ARTIFACT
```

only when it contains one meaningful boot/application sequence, no post-HELLO
panic or reset, expected terminal evidence, and `Returned from app_main()`.

## External USB-UART and GPIO20

The canonical production/P10 procedure uses the onboard CH343. External
USB-UART and GPIO20 are not part of the current canonical P10 measurement
procedure. Retained P9 and bring-up diagnostics may use GPIO20; do not apply
those diagnostic routes to this procedure.
