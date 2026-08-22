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

The firmware targets `esp32p4` through the common project defaults. New
firmware C++ is explicitly compiled as GNU C++20 in the `main` component.
C++ exceptions and RTTI are disabled through ESP-IDF configuration, and no
`iostream` is used.

Production builds select the silicon family explicitly with
`tools/emu/build-production.sh --variant p4-v1x` or `--variant p4-v3x`.
Revision selectors are intentionally absent from the common defaults. The
wrapper uses separate `firmware/build-p4-v1x` and `firmware/build-p4-v3x`
trees, verifies generated revision bounds, and emits variant-labelled
artifacts. The existing esp-emu v0.39.0 regressions use `p4-v3x`.

## Board-qualified UART baud policy

UART baud is a build-time policy. Generic and unqualified builds use the
conservative 115200 8N1 default. The P4-NANO value is selected explicitly:

```bash
tools/emu/build-production.sh --variant p4-v1x --board p4-nano
```

| Board/profile | UART baud | Status |
| --- | ---: | --- |
| Generic or unqualified board | 115200 | Conservative baseline |
| Waveshare ESP32-P4-NANO-KIT-D, onboard CH343P path | 1500000 | IMPLEMENTED; HW VALIDATED on the tested board/configuration |
| Waveshare P4-NANO, 2 Mbps option | 2000000 | Experimental; separately validated option, not the normal default |
| M5Stack TAB5 | 115200 | Not high-speed qualified |
| ESP32-S31 boards | 115200 | Not high-speed qualified |

The 1500000 result is not generalized to all ESP32-P4 boards or all CH343
bridges. Qualification is specific to the tested P4-NANO board routing and
signal integrity, onboard CH343P bridge, oscillator/baud-divisor behavior,
host OS and driver, and the tested cable/hub path. Each board and host path
must be validated independently. There is no runtime baud negotiation or
automatic fallback, and host tools retain an explicit `--baud` option with a
conservative 115200 default.

The qualification evidence is repeated 256 KiB transfers and an exact
1,261,568-byte NP2 fixture SHA pass at 1500000, with a healthy control plane.
The same checks passed at 2000000, but 2 Mbps remains experimental; 1500000
is the normal P4-NANO high-speed choice because it provides more margin with
negligible measured difference.

This is the current ESP32-P4 baseline, not a universal future baseline for
every Espressif SoC target. No ESP32-S31 toolchain, target, or emulator
requirement has been established.

## NP2kai snapshot import and verification

The Step 3 NP2kai snapshot is reproduced from a caller-provided local Git
checkout. The checkout must have the expected upstream origin and the pinned
commit object; the tools do not fetch, checkout, initialize submodules, or
copy from a working tree.

### Verify the committed installed snapshot

```bash
python3 tools/np2kai/verify_np2kai.py
python3 tools/np2kai/verify_np2kai.py --source <local-upstream-checkout>
```

These commands verify the existing tracked `third_party/np2kai/` snapshot.

### Reproduce an independent candidate snapshot

Use an explicit temporary output root so ordinary reproduction does not try to
overwrite the committed managed snapshot:

```bash
python3 tools/np2kai/import_np2kai.py \
  --source <local-upstream-checkout> \
  --manifest third_party/np2kai/import-manifest.json \
  --output-root <temporary-snapshot>
python3 tools/np2kai/verify_np2kai.py \
  --vendor-root <temporary-snapshot>
python3 tools/np2kai/verify_np2kai.py \
  --vendor-root <temporary-snapshot> \
  --source <local-upstream-checkout>
```

The last command is optional when no upstream checkout is available for the
direct blob and mode comparison.

### Controlled replacement

Replacing the tracked managed snapshot is a separate, intentional operation:

```bash
python3 tools/np2kai/import_np2kai.py \
  --source <local-upstream-checkout> \
  --replace
python3 tools/np2kai/verify_np2kai.py \
  --source <local-upstream-checkout> \
  --replacement-safety
```

