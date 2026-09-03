# Examples

Two pairs of standalone programs, moving one telemetry reading through the
library end to end.

| pair | what it shows |
| --- | --- |
| [`sender.cpp`](sender.cpp) / [`receiver.cpp`](receiver.cpp) | the real flow with **`btp::Node`**: the producer publishes a **`MANIFEST_DATA`** describing its topic, then a `TELEMETRY` sample; the consumer **learns the schema from the wire** and decodes the sample against it — nothing about the topic is hard-coded on the receiving side |
| [`by_hand_sender.cpp`](by_hand_sender.cpp) / [`by_hand_receiver.cpp`](by_hand_receiver.cpp) | one sample, **step by step at the wire level**: `btp::Header` → `btp::encode`, and `btp::decode` → the values back. A two-file demo with no discovery step, so it compiles the same schema on both sides |

`sender.cpp` writes the schema **once**, as `btp::FieldRecord`s — its own data
model. The manifest carries them; the sample codec narrows each to a
`btp::FieldSpec` with `btp::field_spec()`. `receiver.cpp` does the mirror:
`btp::ManifestReader` → `btp::field_spec()` per record → a small `FieldSpec`
cache → `btp::SampleReader` against it.

## Build and run

```bash
cd example
cmake -B build
cmake --build build

./build/sender             # writes frame.bin (manifest frame, then sample frame)
./build/receiver           # reads it back -- schema first, then the reading

./build/by_hand_sender     # one sample, one wire step at a time
./build/by_hand_receiver
```

Without CMake:

```bash
g++ -std=c++11 -I../include sender.cpp \
    ../src/codec.cpp ../src/fragmentation.cpp ../src/stream.cpp \
    ../src/messages.cpp ../src/telemetry.cpp \
    ../src/endpoint.cpp ../src/receiver.cpp ../src/session.cpp ../src/node.cpp \
    -o sender
# by_hand_sender.cpp needs only codec.cpp fragmentation.cpp stream.cpp
# messages.cpp telemetry.cpp
```

## The pieces

1. **schema** — `btp::FieldRecord[]` on the producer (type, order, `scale`,
   `offset`, nullability, name, unit). The consumer never writes one; it builds
   `btp::FieldSpec`s from the `FieldRecord`s in the manifest.
2. **manifest** — `btp::ManifestWriter` / `btp::ManifestReader`: the byte
   layout of `MANIFEST_DATA`, walked record by record with no allocation.
3. **sample body** — the values as `PACKED_LE` octets (`btp::SampleWriter` /
   `btp::SampleReader`), with the `raw * scale + offset` conversion and a
   nullable field.
4. **frame** — 36-octet header + body + CRC-32, encoded / decoded for a
   transport profile, fragmented if the body needs it (the manifest here does).
   `btp::Node` fills in the identity, the sequence and the frame; by hand you
   build the `btp::Header` and call `btp::encode`.
5. **transport** — here a file (length-prefixed so the receiver can split the
   frames); in a real system a radio, a serial line or USB, which deliver whole
   datagrams.

## Next

- [`docs/library.md`](../docs/library.md) — the full API
  ([§16 The node layer](../docs/library.md#16-the-node-layer))
- [`docs/telemetry.md`](../docs/telemetry.md) — schemas, encodings, conversion
- [`docs/commands.md`](../docs/commands.md) — the manifest, field by field
- [`test-vectors/`](../test-vectors/) — the canonical octets an implementation
  must reproduce
