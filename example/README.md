# Examples

Two standalone programs that move one telemetry reading through the BTP
library end to end.

| file | what it does |
| --- | --- |
| [`sender.cpp`](sender.cpp) | schema → `btp::SampleWriter` → `btp::Header` → `btp::encode` → `frame.bin` |
| [`receiver.cpp`](receiver.cpp) | `frame.bin` → `btp::decode` → `btp::SampleReader` → the values back |

Each file's top comment shows, as JSON, the logical data being moved — and why
BTP puts neither that JSON nor a C struct on the wire.

## Build and run

```bash
cd example
cmake -B build
cmake --build build
./build/sender      # writes frame.bin, prints the octets
./build/receiver    # reads frame.bin, prints the reconstructed reading
```

Without CMake:

```bash
g++ -std=c++11 -I../include sender.cpp \
    ../src/codec.cpp ../src/fragmentation.cpp ../src/stream.cpp \
    ../src/messages.cpp ../src/telemetry.cpp -o sender
# same command with receiver.cpp for the receiver
```

## The five steps, in both files

1. **schema** — the field table both ends agree on (`btp::FieldSpec[]`)
2. **body** — the values as `PACKED_LE` octets (`SampleWriter` / `SampleReader`),
   with the `raw * scale + offset` engineering conversion and a nullable field
3. **header** — who / when / which topic (`btp::Header`; the timestamp is the
   *producer's*, set at measurement time)
4. **frame** — `btp::encode` / `btp::decode`: 36-octet header + body + CRC-32,
   for a chosen transport profile
5. **transport** — here a file; in a real system a radio, a serial line or USB

## Next

- [`docs/library.md`](../docs/library.md) — the full API and its guarantees
- [`docs/frame.md`](../docs/frame.md) — the 36-octet header, octet by octet
- [`docs/telemetry.md`](../docs/telemetry.md) — schemas, encodings, conversion
- [`test-vectors/`](../test-vectors/) — the canonical octets an implementation
  must reproduce
