# BTP — Binary Telemetry Protocol

BTP moves telemetry, logs, commands and terminal traffic between an embedded
producer and a consumer, across links with opposite characteristics: a
low-bandwidth radio with short datagrams and high loss, and a local bus with
more bandwidth and a byte-stream shape. Every message carries an identity and
an instant created at the source, and no intermediary rewrites either.

This repository is three things at once — the wire specification, the C++11
library that implements it, and the binary vectors that prove an implementation
is correct. They ship as one version.

## Current state

| Component | State |
| --- | --- |
| Wire `version == 0x01` | Specified, implemented, covered by `test-vectors/v1/`. |
| Wire `version == 0x02` (AEAD payload) | Specified, implemented in `btp::aead` with two ciphers, covered by `test-vectors/v2/`. |
| `COMMAND` / `CONTROL` payload layout | Specified, implemented in `btp::messages`, covered by `test-vectors/v2/messages/`. |
| `TELEMETRY` sample body | Specified, implemented in `btp::telemetry` (`PACKED_LE` / `TLV_LE` against a schema), covered by `test-vectors/v2/telemetry/`. |
| Command deduplication | Specified, implemented in `btp::session` (`btp::DedupCache`), covered by `tests/test_session.cpp` (behaviour, no vector tree). |
| Identity, sequencing and the transmit pipeline | Implemented in `btp::endpoint` (`btp::Endpoint`), covered by `tests/test_endpoint.cpp` (orchestration over the codec, no vector tree). |
| The decode + CRC + reassembly receive path | Implemented in `btp::receiver` (`btp::Receiver`), covered by `tests/test_receiver.cpp` (orchestration over the codec, no vector tree). |
| Library | `2.8.0` in `CMakeLists.txt`, `library.json` and `btp::kLibraryVersion*`. |
| Latest published tag | `v2.5.0` on `main` (wire `0x02`); `v1.1.0-beta` on the `1.x` line. |
| Branch `1.x` | The wire `0x01` line, kept alive there after `main` moved on to wire `0x02`. |

Wire version, release tag, branch and library version are four different things
and do not mix. [Versioning and branches](library.md#10-versioning-and-branches) says how to
refer to each, and which branch holds which.

## Where to start

**Deciding whether BTP fits your problem** — read
[Why BTP exists](why-btp.md). It is the only chapter that stands on its own,
and it answers what the design buys, what it costs, and where it does not
belong.

**Integrating BTP into a project** — read [The model](model.md), then
[The datagram](frame.md), then [Using the library](library.md). Come back for
the payload chapters when you need a specific channel.

**Reimplementing BTP on another platform** — read [The datagram](frame.md) and
[Getting it across the link](fragmentation-and-transports.md) in full, then the
payload chapters for the channels you need, then
[the conformance vectors](library.md#9-conformance-vectors) — the frame vectors
and, for the `COMMAND` / `CONTROL` payloads, `test-vectors/v2/messages/`. Your
implementation is not finished when you believe you understood the prose; it is
finished when it produces and consumes the same octets as the vectors.

## The chapters

| Chapter | What it covers |
| --- | --- |
| [Why BTP exists](why-btp.md) | The problem, what the design buys, what it costs, where it fits. |
| [The model](model.md) | Roles, the five logical channels, identity, time, delivery. |
| [The datagram](frame.md) | The 36-octet header octet by octet, flags, CRC, validation. |
| [Getting it across the link](fragmentation-and-transports.md) | Fragmentation, reassembly, and the three transport profiles. |
| [Encryption](encryption.md) | Wire v2 AEAD: ciphers, nonce, the canonicalized AAD, and the limits. |
| [Telemetry payloads](telemetry.md) | Topics, schemas, encodings, and how a client binds a field. |
| [Commands and discovery](commands.md) | Requests, results, deduplication, the manifest, subscriptions, status. |
| [Session and terminal](session-and-terminal.md) | `HELLO`, session lifetime, the opaque terminal, priority under load. |
| [Using the library](library.md) | The API, `btp::messages`, `btp::telemetry`, `btp::DedupCache`, guarantees, build, vectors, versioning, known limits. |

## Scope

This book covers what two implementations must agree on: the wire format, the
shared library and the conformance vectors.

Deliberately out of scope: the internals of any particular consumer, toolchain
tutorials, and the provisioning of identity and keys — a real operational
requirement that stays off the wire, as [Encryption](encryption.md) explains.