Use `--replace` only when intentionally updating vendor data, after generating
and verifying an independent candidate and reviewing its manifest and metadata.
Do not add `--replace` merely to test reproducibility.

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
its `EXPECTED_URL`, `EXPECTED_COMMIT`, and `EXPECTED_VERSION` constants. These
constants form an explicit approval pin for the currently accepted upstream
identity, and identity validation runs for both normal/offline verification and
verification with `--source`. A future upstream URL/SHA/version cannot pass
either mode until those approved constants are deliberately updated in a
separately reviewed source/tooling change. Do not use the current verifier
with a new identity before that change is reviewed.

For a future refresh, prepare the prospective manifest outside the current
managed snapshot, generate a temporary candidate, and review its source,
allowlist, baseline, license evidence, modes, hashes, and generated metadata.
After the verifier identity-pin change has been reviewed, run offline and
source-aware verification against the temporary candidate, then use the
prospective manifest with `--replace` for the tracked `third_party/np2kai/`
destination.
Do not edit the installed `import-manifest.json` in place before replacement.

Tool-only changes should also pass:

```bash
python3 -m py_compile tools/np2kai/import_np2kai.py tools/np2kai/verify_np2kai.py
git diff --check
```

## Ubuntu-native host validation

The Step 4 host validation uses a POSIX shell environment, a C compiler exposed
as `cc`, `make`, binutils including `nm`, and Python 3. The principal full
validation command is:

```sh
make -C host test-headless-runner-np2test
```

This builds the configured portable closure and runs the tracked formal
NP2TEST Stage-1 golden through the Ubuntu-native headless runner. Focused
result-v1 parser and execution-controller commands, including their contract
boundaries, are documented in [`host/README.md`](../host/README.md).

NASM is not required merely to execute the tracked golden through the host
runner. NASM is required when regenerating and validating the guest fixture via
the `tests/guest/np2test` build path.

The validation order remains host native first, then ESP32-P4 firmware and
integration through `esp-emu`, then real hardware. The headless framebuffer and
portable presentation contracts are now complete in the first two layers. The
P4-NANO SDMMC and Host-to-Device File Transfer paths already have scoped
hardware evidence; remaining storage lifecycle/removal/durability, physical
display, input, audio, timing, and broader performance branches remain
hardware work. The host result is not an `esp-emu` or physical-board result.

## Current headless video and presentation validation

These are the current representative entry points for the completed Step 7A
and Step 7B.1 headless contracts.

### Ubuntu-native

```bash
make -C host test-presentation-contract
make -C host test-video-runner-golden
make -C host test-video-gfx-vram-golden
make -C host test-video-gdc-golden
make -C host test-video-golden-checker
```

The video commands validate the text, direct-VRAM, and actual GDC
drawing-command reference paths. They do not validate a physical display.

### ESP32-P4 presentation

Activate the pinned environment before running the ESP checks:

```bash
source <pinned-ESP-IDF-v5.5.4>/export.sh
export ESP_EMU=<pinned-esp-emu-v0.39.0>
bash tools/emu/test-np2presentation-profile.sh
python3 tools/emu/test_validate_np2presentation_log.py
bash tools/emu/test-np2presentation.sh
```

The profile-isolation test confirms that the dedicated
`NP2_PRESENTATION_PROFILE` is selected independently. The log validator
self-test is host-side; the presentation runtime test validates the
ESP32-P4 / FreeRTOS / `esp-emu` contract.

### ESP32-P4 video

```bash
bash tools/emu/test-np2video-golden.sh
bash tools/emu/test-np2video-golden.sh --fixture gfx-vram
bash tools/emu/test-np2video-golden.sh --fixture gdc
```

Each invocation selects its descriptor and builds its oracle independently.
The approved descriptors remain the source of fixture identity and expected
framebuffer metadata.

## Development targets

The intended progression is:

1. Binary Data Plane, File Transfer Base/virtual storage, and NP2kai snapshot
   verification (completed foundations).
2. Ubuntu-native core, framebuffer, and presentation validation (completed
   Step 4, Step 7A, and Step 7B.1a milestones).
3. ESP32-P4 `esp-emu` firmware, FreeRTOS, RISC-V, video, and presentation
   validation (completed Step 5, Step 7A, Step 7B.1b, and Step 7B.1c
   milestones).
