#!/usr/bin/env python3
"""Mutation tests for the R11 terminal publication seam validator."""

from __future__ import annotations

from validate_p4_audio86_terminal_publication_log import validate_text


SUCCESS = """P4_AUDIO86_TERMINAL_PUBLICATION_TEST mode=1 actual_path=HLT_BOARD86_RESET_ADAPTER_BINDING_WORKER_RING_CONTROLLER hold_ack=1 event_visible=1 terminal_absent_before_release=1 notify_before_event=45 notify_after_event=45 pre_ack_pair=1 worker_pair=1 reset_before_remainder=1 retained=1 q399_before_continuation=1 q398_accepted=1 q399_visible=1 q399_accepted=1 virtual_gap_ms=5 service_horizon_ms=20 partial_event=0 partial_wake=0 producer_done=1 transport_residual=0 first_error=0 result=PASS
P4_AUDIO86_REAL_GUEST_RESULT=PASS
"""

FAILURE = """P4_AUDIO86_TERMINAL_PUBLICATION_TEST mode=2 actual_path=HLT_BOARD86_RESET_ADAPTER_BINDING_WORKER_RING_CONTROLLER hold_ack=1 event_visible=1 terminal_absent_before_release=1 notify_before_event=45 notify_after_event=45 pre_ack_pair=0 worker_pair=0 reset_before_remainder=0 retained=0 q399_before_continuation=0 q398_accepted=1 q399_visible=0 q399_accepted=1 virtual_gap_ms=4294967295 service_horizon_ms=20 partial_event=1 partial_wake=1 producer_done=1 transport_residual=0 first_error=1 result=PASS
P4_AUDIO86_PCM_LIFECYCLE scenario=NONE final_occupancy=0 final_partial=0 result=FAIL
P4_AUDIO86_REAL_GUEST_RESULT=FAIL
"""


def rejected(text: str, mode: int) -> None:
    try:
        validate_text(text, mode)
    except ValueError:
        return
    raise AssertionError("mutation was accepted")


def main() -> int:
    validate_text(SUCCESS, 1)
    validate_text(FAILURE, 2)
    mutations = (
        (SUCCESS.replace("notify_after_event=45", "notify_after_event=46"), 1),
        (SUCCESS.replace("pre_ack_pair=1", "pre_ack_pair=0"), 1),
        (SUCCESS.replace("worker_pair=1", "worker_pair=0"), 1),
        (SUCCESS.replace("reset_before_remainder=1", "reset_before_remainder=0"), 1),
        (SUCCESS.replace("retained=1", "retained=0"), 1),
        (SUCCESS.replace("q399_before_continuation=1", "q399_before_continuation=0"), 1),
        (SUCCESS.replace("virtual_gap_ms=5", "virtual_gap_ms=20"), 1),
        (SUCCESS.replace("actual_path=HLT_BOARD86_RESET_ADAPTER_BINDING_WORKER_RING_CONTROLLER",
                         "actual_path=HELPER_ONLY"), 1),
        (FAILURE.replace("partial_wake=1", "partial_wake=0"), 2),
        (FAILURE.replace("transport_residual=0", "transport_residual=1"), 2),
        (FAILURE.replace("first_error=1", "first_error=0"), 2),
        (FAILURE.replace("P4_AUDIO86_REAL_GUEST_RESULT=FAIL",
                         "P4_AUDIO86_REAL_GUEST_RESULT=PASS"), 2),
    )
    for text, mode in mutations:
        rejected(text, mode)
    print(f"R11_TERMINAL_PUBLICATION_VALIDATOR_MUTATIONS={len(mutations)}_ALL_REJECTED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
