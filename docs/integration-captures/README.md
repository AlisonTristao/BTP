# Integration captures

Raw serial captures from real hardware runs, kept for manual comparison
during future debugging. These are **not** conformance vectors — they are
not scanned or checked by `tools/test_vectors.py`/`btp_vector_descriptions`,
and there is no format guarantee beyond "raw bytes read off the wire".

## `topico15_protocol_test_hardware.bin`

Captured 2026-08-10 during topico 15 end-to-end validation.

- **Hardware**: `bally_software` (the robot firmware, since renamed
  `bally_OS`) on an ESP32-S3 (MAC `14:C1:9F:44:24:84`,
  commit `0c26ea6`) talking ESP-NOW to a `t_dongle_develop`
  dongle (since renamed `bally_dongle`)
  (MAC `DC:DA:0C:30:AA:5C`, commit `7ef63a1`), dongle USB serial at
  921600 baud.
- **Content**: raw bytes off the dongle's protocolled serial port
  (`0x00 || COBS(frame) || 0x00` packets) for a session that did
  `BTP/1 ENTER` → `BTP/1 READY` → `HELLO`/`HELLO_RESULT` → ~15s of relayed
  `TELEMETRY` (topic `protocol.test`, object_id `0x0001`) and one
  `TERMINAL_OUT` frame (the dongle's own prompt bytes, session-scoped PTY
  from topico 19).
- **Result of decoding**: 734 frames, 0 CRC/decode failures. 725 were
  `protocol.test` samples; `counter` incremented monotonically with the BTP
  `sequence`, `value` was the fixed IEEE-754 bit pattern `0x3F0D0A00`
  documented in `TELEMETRY.md` 9.4 (so the payload legitimately contains
  `0x00`, `0x0A`, `0x0D` and none of it got mistaken for line framing).
  Measured relay rate ≈48.3 Hz against a 50 Hz publish rate in
  `TelemetryPublisher` (topico 10) — the gap is expected drop-newest queue
  behavior under the dongle's serial relay, not corruption.
- **How it was produced**: an ad hoc Python client (not committed —
  BTP-encodes `HELLO` and decodes COBS+CRC+PACKED_LE using only `struct`/
  `zlib`, no dependency on `btp::codec`) opened the dongle's serial port
  directly. Reproducing it requires re-implementing that client or driving
  a real BTP-capable client (TraceView, once its own HELLO handshake lands)
  against the same two boards.
