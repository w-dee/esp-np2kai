# Development Environment

## Baseline

The initial project baseline is:

| Item | Version or target |
| --- | --- |
| Host OS | Ubuntu 24.04 |
| Current ESP-IDF baseline | v5.5.4 |
| Current esp-emu baseline | v0.39.0 |
| Current ESP target | `esp32p4` |
| New platform-side C++ | C++20 |

The versions are recorded in [`tools/versions.env`](../tools/versions.env).
That file is informational and must not modify the user's shell automatically.

## Environment boundaries

The project must not use or depend on a PlatformIO-installed ESP-IDF
environment. The ESP-IDF and `esp-emu` setup should be reproducible and
explicitly selected by the developer or CI environment. This initial setup
does not install system packages, ESP-IDF, `esp-emu`, or other external
software.

The minimal headless Hello World application exists under `firmware/` and has
been built and executed successfully under ESP-IDF v5.5.4 and esp-emu v0.39.0.
The UART Control Plane Base uses only ESP-IDF-provided `json` and UART
components; no external component dependency has been added.

The firmware targets `esp32p4` through `firmware/sdkconfig.defaults`. New
firmware C++ is explicitly compiled as GNU C++20 in the `main` component.
C++ exceptions and RTTI are disabled through ESP-IDF configuration, and no
`iostream` is used.

The `esp-emu` v0.39.0 test environment reports ESP32-P4 revision v3.1. The
current defaults therefore select `CONFIG_ESP32P4_REV_MIN_301=y`. This setting
is verified for the emulator environment; physical P4-NANO and TAB5 revision
compatibility remains unverified and must be reviewed during physical-board
bring-up.

This is the current ESP32-P4 baseline, not a universal future baseline for
every Espressif SoC target. No ESP32-S31 toolchain, target, or emulator
requirement has been established.

## NP2kai snapshot import and verification

The Step 3 NP2kai snapshot is reproduced from a caller-provided local Git
checkout. The checkout must have the expected upstream origin and the pinned
commit object; the tools do not fetch, checkout, initialize submodules, or
copy from a working tree.

The normal import and verification commands are:

```bash
python3 tools/np2kai/import_np2kai.py --source <local-upstream-checkout>
python3 tools/np2kai/verify_np2kai.py
python3 tools/np2kai/verify_np2kai.py --source <local-upstream-checkout>
python3 tools/np2kai/verify_np2kai.py \
  --source <local-upstream-checkout> \
  --replacement-safety
```

The importer validates the manifest and expected upstream repository identity,
reads exact Git blob bytes and regular-file modes from the pinned commit, writes
the staged snapshot, and validates its file set, hashes, license evidence, and
generated metadata. Without `--source`, the verifier checks the installed
manifest, exact tree, hashes, generated metadata, and license evidence. With
`--source`, it additionally compares installed file bytes and modes with the
pinned Git objects.

`--replace` is not arbitrary recursive directory replacement. It accepts only
an existing importer-managed snapshot, rejects symlink targets and unsafe path
components, builds and validates a sibling staging directory, swaps through a
temporary backup, restores the original snapshot if the final swap fails, and
cleans staging/backup artifacts after success. A failed replacement must not
silently remove an unmanaged directory.

The verifier currently approves the URL, commit, and project version through
its `EXPECTED_URL`, `EXPECTED_COMMIT`, and `EXPECTED_VERSION` constants. A
future upstream SHA/version refresh must update those approved constants in a
separately reviewed source/tooling change before a new manifest can pass
source-aware verification. Do not use the current verifier with a new identity
before that change is reviewed.

For a future refresh, prepare the prospective manifest outside the current
managed snapshot, generate a temporary candidate, and review its source,
allowlist, baseline, license evidence, modes, hashes, and generated metadata.
After the verifier identity-pin change has been reviewed, run the source-aware
verification against the temporary candidate, then use the prospective
manifest with `--replace` for the tracked `third_party/np2kai/` destination.
Do not edit the installed `import-manifest.json` in place before replacement.

Tool-only changes should also pass:

```bash
python3 -m py_compile tools/np2kai/import_np2kai.py tools/np2kai/verify_np2kai.py
git diff --check
```

## Development targets

The intended progression is:

1. Binary Data Plane, File Transfer Base/virtual storage, and NP2kai snapshot
   verification (completed foundations).