4. P4-NANO board validation: UART, SDMMC, and production Host-to-Device File
   Transfer have scoped hardware evidence at 1.5 Mbps. TAB5, physical display,
   input/audio, removal/durability, and broader media lifecycles remain future
   Step 6B/6C, Step 7B.2, Step 8, and Step 9 work.

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
successful emulator exit. Separately,
[`tools/uart_control_smoke.py`](../tools/uart_control_smoke.py) validates the
P4-NANO onboard CH343P control path at 1.5 Mbps; TAB5 remains unverified.

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

- `firmware/build-p4-v3x/esp-emu-file-transfer-base.log`
- `firmware/build-p4-v3x/esp-emu-file-transfer-base.uart.bin`

This paragraph documents the completed RAM-backed File Transfer Base and is an
emulator result. Persistent FATFS validation is documented separately as Step
6A below; production physical-SD qualification is summarized after it.

## Step 6A routine persistent-storage validation

Step 6A's authoritative local entry point is:

```bash
source <pinned-ESP-IDF-install>/export.sh
export ESP_EMU=<pinned-esp-emu-0.39.0>
idf.py --version
${ESP_EMU} --version
bash tools/emu/test-step6a-ci.sh
```

Use ESP-IDF v5.5.4 and esp-emu v0.39.0. The helper checks both versions and
also verifies the tracked NP2 fixture before running. By default, it uses one
shared incremental build tree for the non-reduced storage-provider, UART
FATFS, DOSIO, and VFS profiles; the raw reduced Stage-1 build remains
separate. The resulting application binaries remain profile-specific. Set
`STEP6A_PROFILE_BUILD_MODE=isolated` to use separate per-profile build trees
for fallback or diagnosis. Build reuse does not reuse emulator runtime state:
the profile images and their emulator processes remain independent. The
helper does not depend on a developer-local `firmware/sdkconfig`; it does not
modify the checked-in partition table.

The bounded routine covers:

- raw reduced Stage-1;
- bounded StorageFatfs provider hooks, replacement/rollback, cleanup, and
  remount persistence;
- FATFS File Transfer basic and routine 262145-byte upload/download;
- preloaded NoSpace mapping;
- 4097-byte cross-process persistence;
- high-address physical-flash persistence;
- DOSIO VFS access;
- normal VFS NP2 execution; and
- raw-poisoned VFS source-independence.

The File Transfer service limit in the Step 6A FATFS profile remains 2 MiB.
The 262145-byte routine transfer exercises multiple protocol frames, a partial
final frame, and boundary/ranged reads. The 4097-byte persistence case crosses
the 4 KiB FAT cluster boundary and remains a separate write/read emulator-
process lifecycle. A clean image using the approved 2 MiB-app / 8 MiB
validation geometry also passes a new 2 MiB file upload/readback/ranged-read
workload. This does not guarantee same-size
replacement of a full 2 MiB file on the internal FATFS fixture, because the
old target and staging file temporarily coexist; the extended replacement path
uses 419 clusters (`0x1A3000`) with a 35-cluster safety margin. The protocol
maximum remains 2 MiB.

NoSpace is validated from the generated storage geometry: the 1 MiB request
needs 256 clusters, the target is intentionally left with 255 free clusters,
and under the current approved geometry, the measured result derives a
618-cluster (`0x26A000`) prefill.
Failure occurs during begin/preallocation with `payload_frames=0`; the
pre-existing file remains intact, staging cleanup succeeds, and the endpoint
recovers. This measured prefill is not a permanent source-of-truth constant.

High-address validation requires a marker at physical offset `>= 0x400000`
inside the storage partition, rather than an exact address. Normal and
raw-poisoned VFS runs pass; poisoning changes the raw `np2test` range while
the storage hash remains unchanged, demonstrating source independence.

Each profile explicitly supplies the complete selector set and clears the
corresponding selector environment variables before CMake configuration, so
state cannot leak between profiles. Shared mode reconfigures the reused tree
for each profile and performs a storage-provider round-trip check to detect
stale profile state. This build reuse does not merge runtime lifecycles:
persistence write/read and normal/poisoned VFS checks still run in separate
emulator processes.

