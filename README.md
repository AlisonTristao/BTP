# BTP — Binary Telemetry Protocol

BTP moves telemetry, logs, commands and terminal traffic between an embedded
producer and a consumer, across links with opposite characteristics: a
low-bandwidth radio with short datagrams and high loss, and a local bus with
more bandwidth and a byte-stream shape. Every message carries an identity and
an instant created at the source, and no intermediary rewrites either.

This repository is the canonical source of three things that ship as one
version: the wire specification, the C++11 library that implements it, and the
binary vectors that prove an implementation is correct.

> **You are on `main`** — the current major, `2.x`. The `1.x` line is
> maintained on branch [`1.x`](https://github.com/AlisonTristao/BTP/tree/1.x).
> See [Versioning and branches](#versioning-and-branches).

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
if (btp::encode(frame, btp::kEspNowTransport,
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

A transport is described to the codec as a plain
`btp::TransportLimits{max_frame_size, allow_encrypted}` — not a fixed, closed
list. Three presets cover the links below; a caller with a different one
builds its own, there is no enum to extend.

| | ESP-NOW | Serial | USB HID |
| --- | ---: | ---: | ---: |
| Max frame | 250 | 4096 | 62 |
| Max payload | 210 | 4056 | 22 |
| Link framing | one datagram per frame | `00` + COBS(frame) + `00` | 64-octet report |
| Encryption allowed | yes | yes | no |

Between transports only the numbers change, never the semantics.

## Building

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Ten targets: **`btp::codec`** (envelope, fragmentation, COBS) has zero
dependencies. **`btp::messages`** (the `COMMAND` / `CONTROL` payload layouts),
**`btp::endpoint`** (`btp::Endpoint` — local identity, sequencing and the
seal → fragment → encode transmit pipeline) and **`btp::receiver`**
(`btp::Receiver` — decode + CRC + reassembly on the way in) each link
`btp::codec` and nothing else. **`btp::telemetry`** (a `TELEMETRY` sample
against a schema — `PACKED_LE` / `TLV_LE`) and **`btp::session`**
(`btp::DedupCache`, the command-deduplication cache, and `btp::Session` /
`btp::SessionInitiator`, the session state machine on each end) each link
`btp::messages`. **`btp::catalog`** (`btp::Catalog` — the `MANIFEST_DATA`
schema catalogue, caller-owned storage) links `btp::telemetry`;
**`btp::subscription`** (`btp::SubscriptionTable` / `btp::SubscriptionClient`)
links `btp::catalog` on top of that. **`btp::node`** (`btp::Node` /
`btp::StaticNode` / `btp::SizedNode` — endpoint, receiver, session, catalogue
and subscriptions wired into one object, every external dependency a virtual
method on the `NodeConfig` you inherit from) links all five.
**`btp::aead`** (wire v2 encryption) links mbedcrypto and is behind
`-DBTP_ENABLE_AEAD=OFF` if you do not want it.

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
`test-vectors/v2/telemetry/` does the same for a `TELEMETRY` sample body,
carrying the schema it is decoded against.

```bash
python tools/test_vectors.py --root test-vectors/v1 --check
python tools/test_vectors_v2.py --root test-vectors/v2 --check
python tools/test_messages.py --root test-vectors/v2/messages --check
python tools/test_telemetry.py --root test-vectors/v2/telemetry --check
```

An implementation is not finished when its author believes they understood the
prose. It is finished when it produces and consumes these octets. Changing a
vector is changing the contract.

## Layout

```text
include/btp/    codec, fragmentation, stream, messages, telemetry, session,
                 endpoint, receiver, catalog, subscription, node, aead
src/            the implementation
example/        sender.cpp / receiver.cpp -- a producer and a consumer built
                 on btp::Node; by_hand_{sender,receiver}.cpp -- the same pair
                 with every layer called directly, no Node; hybrid.cpp -- one
                 node that is both a producer and a consumer at once
tests/          host suites plus an embedded compile target
test-vectors/   canonical vectors: wire v1, wire v2, message and telemetry payloads
tools/          independent Python reference decoders
docs/           the book
```

## Versioning and branches

**BTP is one SemVer line, `MAJOR.MINOR.PATCH`.** The number lives in one file,
[`include/btp/version.hpp`](include/btp/version.hpp); `CMakeLists.txt` parses it
and `library.json` is checked against it on every configure, so nothing is kept
in step by hand.

| Part | Bumps when | Also is |
| --- | --- | --- |
| `MAJOR` | the wire format changes incompatibly | the newest wire-version byte ([`docs/frame.md`](docs/frame.md)), and the branch name `MAJOR.x` |
| `MINOR` | a backward-compatible addition — a new library layer, an optional field | — |
| `PATCH` | a fix with no effect on the wire or the API | — |

The git tag is `vMAJOR.MINOR.PATCH` — the same number. There is no separate
"library version"; the tag, the release and `version.hpp` are one thing. So
`2.x` and `wire 2` now say the same thing, and the only term to keep distinct is
the header byte itself — write `wire 2`, not a bare `v2`, when you mean that.

### Branches

**`main` always carries the newest major.** A superseded one is cut to its own
`MAJOR.x` branch and maintained there.

| Branch | Holds | Newest wire byte |
| --- | --- | --- |
| `main` | current development, the `2.x` line — **this branch** | `0x02` (AEAD-sealed payload) |
| [`1.x`](https://github.com/AlisonTristao/BTP/tree/1.x) | maintenance of the `1.x` line, `v1.1.0-beta` | `0x01` (base frame) |

A `2.x` library still decodes a `0x01` frame — `0x01` is the base frame, `0x02`
just marks an AEAD-sealed payload. The `1.x` branch is for deployments that
cannot take the whole `2.x` library, not a sign `main` dropped wire 1. When wire
3 arrives, a `2.x` branch is cut and `main` moves to `3.0`; a branch is named
after the major it holds, never the one still coming.

To cut a release: `python tools/version.py X.Y.Z`, commit, `git tag vX.Y.Z`.
A pre-release suffix (`-beta`) is only for a still-settling `MAJOR.x` line and
only in `library.json` — CMake's `project(VERSION)` is numeric-only.

### What each 2.x minor added (no wire change)

`2.2` `btp::messages` · `2.3` verbatim manifest relay · `2.4` `btp::telemetry` ·
`2.5` body-only sample mode · `2.6` `btp::DedupCache` · `2.7` `btp::Endpoint` ·
`2.8` `btp::Receiver` · `2.9` `btp::Session` · `2.10` `priority_class()` ·
`2.11` `btp::Node` (endpoint + receiver + session, one object) ·
`2.12` `btp::Catalog` (consumer-side discovery) ·
`2.13` telemetry schema-declaration helpers, one line per field ·
`2.14` `connect()` (`SessionInitiator`) + `publish_named()` ·
`2.15` subscriptions (`SubscriptionTable` / `SubscriptionClient`) ·
`2.16` commands (`DedupCache` / `CommandClient`) + `STATUS` reporting ·
`2.21` `on_publish()` + `publish_subscribed_topics()` ·
`2.22` producer/consumer setup + loop boilerplate folded into `Node` ·
`2.23` `on_terminal()`; `StaticNode<>` bundles commands ·
`2.24` `NodeTerminalFn` gets `Node&` / `now_ms` ·
`2.25` `NodeConfig.terminal` / `.command` wire at construction ·
`2.26` `routine()` — one call covers a whole loop pass ·
`2.27` `reply_seal` — per-reply seal selection ·
`2.28` a catalogue field's unit and description ·
`2.29` `Node::reconfigure()` (removed again in 2.34 — see below) ·
`2.30` `publish_with()` / `publish_named_with()` ·
`2.31` `TransportLimits` — generic, replaces the closed `TransportProfile` enum ·
`2.32` `TransportLimits` drops `max_payload_size` (derived, not set) ·
`2.33` `SizedNode<NodeSize>` — Low / Medium / High memory tiers ·
`2.34` `NodeConfig` becomes an abstract class, replacing `reconfigure()`
(mutate the config object's fields directly instead); `HelloBuilder`

