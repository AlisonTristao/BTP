# Using the library

The reference implementation is C++11 with no allocation and no operating
system dependency. This chapter covers the API, the guarantees that make it
embeddable, how to build and test it, the conformance vectors, versioning, and
what is still open.

## 1. The four headers

| Header | Owns |
| --- | --- |
| `btp/codec.hpp` | The envelope: encode, decode, CRC, header serialization |
| `btp/fragmentation.hpp` | Splitting a logical message and reassembling it |
| `btp/stream.hpp` | COBS and the incremental serial decoder |
| `btp/aead.hpp` | Optional wire v2 encryption |

They map to two build targets. **`btp::codec`** builds `codec.cpp`,
`fragmentation.cpp` and `stream.cpp` and has **zero dependencies**.
**`btp::aead`** builds `aead.cpp`, links mbedcrypto, and is behind
`option(BTP_ENABLE_AEAD)` — turning it off removes the entire crypto
dependency from the build.

## 2. Guarantees

These hold across every function in the library, and they are what make it
usable on a microcontroller:

- **Every function is `noexcept`.** Errors come back as an enum value. Nothing
  throws, nothing logs, nothing aborts.
- **Nothing allocates.** The caller supplies every buffer, every reassembly
  slot and all storage. There is no hidden growth and no `realloc` on overflow
  — an overflow is a returned error.
- **Nothing is written on failure.** If a call does not return `Ok`, the output
  buffer and the out-parameters are untouched. A rejected encode does not leave
  a half-written frame behind, and does not set `bytes_written`.
- **`decode()` is zero-copy.** The decoded payload is a view *into the caller's
  input buffer*. It is valid only as long as that buffer is, which is the one
  lifetime rule you have to hold in your head.
- **`encode()` is safe in place.** If the payload already sits at
  `output + 36`, encoding over it works — the implementation uses `memmove`
  rather than assuming disjoint buffers.
- **The clock is injected.** Reassembly takes `now_ms` as a parameter, so the
  library has no time dependency and behaves identically in a test and on
  hardware.

## 3. The trap

A zero-initialized `Header` is **not** valid. Three fields have to be set even
for the simplest message:

```cpp
btp::Header header = {};
header.type = btp::MessageType::Telemetry;
header.source_id = 1U;        // must be non-zero
header.boot_id = 1U;          // must be non-zero
header.fragment_count = 1U;   // must be 1, not 0, when unfragmented
```