2. Ubuntu native builds and tests for the candidate emulator core.
3. `esp-emu` for the ESP32-P4 headless core, FreeRTOS, RISC-V integration, and
   headless regression.
4. After physical hardware arrival, ESP32-P4-NANO-KIT-D and M5Stack TAB5 board
   features, including storage, display, input, and audio.

ESP32-S31 / S31 Korvo-1 is a possible future portability target only. It is not
part of the current bring-up sequence and is not implemented, tested, or
validated.

The automated Hello World check is
[`tools/emu/test-hello-world.sh`](../tools/emu/test-hello-world.sh). It will
activate the separately installed ESP-IDF v5.5.4 environment, build and merge
the firmware, then run the explicit `~/.local/bin/esp-emu` executable. It
checks both ESP-IDF v5.5.4 and esp-emu v0.39.0 before building. It must not
routinely call `idf.py set-target`; a target change is an explicit setup
operation because that command regenerates configuration and clears the build
directory.

The UART Control Plane Base is verified under `esp-emu` v0.39.0 for the
ESP32-P4 emulator environment. Its integration check is
[`tools/emu/test-uart-control-plane.sh`](../tools/emu/test-uart-control-plane.sh).
The check preserves the existing Hello World regression, reuses the resulting
merged firmware image, starts a second esp-emu instance, waits for the
`ESP-NP2KAI UART CONTROL READY` marker, injects protocol requests, and
validates the framed responses, request IDs, malformed-input recovery, and
successful emulator exit. This verifies the emulator path only; no equivalent
physical-board test command is defined yet.

The verified Binary Data Plane v1 integration check is
[`tools/emu/test-uart-binary-data-plane.sh`](../tools/emu/test-uart-binary-data-plane.sh).
It first runs `tools/emu/test-uart-control-plane.sh`, which preserves the Hello
World regression, then reuses the merged image for a binary phase over
esp-emu v0.39.0 UART-TCP. The UART-TCP bridge provides a byte-transparent
bidirectional stream suitable for arbitrary NUL-containing data.

The binary phase verifies deterministic 64 KiB transfers in both directions,
per-frame and whole-transfer CRC, duplicate DATA idempotency, corrupted-frame
and `BAD_CRC` recovery, host-generated NACK retransmission, and text/binary
resynchronization after a false NUL delimiter. The source also implements
timeout retransmission, a shared timeout/NACK retry budget, retry exhaustion
abort, and mismatched-NACK abort as v1 semantics; these are not all separately
injected runtime cases.

The verified File Transfer Base integration check is
[`tools/emu/test-file-transfer-base.sh`](../tools/emu/test-file-transfer-base.sh).
It runs the complete preceding regression chain, then uses the merged image for
a RAM-backed file phase over UART-TCP. That phase verifies pagination under an
explicit response budget, a 131,109-byte upload and exact readback, terminal
RX ACK replay, whole-transfer CRC, ranges, abort-safe staged replacement,
zero-length behavior, UTF-8 names, single-component listing-cursor rejection,
and bounded path/storage errors.

Its additional artifacts are:

- `firmware/build/esp-emu-file-transfer-base.log`
- `firmware/build/esp-emu-file-transfer-base.uart.bin`

This remains an emulator and RAM-backend result. It does not validate FATFS,
microSD hardware, durability, physical UART transport, throughput, or timing.

The primary success evidence is protocol validation on the UART-TCP socket.
After the final JSON response, the helper performs controlled esp-emu cleanup;
the binary phase does not use `--exit-on` for arbitrary binary UART data. It
preserves these artifacts:

- `firmware/build/esp-emu-uart-binary-data-plane.log`
- `firmware/build/esp-emu-uart-binary-data-plane.uart.bin`

This verifies the emulator test bridge only. Physical P4-NANO-KIT-D, CH343P,
and TAB5 UART transport, throughput, and timing remain unverified.

The ESP-IDF v5.5.4 activation script is not safe under the test script's
strict Bash options and may run an external `eim select` operation. The test
script temporarily relaxes `-e` and `-u`, presents the expected sourced Bash
context, and suppresses that optional `eim` behavior only while activating
ESP-IDF. This workaround is specific to the ESP-IDF v5.5.4 activation script
and should be reviewed if the ESP-IDF version changes.

The future firmware should remain capable of headless operation so CPU,
memory, timer, UART, and other emulator-core tests can run without display or
audio hardware. `esp-emu` performance is not representative of real ESP32-P4
hardware performance.
