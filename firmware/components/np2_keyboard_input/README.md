# Project-owned keyboard input contract

This component is the B0, host-testable boundary for keyboard producers. It
has no ESP-IDF, FreeRTOS, USB, UART, or NP2-core dependency and does not call
`keystat_keydown()`/`keystat_keyup()`.

`Event` contains only a small `SourceId`, a host-neutral `Key`, and an
independent `Action` (`Press` or `Release`). It intentionally contains no
character, host toolkit value, timestamp, repeat bit, allocation, NP2 frontend
ID, or PC-98 make/break byte.

The mapping detail API has a separate strong `FrontendKeyId` type. The layers
are:

```text
producer Event -> Key -> NP2 FrontendKeyId -> nkeytbl -> PC-98 device code(s)
```

The numeric values are cross-checked against the pinned vendor snapshot
(`src/keystat.h`, upstream commit
`e2dc9046aa5c786fcfbfb87e883457e421026e31`) and that same pin's SDL2
`sdl/kbtrans.c:sdlcnv106` table. The external reference checkout is not copied
into this repository. wx mapping is not a source of truth.

Modifiers are independent key edges; B0 performs no character synthesis.
Repeated identical producer reports are not repeat events. A later owner
layer must suppress duplicate `Press` edges per source/key before injection.

Left and right Shift remain distinct producer identities. The NP2 device layer
may collapse the right-side frontend alias to an effective Shift code, so a
future owner must use aggregate ownership and must not release the other side
when one side is released.

CapsLock and Kana have NP2 mechanical-toggle semantics and are not ordinary
momentary modifiers. LED synchronization, toggle ownership, and disconnect
recovery are intentionally outside B0.

For a future single-source disconnect, release only momentary keys whose
aggregate ownership reaches zero. `keystat_allrelease()` is reserved for
global emergency recovery (overflow, unrecoverable desynchronization, or
shutdown), and it does not release Caps/Kana.