`fragment_count = 0` fails `InvalidFragmentation`, which is the single most
common first-use surprise. The rule is in
[The datagram](frame.md#6-how-a-decoder-validates): unfragmented means index 0
and count exactly 1.

## 4. Encoding and decoding

```cpp
std::size_t written = 0U;
const btp::Frame frame = {header, {payload, payload_size}};

btp::Error rc = btp::encode(frame, btp::TransportProfile::EspNow,
                            buffer, sizeof(buffer), &written);
if (rc != btp::Error::Ok) {
    // btp::error_string(rc) gives a human-readable reason
}

btp::DecodedFrame decoded = {};
rc = btp::decode(buffer, written, btp::TransportProfile::EspNow, &decoded);
```

`encoded_size()` computes the exact wire size without writing anything, which
is how you size a buffer for one specific payload. For the ceiling of a whole
profile — sizing a receive buffer, or deciding whether a message needs
fragmenting — use `max_frame_size()` and `max_payload_size()`.

The `TransportProfile` argument is the whole transport abstraction. It is a
plain enum that selects a size ceiling — there are no virtual interfaces, no
callbacks and no I/O anywhere in the library. The caller owns the radio, the
UART and the HID handle.

## 5. Fragmentation and reassembly

`fragment_count()` tells you how many frames a logical payload needs.
`make_fragment()` produces one fragment as a zero-copy view over a slice of
that payload.

The `Reassembler` is constructed over caller-owned slots and storage, plus a
timeout:

```cpp
btp::ReassemblySlot slots[4];
std::uint8_t storage_bytes[4][2048];
btp::ReassemblyStorage storage[4] = { /* ... */ };

btp::Reassembler reassembler(slots, storage, 4U, 2000U);

btp::ReassembledMessage message = {};
btp::ReassemblyEvent event = reassembler.push(decoded, now_ms, &message);
if (event == btp::ReassemblyEvent::Complete) {
    // use message.payload ...
    reassembler.release(message.slot_index);   // <- do not forget this
}
```

`valid()` reports whether the buffers you supplied are large enough; check it
once at startup.

**Releasing is mandatory.** A completed message keeps its slot so its payload
view stays stable. A consumer that never releases will exhaust its slots and
start getting `NoSlot`. Completed slots do expire on the timeout, but arriving
there means you already lost data.

`expire(now_ms)` sweeps timed-out slots and returns how many it freed;
`clear()` resets everything.

## 6. COBS and the serial decoder

`cobs_encode()` and `cobs_decode()` work on caller-owned, non-overlapping
buffers. `cobs_max_encoded_size()` gives the worst case.

`SerialDecoder` is the incremental receiver: feed it one byte at a time and it
tells you when a frame completed.

```cpp
std::uint8_t encoded[btp::kSerialMaxCobsBlockSize];
std::uint8_t decoded_buffer[btp::kSerialMaxFrameSize];
btp::SerialDecoder decoder(encoded, sizeof(encoded),
                           decoded_buffer, sizeof(decoded_buffer));

btp::DecodedFrame frame = {};
btp::SerialDecodeResult result = decoder.push(byte, &frame);
if (result.event == btp::SerialDecodeEvent::Frame) { /* ... */ }
```

Events are `None`, `Frame`, `CobsError`, `FrameError`, `Overflow` and
`InvalidConfiguration`. On `FrameError`, `result.frame_error` carries the
specific `btp::Error`, so a CRC failure is distinguishable from a bad magic.

The decoder starts unsynchronized on purpose and discards input until the first
delimiter. It is hardwired to the serial profile — there is no equivalent
incremental decoder for ESP-NOW or USB HID, and none is needed, because both of
those deliver whole messages already.

## 7. Encryption

`btp::aead` is a thin layer over mbedtls that the caller drives *around* the
codec. The codec itself never touches a key.

Sending is seal, then fragment, then encode. Because `encode()` expects an
already-encrypted payload, the AAD has to be built before it runs — which is
what `encode_header()` is for:

```cpp
const btp::AeadKey key = {key_bytes, sizeof(key_bytes)};
btp::AeadError rc = btp::aead_seal(key, logical_header, plaintext_size,
                                   plaintext, ciphertext_and_tag);
```

Receiving is decode, then reassemble, then open:

```cpp
rc = btp::aead_open(key, message.header,
                    static_cast<std::uint16_t>(message.payload.size),
                    message.payload.data, plaintext);
```

`aead_seal`/`aead_open` dispatch on the `CIPHER_ID` in the header's flags. The
per-cipher functions are also exposed directly. `aead_nonce()` derives the
12-octet nonce from the header if you need it explicitly.

`AeadError::TagMismatch` is the one that matters: it means authentication
failed, and the message must be discarded rather than examined.

## 8. Build and test

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

| Option | Default | Effect |
| --- | --- | --- |
| `BTP_ENABLE_AEAD` | `ON` | Build `btp::aead`. Off removes the crypto dependency entirely. |
| `BTP_BUILD_TESTS` | on top-level only | Build the test suites. Off for a consumer, so it needs no Python. |
| `BTP_INSTALL` | on top-level only | Generate install rules. |
| `BTP_USE_SYSTEM_MBEDTLS` | `OFF` | Use an installed mbedtls instead of fetching one. |

### As a dependency

Three ways, in order of how self-contained they leave you.

**Installed.** `cmake --install` puts the four headers, the static library and
a CMake package config in your prefix, and nothing else:

```bash
cmake -S . -B build && cmake --build build
cmake --install build --prefix /usr/local
```

```cmake
find_package(btp 2.0 REQUIRED)
target_link_libraries(app PRIVATE btp::codec)
```

`btp::aead` joins the installed package only when mbedtls came from outside
this build (`BTP_USE_SYSTEM_MBEDTLS=ON`, or a parent project that already
defines `mbedcrypto`). A fetched mbedtls is never installed, so exporting a
target that links it would hand you a package that configures and then fails at
link time.

**As a subproject.** `add_subdirectory()` or `FetchContent`. Tests and install
rules switch themselves off, and if your project already defines an
`mbedcrypto` target, BTP links that one instead of fetching a second copy.

**PlatformIO.** The repository is a library as-is: `library.json` sets
`includeDir: include` and `srcDir: src`. PlatformIO has no equivalent of
`BTP_ENABLE_AEAD`, so `src/aead.cpp` guards itself on whether the mbedtls
headers are reachable — it compiles fully on ESP32, where they ship with the
SDK, and compiles to nothing on a board without them instead of breaking the
build over a feature you may not want.

The embedded compile-and-smoke target lives in `tests/embedded`:

```bash
pio run -d tests/embedded
```

The suites cover the codec, transports and COBS, the v1 and v2 conformance
vectors, the AEAD primitives, and the AEAD conformance vectors. Two of them are
Python reference decoders wired in as ctest targets — deliberately independent
reimplementations, so that a bug in the C++ decoder cannot hide inside a test
that shares its logic.

## 9. Conformance vectors

`test-vectors/v1/` and `test-vectors/v2/` are the machine-checkable half of the
contract. Each vector is a `.json` describing a frame beside the `.bin` it
produces, listed in a `manifest.json`, split into `valid/` and `invalid/`.

An invalid vector carries the exact `expected_error` it must produce, so
"rejected" is never good enough — a decoder has to reject it *for the right
reason*, in the right order.

Regenerate or verify with:

```bash
python tools/test_vectors.py --root test-vectors/v1 --check
python tools/test_vectors_v2.py --root test-vectors/v2 --check
```

Without `--check` the tools rewrite the `.bin` files from the descriptions.

The v2 set includes `aead_fragmented_gcm_0` and `aead_fragmented_gcm_1`: the
two fragments of one message sealed whole, whose `aead` block publishes the
key, nonce, AAD and plaintext so any implementation can reproduce the exact
ciphertext. They are the cross-implementation proof for
[the canonicalized AAD](encryption.md#5-the-aad-is-the-canonicalized-logical-header).

A minimum bar for a new implementation: produce every valid vector byte for
byte, consume every valid vector, reject every invalid vector with the stated
error, round-trip fragmentation, and reassemble the fragmented AEAD vectors.

**Changing a vector is changing the contract.** It is not a test edit — it is a
wire change, with the version process in section 10 attached to it.

## 10. Versioning

Four things get called "the version" and they are different:

| Concept | Notation | What it means |
| --- | --- | --- |
| Wire version | `wire 0x01`, `wire 0x02` | The octet at offset 4 of the header |
| Release | `v1.1.0-beta` | The git tag covering spec, library and vectors together |
| Branch | `1.x` | Holds the previous major; `main` always holds the newest |
| Library | `2.0.0` | `CMakeLists.txt`, `library.json`, `kLibraryVersion*` |

Never write "BTP v1" unqualified — it is ambiguous between at least three of
those.

`main` always carries the newest major. When a new major lands, the previous
one is cut to an `N.x` branch and maintained there — `1.x` holds the wire v1
line today, and a `2.x` branch would be cut from the current `main` if a wire
v3 ever arrived.

A pre-release suffix belongs to the line that is still settling, not to `main`:
the `1.x` line published `v1.1.0-beta`, while `main` declares a plain `2.0.0`.


Specification, library and vectors ship as **one SemVer line**. MAJOR is an
incompatible wire change, MINOR is a backward-compatible addition, PATCH is a
correction that does not change the octets. A `-beta` suffix applies while the
contract is still moving.

A consumer pins an immutable version or revision and does not keep its own copy
of the spec, the codec or the vectors. Because there is no legacy mode, an
incompatible change requires coordinated updates: a wire change is complete
only when every supported platform produces and consumes the same octets.

## 11. Known limitations

Honest list of what is open today.

**No anti-replay.** A captured valid frame can be reinjected and will verify.
Deliberate, and documented in [Encryption](encryption.md#7-what-v2-does-not-protect).

**No key provisioning.** Distributing keys is out of scope and no mechanism is
offered.

**Reassembly caps fragment payloads at the serial ceiling, not per transport.**
`Reassembler::push()` bounds a fragment payload by `kSerialMaxPayloadSize`
regardless of which transport it came from. This is a backstop rather than a
hole: every fragment reaching it already passed `decode()` under its own
profile's limit, and a gateway reassembling from ESP-NOW in order to
re-fragment onto serial legitimately needs the wider bound. It is documented
rather than tightened because narrowing it would mean binding a `Reassembler`
to a single transport at construction.

**No incremental decoder for ESP-NOW or USB HID.** `SerialDecoder` is serial
only. Neither of the other profiles needs one — both deliver complete messages
— but the asymmetry is worth knowing about.

**Session, handshake and dedup are specified but not implemented here.**
`HELLO`, session lifetime, command deduplication, subscriptions and the
priority queues are part of the contract in
[Commands](commands.md) and [Session and terminal](session-and-terminal.md),
but the library implements only framing, CRC, fragmentation, reassembly and
COBS. Everything above that is the integrator's to build.
