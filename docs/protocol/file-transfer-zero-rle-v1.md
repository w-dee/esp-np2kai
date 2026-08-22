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
