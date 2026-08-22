# File Transfer `zero-rle-v1`

This protocol extension applies only to host-to-device File Transfer writes.
Device-to-host reads remain raw, and an omitted `encoding` field continues to
select the existing raw write behavior. The Binary Data Plane frame format,
COBS framing, payload limit, CRC, ACK/NACK protocol, retry behavior, and
stop-and-wait sequencing are unchanged.

## Wire format

The encoded stream has no magic value or header. It is a sequence of records:

```text
ZERO_RUN:  0x00 | uint32_le length
LITERAL:   0x01 | uint32_le length | exactly length bytes
```

Lengths are nonzero. Unknown tags, incomplete headers or literals, zero
lengths, logical-size overflow, encoded-size overflow, trailing bytes, and
logical-size underrun are malformed. Records may be adjacent and literal
records may contain zero bytes. The decoder keeps its record state across
arbitrary Binary Data DATA-frame boundaries.

The logical file size is the decoded output size. The wire size is the total
encoded stream size. Both are bounded independently by the existing File
Transfer maximum. A compressed begin request must provide both:

```json
{
  "size_bytes": 123,
  "encoding": "zero-rle-v1",
  "wire_size_bytes": 17
}
```

For raw writes, `size_bytes` remains the wire and logical size and
`wire_size_bytes` is omitted. An unknown encoding is rejected as
`UNSUPPORTED`; a compressed request without `wire_size_bytes` is invalid.

## Progress and CRC

`file.write.begin` and `file.transfer.status` report `size_bytes` and
`transferred_bytes` in logical bytes for compressed writes. Compressed status
also reports `encoding`, `wire_size_bytes`, and `wire_transferred_bytes`.
`binary.transfer.status` remains transport-oriented and reports encoded wire
bytes. The Binary Data Plane CRC is calculated over the encoded wire payload;
there is no logical CRC in this version.

## Storage and errors

The decoder writes incrementally to the existing File Transfer WriteSession.
ZERO_RUN output uses a bounded shared zero buffer; it does not seek, truncate,
or create sparse files. A successful stream commits through the existing
staging-to-target transaction. Malformed streams abort without publishing a
new target and report the File Transfer error `MALFORMED_ENCODING`; the Binary
Data Plane has no new terminal reason. Physical storage errors retain their
existing error mapping, while the codec diagnostic takes precedence in the
status response when both errors are present.

A zero-byte compressed write is completed synchronously and does not start a
Binary Data Plane transfer. Raw clients and raw response shapes remain
backward compatible.

The capability `file-transfer.zero-rle-v1` is advertised alongside
`file-transfer.v1`; the protocol version is unchanged. This document records
the software and esp-emu scope only and does not claim physical UART or
physical-media validation.

## Host encoder and fixture provisioning

The host fixture-cache tool uses a bounded two-pass encoder. Pass 1 streams
the source through a 64 KiB scan buffer to calculate the logical SHA-256,
logical size, canonical record counts, and exact encoded wire size. Pass 2
reopens the source and emits at most one 1024-byte DATA payload at a time.
Canonical records are maximal contiguous zero runs and maximal contiguous
non-zero literal runs; no encoded stream is materialized in a temporary file.

The upload mode is explicit `raw`, explicit `zero-rle-v1`, or `auto`.
Explicit compressed mode requires the `file-transfer.zero-rle-v1` capability.
`auto` falls back to raw when the capability is absent and selects compressed
only when the predicted encoded size is strictly smaller than the logical
size. Raw remains the default for direct low-level client calls; fixture-cache
provisioning defaults to `auto`.

The producer retains each encoded DATA payload until its ACK, so timeout and
NACK retries retransmit the same frame byte-for-byte. Pass 2 recomputes the
source SHA-256 and checks logical size, encoded byte count, and source size/
mtime against Pass 1. A mismatch aborts the active transfer when possible and
is never reported as successful provisioning. After upload, `file.stat` and
device-side `file.sha256` verify the decoded logical file.

Diagnostics distinguish logical and wire sizes. The existing
`fixture_upload_bytes` field remains the actual DATA payload bytes sent; for
compressed uploads the encoded wire count is reported separately with the
selected encoding and DATA frame count.