UART-intensive File Transfer tests under esp-emu are dominated by stop-and-
wait frame/ACK scheduling. Observed transfer time was approximately linear,
not a protocol deadlock. The UART test harness uses `--batch-size 5000`;
tested larger values 50000, 100000, and 200000 increased ACK latency and test
time. This is a test-harness setting, not a global esp-emu recommendation;
non-UART tests retain their normal emulator configuration.

The first pushed Step 6A CI run succeeded on GitHub Actions:

```text
workflow: NP2TEST fixture CI
run: #75
commit: 9378dab22e39836c588ae668a04fadd7d788b1cc
jobs: np2test, ubuntu-native-headless, NON-FORMAL ESP32-P4 esp-emu reduced Stage-1
conclusion: success
```

Observed first-run timing was approximately 3m29s provisioning, 1m02s for
the existing raw reduced validation, 9m00s for the Step 6A bounded suite, and
13m37s for the complete esp-emu job. These are observations, not guaranteed
timings. The existing 30-minute job timeout remains unchanged. Provisioning
is performed once in the existing esp-emu job; Step 6A does not add a second
job or duplicate provisioning.

The formal machine configuration remains `EXTMEM=13` and the authoritative
formal result is the Ubuntu-native 13/13 CRC `0x58f5b827`. The ESP32-P4
esp-emu result is explicitly NON-FORMAL reduced `EXTMEM=8`; its VFS/FATFS NP2
run also reaches 13/13 with CRC `0x58f5b827`. Those Step 6A emulator results do
not themselves imply hardware validation. Separately, a formal physical-SD
P4-NANO NP2TEST run reached 13/13 with the same CRC; it is distinct from the
File Transfer qualification below.

Current production File Transfer status is:

| Feature | ESP-EMU / CI | P4-NANO hardware | Transport selection |
| --- | --- | --- | --- |
| raw W=1 | validated | Host-to-Device physical SD at 1.5 Mbps | default W=1 |
| bounded `zero-rle-v1` W=1 | validated and wired into CI | Host-to-Device physical SD at 1.5 Mbps | default W=1 when selected |
| raw W=2 bounded Go-Back-N | validated | Host-to-Device physical SD at 1.5 Mbps | opt-in |
| bounded `zero-rle-v1` W=2 | validated and wired into CI | Host-to-Device physical SD at 1.5 Mbps | opt-in |

The bounded production NP2TEST fixture plan is 1,907 encoded bytes, 115
ZERO_RUN records, 96 LITERAL records, 852 literal bytes, and 22 DATA frames.
The independent 1,812-byte maximal-run stream remains a valid wire-format
reference and must not be confused with production Host framing. Hardware
File Transfer evidence is exact size/SHA and transport-event validation; it is
not itself a formal 13-test NP2TEST execution.

The primary success evidence is protocol validation on the UART-TCP socket.
After the final JSON response, the helper performs controlled esp-emu cleanup;
the binary phase does not use `--exit-on` for arbitrary binary UART data. It
preserves these artifacts:

- `firmware/build-p4-v3x/esp-emu-uart-binary-data-plane.log`
- `firmware/build-p4-v3x/esp-emu-uart-binary-data-plane.uart.bin`

This verifies the complete bidirectional emulator test bridge. The production
Host-to-Device subset has separate P4-NANO/CH343P hardware evidence at
1.5 Mbps through File Transfer; the complete physical bidirectional suite and
TAB5 remain unverified.

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

## CI structure

The workflow currently has five jobs. Presentation checks are integrated into
the existing video jobs rather than provisioned as a separate job.

The Ubuntu-native video job covers deterministic fixture preparation,
framebuffer goldens, the portable presentation contract, and the ESP
presentation-log validator self-test. The ESP32-P4 / `esp-emu` video job covers
presentation profile isolation, text golden, direct-VRAM golden, GDC golden,
and the ESP presentation contract. The other headless, reduced Stage-1, and
fixture jobs retain their separate scopes.
