#!/usr/bin/env python3

from validate_np2presentation_log import validate_text


VALID_LOG = """\
NP2PRESENT_MEMORY phase=before_guest psram_total=1 free_spiram=1 largest_spiram=1 guest_bytes=0 presentation_bytes=1024000 guest_external=0 slot0_external=0 slot1_external=0
NP2PRESENT_MEMORY phase=after_guest psram_total=1 free_spiram=1 largest_spiram=1 guest_bytes=512000 presentation_bytes=1024000 guest_external=1 slot0_external=0 slot1_external=0
NP2PRESENT_MEMORY phase=after_slots psram_total=1 free_spiram=1 largest_spiram=1 guest_bytes=512000 presentation_bytes=1024000 guest_external=1 slot0_external=1 slot1_external=1
NP2PRESENT_INIT guest_external=1 slot0_external=1 slot1_external=1 slots=2 slot_bytes=512000 atomic32_lock_free=1 slot_state0_lock_free=1 slot_state1_lock_free=1
NP2PRESENT_BASIC result=PASS guest_external=1 presentation_external=1 ptr_distinct=1 width=640 height=400 pitch=1280 published_sequence=1
NP2PRESENT_IMMUTABLE result=PASS guest_locked=1 unchanged=1
NP2PRESENT_COALESCE result=PASS acquired_frame=4 published_sequence=4 coalesced_count=2 dropped_count=0
NP2PRESENT_MEMORY phase=after_resize psram_total=1 free_spiram=1 largest_spiram=1 guest_bytes=128000 presentation_bytes=1024000 guest_external=1 slot0_external=1 slot1_external=1
NP2PRESENT_RESIZE result=PASS old_generation=1 new_generation=2 old_frame_unchanged=1 width=320 height=200 pitch=640 source_generation=2
NP2PRESENT_RESULT=PASS
"""


def main() -> int:
    if validate_text(VALID_LOG):
        return 1
    if not validate_text(VALID_LOG.replace("coalesced_count=2", "coalesced_count=1")):
        return 1
    if not validate_text(VALID_LOG + "NP2PRESENT_RESULT=PASS\n"):
        return 1
    print("NP2PRESENT_VALIDATOR_SELFTEST=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
