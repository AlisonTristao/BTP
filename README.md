# BTP — Binary Telemetry Protocol

BTP moves telemetry, logs, commands and terminal traffic between an embedded
producer and a consumer, across links with opposite characteristics: a
low-bandwidth radio with short datagrams and high loss, and a local bus with
more bandwidth and a byte-stream shape. Every message carries an identity and
an instant created at the source, and no intermediary rewrites either.

This repository is the canonical source of three things that ship as one
version: the wire specification, the C++11 library that implements it, and the
binary vectors that prove an implementation is correct.

**📖 [Read the book](https://alisontristao.github.io/BTP/)** — the full
documentation, from why the protocol exists down to the octets on the wire.

## What it looks like

A frame is a 36-octet header, a payload, and a CRC-32:

```text
+---------------------------+------------------------+------------+
| header (36 octets)        | payload (N octets)     | CRC32 (4)  |
+---------------------------+------------------------+------------+
```

Everything multi-octet is little-endian, including the CRC. No struct is ever
put on the wire, so there is no ABI to negotiate between a microcontroller and
a desktop.

```cpp
#include "btp/codec.hpp"

btp::Header header = {};
header.type = btp::MessageType::Telemetry;
header.source_id = 0x11223344U;   // non-zero
header.boot_id = 0xA1B2C3D4U;     // non-zero, changes every boot
header.sequence = 1U;
header.timestamp_us = now_us();
header.object_id = 0x0101U;       // the topic id
header.fragment_count = 1U;       // 1, not 0, when unfragmented

const btp::Frame frame = {header, {payload, payload_size}};

std::uint8_t buffer[btp::kEspNowMaxFrameSize];
std::size_t written = 0U;
if (btp::encode(frame, btp::TransportProfile::EspNow,
                buffer, sizeof(buffer), &written) == btp::Error::Ok) {
    send(buffer, written);
}
```

## Why

Four things it fixes at once, each of which breaks the obvious alternative:

- **Text over serial** breaks on the first payload containing `0x0A`.
- **Shipping the struct** breaks on the first different compiler or alignment
  flag.
- **Timestamping on arrival** measures transport latency instead of the
  phenomenon.
- **One format per link** turns every gateway into a translator.

What it costs, stated up front: no anti-replay, no key rotation, no per-peer
identity, telemetry is best-effort by design, and there is no legacy mode — so
migration is coordinated rather than incremental.
[Why BTP exists](https://alisontristao.github.io/BTP/why-btp/) covers the whole
trade, and it is the one chapter that stands on its own.

## Transports

| | ESP-NOW | Serial | USB HID |
| --- | ---: | ---: | ---: |
| Max frame | 250 | 4096 | 62 |
| Max payload | 210 | 4056 | 22 |
| Link framing | one datagram per frame | `00` + COBS(frame) + `00` | 64-octet report |
| Encryption allowed | yes | yes | no |

Between profiles only a limit constant changes, never the semantics.

## Building

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Two targets: **`btp::codec`** (envelope, fragmentation, COBS) has zero
dependencies. **`btp::aead`** (wire v2 encryption) links mbedcrypto and is
behind `-DBTP_ENABLE_AEAD=OFF` if you do not want it.

For PlatformIO the repository is a library as-is. The embedded compile target:

```bash
pio run -d tests/embedded
```

## Conformance vectors

`test-vectors/v1/` and `test-vectors/v2/` are the machine-checkable half of the
contract: a `.json` describing each frame beside the `.bin` it produces, split
into valid cases and invalid ones that each name the exact error they must
provoke.

```bash
python tools/test_vectors.py --root test-vectors/v1 --check
python tools/test_vectors_v2.py --root test-vectors/v2 --check
```

An implementation is not finished when its author believes they understood the
prose. It is finished when it produces and consumes these octets. Changing a
vector is changing the contract.

## Layout

```text
include/btp/    codec, fragmentation, stream, aead
src/            the implementation
tests/          host suites plus an embedded compile target
test-vectors/   canonical vectors for wire v1 and v2
tools/          independent Python reference decoders
docs/           the book
```

## Versioning

Specification, library and vectors ship as one SemVer line. Four different
things get called "the version" and they do not mix:

| Concept | Example | Meaning |
| --- | --- | --- |
| Wire version | `wire 0x02` | The octet at offset 4 of the header |
| Release | `v1.1.0-beta` | The git tag over spec, library and vectors |
| Branch | `1.x` | A maintenance line, cut only at a MAJOR change |
| Library | `2.0.0` | `CMakeLists.txt`, `library.json`, `kLibraryVersion*` |

Never write "BTP v1" unqualified — it is ambiguous between three of those.
