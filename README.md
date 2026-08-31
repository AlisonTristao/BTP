# BTP — Binary Telemetry Protocol

BTP moves telemetry, logs, commands and terminal traffic between an embedded
producer and a consumer, across links with opposite characteristics: a
low-bandwidth radio with short datagrams and high loss, and a local bus with
more bandwidth and a byte-stream shape. Every message carries an identity and
an instant created at the source, and no intermediary rewrites either.

This repository is the canonical source of three things that ship as one
version: the wire specification, the C++11 library that implements it, and the
binary vectors that prove an implementation is correct.

> **You are on `main`** — the wire `version == 0x02` line, library `2.2.0`.
> The wire v1 line is maintained on branch
> [`1.x`](https://github.com/AlisonTristao/BTP/tree/1.x). See
> [Versioning and branches](#versioning-and-branches).

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

Three targets: **`btp::codec`** (envelope, fragmentation, COBS) has zero
dependencies. **`btp::messages`** (the `COMMAND` / `CONTROL` payload layouts)
links `btp::codec` and nothing else. **`btp::aead`** (wire v2 encryption)
links mbedcrypto and is behind `-DBTP_ENABLE_AEAD=OFF` if you do not want it.

To consume it as an installed package:

```bash
cmake --install build --prefix /usr/local
```

```cmake
find_package(btp 2.0 REQUIRED)
target_link_libraries(app PRIVATE btp::codec)
```

It also works as an `add_subdirectory()`/`FetchContent` subproject, where tests
and install rules switch themselves off. For PlatformIO the repository is a
library as-is. See [Using the library](https://alisontristao.github.io/BTP/library/)
for the full option list. The embedded compile target:

```bash
pio run -d tests/embedded
```

## Conformance vectors

`test-vectors/v1/` and `test-vectors/v2/` are the machine-checkable half of the
contract: a `.json` describing each frame beside the `.bin` it produces, split
into valid cases and invalid ones that each name the exact error they must
provoke.

`test-vectors/v2/messages/` extends this to the payload layer: the same
`.json`/`.bin` convention, but the `.bin` is a logical `COMMAND` / `CONTROL`
payload with no frame around it, checked against `btp::messages`.

```bash
python tools/test_vectors.py --root test-vectors/v1 --check
python tools/test_vectors_v2.py --root test-vectors/v2 --check
python tools/test_messages.py --root test-vectors/v2/messages --check
```

An implementation is not finished when its author believes they understood the
prose. It is finished when it produces and consumes these octets. Changing a
vector is changing the contract.

## Layout

```text
include/btp/    codec, fragmentation, stream, messages, aead
src/            the implementation
tests/          host suites plus an embedded compile target
test-vectors/   canonical vectors: wire v1, wire v2, and the message payloads
tools/          independent Python reference decoders
docs/           the book
```

## Versioning and branches

Specification, library and vectors ship as one SemVer line: an incompatible
change to the bytes or their meaning is `MAJOR`, a compatible addition is
`MINOR`, and a correction with no observable effect on the wire is `PATCH`.

### How the branches are laid out

**`main` always carries the newest major.** When a new one lands, the previous
is cut to an `N.x` branch and maintained there:

| Branch | Wire | Line |
| --- | --- | --- |
| `main` | `0x02` | Current development, library `2.2.0` — **this branch** |
| [`1.x`](https://github.com/AlisonTristao/BTP/tree/1.x) | `0x01` | Maintenance of the wire v1 line, released as `v1.1.0-beta` |

If a wire v3 ever arrives, a `2.x` branch is cut from the `main` of that
moment carrying `2.0`, and `main` moves on to `3.0`. A branch is named after
the major it holds, never after the one still coming.

### Four things called "the version"

They do not mix:

| Concept | Example | What it is |
| --- | --- | --- |
| Wire version | `wire 0x02` | The octet at offset 4 of the header |
| Release | `v2.2.0` | The git tag over spec, library and vectors. `btp::messages` (the `COMMAND` / `CONTROL` payload layouts, no wire change) is the `2.2.0` addition |
| Branch | `1.x` | Holds the previous major; `main` holds the newest |
| Library | `2.2.0` | `CMakeLists.txt`, `library.json`, `kLibraryVersion*` |

Never write "BTP v1" unqualified — it is ambiguous between three of those.

A pre-release suffix belongs to the line that is still settling, not to `main`:
the `1.x` line published `v1.1.0-beta`, while `main` declares a plain `2.2.0`.
Note that only `library.json` can spell a suffix at all — CMake's
`project(VERSION)` is numeric-only and `kLibraryVersion*` are three `uint8`.

