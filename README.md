# BTP - Binary Telemetry Protocol

BTP carries telemetry, logs, commands and terminal traffic between embedded
systems and desktop applications over ESP-NOW, serial and USB HID.
It defines a shared binary format with source identity, sequencing and
source timestamps. Relays are required to preserve that identity and timestamp.

This repository contains the wire specification, a C++11 library and
conformance vectors. The library provides framing, optional authenticated
encryption, sessions, schema discovery, subscriptions and command handling.
Your application supplies transport I/O and owns the storage; BTP creates no
threads and performs no heap allocation.

[Documentation](https://alisontristao.github.io/BTP/) |
[Examples](example/README.md) | [Changelog](CHANGELOG.md)

## Minimal example

Use `btp::codec` to encode individual frames. Each frame contains a 36-byte
header, a payload and a 4-byte CRC-32; multi-byte values are little-endian.
Fields are serialized explicitly, without relying on C++ struct layout.

This function encodes one byte of opaque telemetry and passes the frame to
your transport callback:

```cpp
#include "btp/codec.hpp"

bool send_sample(std::uint32_t source_id, std::uint32_t boot_id,
                 std::uint32_t sequence, std::uint64_t timestamp_us,
                 std::uint8_t value,
                 bool (*send_frame)(const std::uint8_t*, std::size_t)) {
    btp::Header header = {};
    header.type = btp::MessageType::Telemetry;
    header.source_id = source_id;
    header.boot_id = boot_id;
    header.sequence = sequence;
    header.timestamp_us = timestamp_us;
    header.object_id = 0x0101U;  // topic ID
    header.fragment_count = 1U;

    const btp::Frame frame = {header, {&value, 1U}};
    std::uint8_t buffer[btp::kEspNowMaxFrameSize];
    std::size_t written = 0U;
    return send_frame != nullptr &&
           btp::encode(frame, btp::kEspNowTransport, buffer, sizeof(buffer),
                       &written) == btp::Error::Ok &&
           send_frame(buffer, written);
}
```

Use nonzero source and boot IDs, change the boot ID on each boot, and supply
a new nonzero sequence for each logical message. The receiver must know the
topic's encoding; this example assumes an `OpaqueBytes` topic. The callback
must consume or copy the frame before returning.

For a complete producer or consumer, use **`btp::Node`** to manage identity,
sequencing, receive processing, sessions, discovery and subscriptions.
`SizedNode` supplies buffer presets; `StaticNode` lets you size the storage.
See the [producer/consumer examples](example/README.md) and
[Node guide](docs/library.md#16-the-node-layer) for setup, callbacks and memory
sizing. Set `NodeConfig.transport` before constructing the node and keep the
config alive for the node's lifetime.

## Transports and limits

The codec accepts `btp::TransportLimits{max_frame_size, allow_encrypted}`.
These are its built-in presets, with sizes in bytes:

| | ESP-NOW | Serial | USB HID |
| --- | ---: | ---: | ---: |
| Maximum BTP frame | 250 | 4096 | 62 |
| Maximum frame payload, including any AEAD tag | 210 | 4056 | 22 |
| Link framing | One datagram per frame | `00` + COBS(frame) + `00` | 64-byte report |
| Encryption allowed | Yes | Yes | No |

**`Node` / `Endpoint` transmission currently uses a 250-byte frame buffer.**
The codec's 4096-byte Serial limit does not extend to those transmit helpers:
a larger encoded frame fails. For Serial links using these helpers, configure
a frame limit of 250 bytes or less on both ends and apply COBS in the transport
integration. Logical messages can still be fragmented within the configured
limits and storage capacity. Use the lower-level codec with caller-provided
buffers when larger individual frames are needed.

Telemetry is best-effort. BTP does not provide anti-replay protection, key
rotation or routing; the integration supplies key management and peer routing.
See [protocol tradeoffs](docs/why-btp.md),
[transports](docs/fragmentation-and-transports.md) and
[encryption](docs/encryption.md) for the full contracts.

## Build and install

For a host build, use CMake, a C++11 compiler and Python 3 for the tests:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Optional `btp::aead` uses mbedcrypto. To build without it, add
`-DBTP_ENABLE_AEAD=OFF` to the configure command.

Install to a prefix of your choice:

```bash
cmake --install build --prefix /path/to/install
```

Point your application's `CMAKE_PREFIX_PATH` at that prefix, then link the
layer you need:

```cmake
find_package(btp 2.0 REQUIRED)
target_link_libraries(app PRIVATE btp::codec)  # or btp::node
```

The repository also supports `add_subdirectory()`, `FetchContent` and
PlatformIO. The [library guide](docs/library.md) lists all targets and build
options. To compile the embedded test target:

```bash
pio run -d tests/embedded
```

## Documentation and tests

* [Protocol overview](docs/index.md): frame, payload and session specifications.
* [Library guide](docs/library.md): APIs, storage, integration and build options.
* [Examples](example/README.md): producer, consumer and hybrid nodes.
* [Frame vectors](test-vectors/v2/README.md): valid and invalid frame cases.
* [Message vectors](test-vectors/v2/messages/README.md) and
  [telemetry vectors](test-vectors/v2/telemetry/README.md): payload conformance.

CTest runs the host suites and independent Python vector checks. To run the
vector checks separately:

```bash
python tools/test_vectors.py --root test-vectors/v1 --check
python tools/test_vectors_v2.py --root test-vectors/v2 --check
python tools/test_messages.py --root test-vectors/v2/messages --check
python tools/test_telemetry.py --root test-vectors/v2/telemetry --check
```

## Versioning and branches

The specification, library and vectors share the version declared in
[`include/btp/version.hpp`](include/btp/version.hpp). `main` carries the `2.x`
line; branch [`1.x`](https://github.com/AlisonTristao/BTP/tree/1.x) maintains
the earlier line. The 2.x library accepts both wire 1 (base frames) and wire 2
(AEAD-sealed payloads).

Payload compatibility also depends on the manifest format: the format-3
layout changed between 2.44 and 2.45, so producers and consumers using it
must be upgraded together. See the [changelog](CHANGELOG.md) for changes and
migration notes, and [versioning policy](docs/library.md#10-versioning-and-branches)
for release instructions.
