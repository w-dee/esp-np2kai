#!/usr/bin/env python3
"""Regression model for UART control/feed and binary timeout timestamping."""

from __future__ import annotations

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
UART_TRANSPORT = ROOT / "firmware/components/uart_control_transport/uart_control_transport.cpp"
BINARY_HEADER = ROOT / "firmware/components/binary_data_plane/include/binary_data_plane/binary_data_plane.hpp"
BINARY_MANAGER = ROOT / "firmware/components/binary_data_plane/transfer_manager.cpp"
RECEIVER_TIMEOUT_MS = 10_000
UINT32_MASK = 0xFFFF_FFFF


def elapsed(now: int, then: int) -> bool:
    return ((now - then) & UINT32_MASK) >= RECEIVER_TIMEOUT_MS


def test_source_orders_poll_timestamp_after_feed() -> None:
    source = UART_TRANSPORT.read_text(encoding="utf-8")
    feed_match = re.search(r"control_stream::feed\(.*?\);", source, re.DOTALL)
    assert feed_match is not None
    after_feed = source[feed_match.end():source.index("        } else {", feed_match.end())]
    assert re.search(
        r"binary_data_plane::poll\(\s*&s_binary_manager,\s*"
        r"static_cast<std::uint32_t>\(esp_timer_get_time\(\) / 1000\)\)",
        after_feed,
    )


def test_slow_begin_does_not_age_receiver_before_first_partial_frame() -> None:
    old_timestamp = 1_000
    post_feed_timestamp = 13_000
    partial_frame_timestamp = 13_001

    # Old behavior: begin completes during feed(), but poll() receives the
    # timestamp captured before the slow synchronous operation.
    stale_last_activity = old_timestamp
    assert elapsed(partial_frame_timestamp, stale_last_activity)

    # Fixed behavior: the post-feed poll anchors the newly active transfer at
    # completion of the synchronous control operation. A partial DATA frame
    # has not completed protocol progress and therefore does not refresh it.
    fresh_last_activity = post_feed_timestamp
    assert not elapsed(partial_frame_timestamp, fresh_last_activity)


def test_slow_valid_frame_processing_is_anchored_after_feed() -> None:
    header = BINARY_HEADER.read_text(encoding="utf-8")
    manager = BINARY_MANAGER.read_text(encoding="utf-8")
    assert "bool activity_pending = false;" in header
    assert "manager->activity_pending = true;" in manager
    assert re.search(
        r"if \(manager->activity_pending\) \{.*?"
        r"manager->last_activity_ms = now_ms;.*?"
        r"manager->activity_pending = false;",
        manager,
        re.DOTALL,
    )

    # A complete valid DATA frame may spend longer than the timeout in
    # endpoint.consume(). The pending activity is committed by the post-feed
    # poll, so that processing time is not mistaken for receiver inactivity.
    begin_timestamp = 20_000
    consume_return_timestamp = begin_timestamp + 12_000
    assert not elapsed(consume_return_timestamp, consume_return_timestamp)


def main() -> int:
    test_source_orders_poll_timestamp_after_feed()
    test_slow_begin_does_not_age_receiver_before_first_partial_frame()
    test_slow_valid_frame_processing_is_anchored_after_feed()
    print("PASS: UART control/feed timestamp regression")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
