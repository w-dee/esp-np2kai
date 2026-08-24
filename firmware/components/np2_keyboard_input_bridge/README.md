# Owner-task keyboard bridge

This ESP-IDF component is the B1 transport and adapter around the pure
`np2_keyboard_input` ownership engine.

```text
producer task(s) --enqueue--> static FreeRTOS queue
                                      |
                         NP2 owner task only
                                      v
                         ownership / duplicate suppression
                                      v
                         keystat_keydown/up -> pccore_exec(TRUE)
```

The queue has 64 fixed-size commands and producers never block. The owner
drains at most 8 commands per pre-exec iteration so a burst cannot starve the
core. Queue overflow latches a fail-closed quarantine: stale commands are
discarded, ownership is cleared, `keystat_allrelease()` is issued once, and
explicit owner-task rearm is required before new input is accepted.

The bridge is not connected to USB or UART in B1. Its production composition
starts with an empty queue, preserving existing runtime behavior.
