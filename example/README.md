# Examples

Standalone programs that move one telemetry reading through the BTP library end
to end — the same reading, two ways.

| file | what it does |
| --- | --- |
| [`sender.cpp`](sender.cpp) / [`receiver.cpp`](receiver.cpp) | with **`btp::Node`**, the friendly facade: `schema → SampleWriter → node.send()` and `node.receive() → SampleReader` |
| [`by_hand_sender.cpp`](by_hand_sender.cpp) / [`by_hand_receiver.cpp`](by_hand_receiver.cpp) | the same exchange **step by step at the wire level**: `btp::Header` → `btp::encode` and `btp::decode` → the values back |

The `by_hand_*` pair is the walkthrough of what `btp::Node` fills in for you —
the identity, the sequence, the 36-octet header (and the "a zero-initialised
`Header` is not valid" trap), the fragment count, the frame encoding. Its top
comment shows, as JSON, the logical data being moved and why BTP puts neither
that JSON nor a C struct on the wire.

## Build and run

```bash
cd example
cmake -B build
cmake --build build

./build/sender             # writes frame.bin, prints the octets sent
./build/receiver           # reads frame.bin, prints the reconstructed reading

./build/by_hand_sender     # the same, one wire step at a time
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

## The steps

Both pairs do the same five things; the `btp::Node` pair hands steps 3–4 to the
node.

1. **schema** — the field table both ends agree on (`btp::FieldSpec[]`)
2. **body** — the values as `PACKED_LE` octets (`SampleWriter` / `SampleReader`),
   with the `raw * scale + offset` engineering conversion and a nullable field
3. **header** — who / when / which topic (`btp::Node` from its identity, or
   `btp::Header` by hand; the timestamp is the *producer's*)
4. **frame** — 36-octet header + body + CRC-32, encoded / decoded for a
   transport profile, fragmented if the body needs it
5. **transport** — here a file; in a real system a radio, a serial line or USB
   (a `btp::Node` calls your `send` callback; by hand you call `fwrite`)

## Next

- [`docs/library.md`](../docs/library.md) — the full API and its guarantees
  ([§16 The node layer](../docs/library.md#16-the-node-layer))
- [`docs/frame.md`](../docs/frame.md) — the 36-octet header, octet by octet
- [`docs/telemetry.md`](../docs/telemetry.md) — schemas, encodings, conversion
- [`test-vectors/`](../test-vectors/) — the canonical octets an implementation
  must reproduce
