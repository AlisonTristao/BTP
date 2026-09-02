# Using the library

The BTP reference implementation is a C++11 library that implements the lower-level mechanisms defined by the protocol.

The core library provides:

* frame encoding and decoding;
* CRC-32 validation;
* transport-size validation;
* fragmentation;
* reassembly;
* COBS framing for serial;
* incremental serial decoding;
* optional authenticated encryption.

The library does **not** perform transport I/O.

It does not:

* transmit ESP-NOW packets;
* read a UART;
* access USB HID;
* open sockets;
* create threads;
* depend on an operating system.

The application owns the transport and passes complete buffers or received bytes to BTP.

Conceptually:

```text
Application
    |
    +-- ESP-NOW
    +-- UART
    +-- USB HID
    +-- other integration
    |
    v
+-----------------------------+
|        BTP library          |
|                             |
| codec                       |
| fragmentation / reassembly  |
| COBS                        |
| optional AEAD               |
+-----------------------------+
    |
    v
BTP wire representation
```

The library implements the wire: the frame, the fragmentation, the COBS
framing, the optional AEAD, the byte layout of every `COMMAND` and `CONTROL`
payload (`btp::messages`, [The message layer](#12-the-message-layer)) and the
`TELEMETRY` sample body against a schema (`btp::telemetry`,
[§12.4](#124-decoding-and-encoding-telemetry-samples)). One stateful mechanism
sits just above that line because getting it wrong is a safety fault, not a
parse fault: `btp::DedupCache`, the command-deduplication cache
([The session layer](#13-the-session-layer)).

It deliberately stops below the rest of the mechanisms that carry *state*:

* the session state machine and its watchdog;
* command *execution* (`btp::DedupCache` remembers the result; running the
  action and sending the reply stay the integration's);
* the manifest catalogue a consumer keeps;
* telemetry schema storage (which schemas a consumer caches, and for how long);
* the priority scheduler.

Those use the BTP frame and payload formats the library defines, but their
application state is owned by the integration above it. This distinction is
important when considering memory usage.

---

## 1. The headers

The public API is divided into nine headers.

| Header                  | Responsibility                                              |
| ----------------------- | ---------------------------------------------------------- |
| `btp/codec.hpp`         | Frame encoding, decoding, CRC and header serialization      |
| `btp/fragmentation.hpp` | Fragmentation and logical-message reassembly                |
| `btp/stream.hpp`        | COBS and incremental serial decoding                        |
| `btp/messages.hpp`      | Struct ⇄ bytes for every `COMMAND` / `CONTROL` payload      |
| `btp/telemetry.hpp`     | A `TELEMETRY` sample ⇄ values against a schema              |
| `btp/session.hpp`       | `btp::DedupCache` — the command-deduplication cache         |
| `btp/endpoint.hpp`      | `btp::Endpoint` — identity, sequencing and the transmit pipeline |
| `btp/receiver.hpp`      | `btp::Receiver` — decode, CRC and reassembly on the receive path |
| `btp/aead.hpp`          | Optional version-2 authenticated encryption                 |

The implementation is exposed through seven CMake targets.

### `btp::codec`

`btp::codec` contains:

```text
codec.cpp
fragmentation.cpp
stream.cpp
```

It has no external dependency.

Applications that do not require BTP encryption or the payload layer can use
only this target.

### `btp::messages`

`btp::messages` contains:

```text
messages.cpp
```

It links `btp::codec` and has no other dependency -- in particular no
mbedtls -- so it is always built, like `btp::codec` and unlike `btp::aead`. It
carries the same guarantees as `btp::codec`: no allocation, `noexcept`,
caller-owned buffers, no partial output on failure.

It is the struct ⇄ bytes half of [Commands and discovery](commands.md) and
[Session and terminal](session-and-terminal.md): a `decode_*` / `encode_*`
pair for each fixed message, and `ManifestReader` / `ManifestWriter` for
`MANIFEST_DATA`. It adds no wire field. [The message layer](#12-the-message-layer)
covers it in full.

### `btp::telemetry`

`btp::telemetry` contains:

```text
telemetry.cpp
```

It links `btp::messages` (for `FieldRecord` and the shared error enum) and,
like it, needs no mbedtls and is always built. Same guarantees as the rest of
the library.

Where `btp::messages` decodes the manifest that *describes* a telemetry
schema, `btp::telemetry` decodes and encodes a `TELEMETRY` sample *against* a
schema the caller already holds: `SampleReader` / `SampleWriter` for the
`PACKED_LE` / `TLV_LE` body, the nullable presence bitmap and the
`raw * scale + offset` conversion. It adds no wire field.
[The telemetry layer](#124-decoding-and-encoding-telemetry-samples) covers it.

### `btp::session`

`btp::session` contains:

```text
session.cpp
```

It links `btp::messages` (for `MessageError`, `btp::negotiate` and the shared
enums) and, like it, needs no mbedtls and is always built. Same guarantees as
the rest of the library.

It has two members. `btp::DedupCache` is the bounded, caller-owned
command-deduplication cache of [Commands and discovery
§2.5–2.6](commands.md#25-command-deduplication): an executor feeds it each
`COMMAND_REQUEST` identity and it says whether to execute, replay a stored
`COMMAND_RESULT`, or reject a conflict — and it holds **no clock** (boot-scoped,
no time-based expiry). `btp::Session` is the responder state machine of
[Session and terminal §3–5](session-and-terminal.md#3-entering-protocol-mode-on-serial):
`HELLO` negotiation, `SESSION_CLOSE`, and the inactivity watchdog — it takes a
`now_ms` reading on every timed call, the same way `btp::Reassembler` does, but
reads no clock itself. [The session layer](#13-the-session-layer) covers both.

### `btp::endpoint`

`btp::endpoint` contains:

```text
endpoint.cpp
```

It links `btp::codec` for the frame codec and fragmentation and, like
`btp::messages`, needs no mbedtls and is always built. Same guarantees as the
rest of the library.

It is `btp::Endpoint`: the transmit side of a BTP endpoint — the local
`(source_id, boot_id)` identity, the outgoing sequence counter, and the
`seal → fragment → encode` pipeline every producer runs to put a logical
message on the wire. Sealing is a caller callback (no cryptographic
dependency), and an encoded frame is handed to a caller callback (no I/O). It
adds no wire field. [The endpoint layer](#14-the-endpoint-layer) covers it.

### `btp::receiver`

`btp::receiver` contains:

```text
receiver.cpp
```

It links `btp::codec` for `btp::decode()` and `btp::Reassembler` and, like
`btp::messages`, needs no mbedtls and is always built. Same guarantees as the
rest of the library.

It is `btp::Receiver`, the mirror of `btp::Endpoint`: `submit()` a received
datagram and it decodes the frame, checks the CRC, feeds fragments to a
reassembler, sweeps stale partials, and hands back a whole logical message
(copied into a caller buffer) — or nothing yet. The routing decision on top,
and opening an encrypted payload, stay the integration's.
[The receive layer](#15-the-receive-layer) covers it.

### `btp::aead`

`btp::aead` contains the optional authenticated-encryption implementation.

It depends on:

```text
mbedcrypto
```

and is controlled by:

```text
BTP_ENABLE_AEAD
```

Disabling AEAD removes the cryptographic dependency from the build.

Conceptually:

```text
                       btp::codec
                           |
       +-------------------+-------------------+
       |                   |                   |
     codec           fragmentation           stream
                           ^
       +-------------+------+------+
       |             |             |
 btp::messages  btp::endpoint  btp::receiver     btp::aead
       ^                                            |
+------+------+                                     v
|             |                                mbedcrypto
btp::telemetry  btp::session
```

`btp::messages`, `btp::endpoint` and `btp::receiver` each link `btp::codec`;
`btp::telemetry` and `btp::session` each link `btp::messages`; `btp::aead`
additionally links mbedcrypto. Encryption is therefore optional at build time,
and the basic frame codec requires neither the payload layer nor the
cryptographic component.

---

## 2. Guarantees

The reference library follows a set of rules intended to keep its behavior deterministic and suitable for constrained targets.

### 2.1 No internal allocation

The library does not dynamically allocate memory.

It does not internally call mechanisms equivalent to:

```text
new
malloc
realloc
```

for its working storage.

Instead, required buffers and state are supplied by the caller.

This includes:

* encoded-frame buffers;
* decoded-frame input buffers;
* COBS buffers;
* serial-decoder storage;
* reassembly slots;
* reassembly payload storage;
* encryption input and output buffers.

This does **not** mean that a BTP application is forbidden from using dynamic memory.

It means:

```text
BTP library
    |
    `-- does not allocate internally
```

The caller decides how its memory is obtained.

For example, an embedded application may use fixed storage:

```text
static buffers
fixed arrays
preallocated reassembly slots
```

while a desktop application may obtain its buffers differently.

The BTP wire representation is unaffected.

---

### 2.2 Runtime discovery does not change this rule

The manifest mechanism described in [Commands and discovery](commands.md#3-the-manifest) allows information to become known during execution.

For example, a consumer may discover:

```text
topic_id
schema_version
field descriptors
action descriptors
```

after receiving `MANIFEST_DATA`.

The current codec library does not automatically store that catalog.

There is no hidden internal manifest table.

Conceptually:

```text
MANIFEST_DATA
      |
      v
BTP decode / reassembly
      |
      v
logical payload
      |
      v
application
      |
      `-- application decides where
          descriptors are stored
```

An embedded application can therefore use fixed-capacity storage for discovered objects.

A desktop application can use a different storage model.

Runtime discovery means that the information becomes known at runtime.

It does not imply that the BTP library dynamically allocates memory for it.

---

### 2.3 No exceptions

Every public library operation is `noexcept`.

Errors are returned explicitly.

The library does not use exceptions to report protocol failures.

Conceptually:

```cpp
btp::Error rc = btp::decode(...);

if (rc != btp::Error::Ok) {
    // handle the returned error
}
```

The library does not:

* throw;
* log;
* terminate the application;
* abort the process.

The caller decides how an error is reported or handled.

---

### 2.4 No partial output on failure

A failed operation does not leave a partially valid result behind.

If an operation does not return success, output data and output parameters are not treated as completed output.

For example:

```text
encode()
   |
   +-- Ok ---------> complete BTP frame
   |
   `-- error ------> no partial frame
```

A failed encoder does not produce a half-written BTP packet that the caller should attempt to transmit.

Similarly, the caller must only consume decoded output after a successful decode.

---

### 2.5 Zero-copy decoding

`decode()` does not copy the BTP payload into another internal buffer.

The returned payload is a view into the input buffer supplied by the caller.

Conceptually:

```text
caller input buffer

+----------------------------------------------+
| BTP header | payload                         |
+----------------------------------------------+
             ^
             |
             +---- decoded.payload.data
```

This avoids an additional payload copy.

However, it creates an important lifetime rule:

> The decoded payload remains valid only while the original input buffer remains valid and unchanged.

For example:

```text
receive buffer
      |
      v
decode()
      |
      v
DecodedFrame.payload
      |
      v
view into receive buffer
```

If the receive buffer is overwritten with the next frame, the previous payload view must no longer be used.

The library does not retain a private copy.

---

### 2.6 In-place encoding

`encode()` supports a payload already located at the payload position of the destination frame.

For a BTP frame:

```text
header = 36 octets
```

so an application may prepare the payload starting at:

```text
output + 36
```

and then encode the frame around it.

Conceptually:

```text
before encode()

+----------------------------------------+
| free header area | prepared payload    |
+----------------------------------------+
0                  36


after encode()

+-----------------------------------------------+
| BTP header | payload | CRC                    |
+-----------------------------------------------+
0            36
```

The implementation handles overlap safely.

This can avoid another temporary payload buffer.

---

### 2.7 No clock dependency

The library does not obtain time directly from an operating system or hardware clock.

Operations that require time receive it from the caller.

For example, reassembly uses:

```text
now_ms
```

provided to the reassembler.

Conceptually:

```text
application clock
      |
      | now_ms
      v
Reassembler
```

The same reassembly implementation can therefore run:

```text
on embedded hardware
```

and:

```text
inside a deterministic test
```

without changing its time source.

---

## 3. The trap

A zero-initialized BTP `Header` is not automatically a valid unfragmented header.

For example:

```cpp
btp::Header header = {};
```

leaves:

```text
fragment_count = 0
```

but an unfragmented BTP message requires:

```text
FRAGMENTED     = 0
fragment_index = 0
fragment_count = 1
```

Therefore a minimal header needs at least the required message fields:

```cpp
btp::Header header = {};

header.type = btp::MessageType::Telemetry;
header.source_id = 1U;
header.boot_id = 1U;
header.fragment_count = 1U;
```

`source_id` and `boot_id` must be non-zero.

For an unfragmented frame:

```text
fragment_index = 0
fragment_count = 1
```

A header left with:

```text
fragment_count = 0
```

is rejected with:

```text
InvalidFragmentation
```

The complete fragmentation invariants are defined in [The datagram](frame.md#6-frame-validation).

This is one of the most common mistakes when creating the first frame manually.

---

## 4. Encoding and decoding

The basic codec operates on complete BTP frames.

### 4.1 Encoding

A frame consists of:

```text
Header
+
payload view
```

For example:

```cpp
std::size_t written = 0U;

const btp::Frame frame = {
    header,
    {payload, payload_size}
};

btp::Error rc =
    btp::encode(
        frame,
        btp::TransportProfile::EspNow,
        buffer,
        sizeof(buffer),
        &written
    );

if (rc != btp::Error::Ok) {
    // handle the error
}
```

On success:

```text
buffer[0 .. written)
```

contains one complete BTP frame:

```text
header || payload || CRC-32
```

The library serializes the header explicitly.

It does not transmit the memory representation of `btp::Header`.

---

### 4.2 Transport profile

`TransportProfile` selects the frame-size restrictions that apply to the operation.

The codec uses the profile to validate whether the frame can be represented on that transport.

It does not perform transport I/O.

For example:

```cpp
btp::TransportProfile::EspNow
```

means:

```text
validate using the ESP-NOW BTP limits
```

not:

```text
send using ESP-NOW
```

The distinction is:

```text
BTP

frame creation
frame validation
size limits


Application

radio API
UART API
USB API
actual transmission
```

There are no transport callback interfaces inside the codec.

---

### 4.3 Frame-size helpers

`encoded_size()` calculates the exact size required to encode a particular frame without writing it.

This can be used before choosing or validating an output buffer.

Conceptually:

```text
payload size
     |
     v
encoded_size()
     |
     v
exact frame size
```

For transport-wide limits, the library also provides:

```text
max_frame_size()
max_payload_size()
```

These describe the maximum physical frame and payload sizes associated with a transport profile.

They are useful for:

* receive-buffer sizing;
* checking whether fragmentation is required;
* validating transport integration.

---

### 4.4 Decoding

A complete received BTP frame is decoded with:

```cpp
btp::DecodedFrame decoded = {};

btp::Error rc =
    btp::decode(
        buffer,
        received_size,
        btp::TransportProfile::EspNow,
        &decoded
    );

if (rc == btp::Error::Ok) {
    // decoded.header
    // decoded.payload
}
```

The decoder validates the frame according to the rules defined in [The datagram](frame.md#6-frame-validation).

This includes checks such as:

```text
transport limits
magic
version
header size
payload size
total size
CRC
flags
source_id
boot_id
fragmentation invariants
```

Only after successful validation is the payload exposed.

---

### 4.5 Error reporting

Codec failures use `btp::Error`.

The textual helper:

```cpp
btp::error_string(rc)
```

returns a human-readable description of an error.

The enum value remains the programmatic result.

For example:

```text
CrcMismatch
InvalidMagic
InvalidFragmentation
PayloadTooLarge
UnsupportedVersion
```

remain distinct conditions.

The caller does not need to parse a diagnostic string to determine the failure type.

---

## 5. Fragmentation and reassembly

The fragmentation API operates on logical messages larger than one physical transport frame.

The sender performs:

```text
logical payload
      |
      v
fragment_count()
      |
      v
make_fragment()
      |
      v
encode each fragment
      |
      v
transport
```

The receiver performs:

```text
transport frame
      |
      v
decode()
      |
      v
Reassembler::push()
      |
      v
complete logical message
```

---

### 5.1 Determining fragment count

`fragment_count()` calculates how many BTP frames are required for a logical payload under the selected transport profile.

If the complete logical payload fits into one frame:

```text
fragment_count = 1
```

Otherwise:

```text
fragment_count >= 2
```

according to the transport payload limit.

---

### 5.2 Creating fragments

`make_fragment()` creates one BTP fragment at a time.

The fragment payload is a view over the corresponding region of the logical payload.

The library therefore does not need to copy the complete logical message into a separate fragmentation buffer.

Conceptually:

```text
logical payload

+----------+----------+----------+
| region 0 | region 1 | region 2 |
+----------+----------+----------+
     |          |          |
     v          v          v
 fragment 0  fragment 1  fragment 2
```

Each fragment can then be passed to `encode()`.

---

### 5.3 Caller-owned reassembly memory

Reassembly uses storage provided entirely by the caller.

For example:

```cpp
btp::ReassemblySlot slots[4];

std::uint8_t storage_bytes[4][2048];

btp::ReassemblyStorage storage[4] = {
    /* caller-provided storage descriptors */
};

btp::Reassembler reassembler(
    slots,
    storage,
    4U,
    2000U
);
```

In this example the caller has explicitly decided:

```text
maximum simultaneous slots = 4
```

and provided the corresponding storage.

The reassembler does not increase this capacity dynamically.

If all usable slots are occupied, the library reports the corresponding capacity condition instead of allocating another one.

This is the practical meaning of bounded memory in the reassembly layer.

---

### 5.4 Configuration validation

After construction:

```cpp
reassembler.valid()
```

reports whether the supplied slots and storage form a valid reassembly configuration.

This should be checked before normal message processing begins.

An invalid configuration is not repaired by allocating additional memory.

---

### 5.5 Receiving fragments

A decoded frame is inserted with:

```cpp
btp::ReassembledMessage message = {};

btp::ReassemblyEvent event =
    reassembler.push(
        decoded,
        now_ms,
        &message
    );
```

The event reports the state resulting from the received fragment.

When:

```cpp
event == btp::ReassemblyEvent::Complete
```

the complete logical message is available through:

```cpp
message
```

Conceptually:

```text
fragment 0
    |
fragment 2
    |
fragment 1
    |
    v
Reassembler
    |
    v
Complete
    |
    v
logical payload
```

Fragments do not need to arrive in index order.

The reassembler applies the BTP fragmentation consistency rules before completing the message.

---

### 5.6 Releasing completed messages

A completed reassembly retains its slot.

This keeps the payload view stable while the caller processes the logical message.

After processing it, the caller must release the slot:

```cpp
reassembler.release(message.slot_index);
```

The lifecycle is:

```text
slot free
   |
   v
collecting fragments
   |
   v
complete
   |
   | caller uses message
   |
   v
release()
   |
   v
slot free
```

If completed slots are never released, the reassembler eventually has no free slot available.

The resulting problem is not dynamic-memory exhaustion.

It is exhaustion of the fixed capacity intentionally supplied by the caller.

---

### 5.7 Expiration

Incomplete reassemblies do not remain active indefinitely.

The caller supplies:

```text
now_ms
```

and the configured timeout determines when stale slots expire.

The application may explicitly sweep expired state with:

```cpp
reassembler.expire(now_ms);
```

which returns the number of slots released.

To reset all reassembly state:

```cpp
reassembler.clear();
```

may be used.

---

## 6. COBS and the serial decoder

The serial transport uses COBS framing as defined in [Getting it across the link](fragmentation-and-transports.md).

The library provides both direct COBS functions and an incremental serial decoder.

---

### 6.1 COBS encoding

The direct helpers are:

```text
cobs_encode()
cobs_decode()
```

They operate on caller-provided buffers.

Input and output buffers for these functions must not overlap.

The helper:

```text
cobs_max_encoded_size()
```

returns the worst-case encoded size for a given input size.

This allows the caller to provision the output buffer before encoding.

---

### 6.2 Incremental serial decoding

A UART commonly delivers bytes as a stream rather than one complete BTP frame at a time.

`SerialDecoder` handles that stream incrementally.

For example:

```cpp
std::uint8_t encoded[btp::kSerialMaxCobsBlockSize];
std::uint8_t decoded_buffer[btp::kSerialMaxFrameSize];

btp::SerialDecoder decoder(
    encoded,
    sizeof(encoded),
    decoded_buffer,
    sizeof(decoded_buffer)
);

btp::DecodedFrame frame = {};

btp::SerialDecodeResult result =
    decoder.push(byte, &frame);
```

The application repeatedly feeds received bytes:

```text
byte
byte
byte
byte
...
```

and receives an event when the delimiter completes a COBS block.

---

### 6.3 Serial decoder events

The decoder reports:

```text
None
Frame
CobsError
FrameError
Overflow
InvalidConfiguration
```

`Frame` means that a complete BTP frame was successfully decoded.

For:

```text
FrameError
```

the result also contains the corresponding BTP frame error.

This keeps errors such as:

```text
bad CRC
bad magic
invalid fragmentation
```

distinguishable from a COBS framing error.

---

### 6.4 Initial synchronization

The serial decoder starts unsynchronized.

Input bytes are discarded until the first COBS delimiter is observed.

After synchronization, the following delimited block becomes a candidate frame.

Conceptually:

```text
unknown stream position

xx xx xx xx xx
            |
            v
        delimiter
            |
            v
       synchronized
            |
            v
next COBS block
            |
            v
       BTP decode
```

This prevents startup in the middle of an existing encoded block from being interpreted as a complete frame.

---

### 6.5 Why this decoder is serial-only

`SerialDecoder` exists because serial communication presents a byte stream.

ESP-NOW and USB HID already provide complete transport messages to the application.

Their integration therefore normally begins with:

```text
complete received buffer
        |
        v
btp::decode()
```

rather than a byte-by-byte BTP decoder.

---

## 7. Encryption

Authenticated encryption is provided by the optional:

```text
btp::aead
```

component.

The codec itself does not own encryption keys and does not automatically encrypt or decrypt payloads.

Cryptographic processing surrounds the normal BTP codec.

The sender order is:

```text
plaintext logical payload
        |
        v
AEAD seal
        |
        v
ciphertext || tag
        |
        v
fragmentation
        |
        v
BTP encode
```

The receiver performs:

```text
BTP decode
     |
     v
reassembly
     |
     v
ciphertext || tag
     |
     v
AEAD open
     |
     v
plaintext
```

This is the same order defined in [Authenticated encryption](encryption.md).

---

### 7.1 Sealing

A key is represented using:

```cpp
const btp::AeadKey key = {
    key_bytes,
    sizeof(key_bytes)
};
```

The logical message can then be sealed with:

```cpp
btp::AeadError rc =
    btp::aead_seal(
        key,
        logical_header,
        plaintext_size,
        plaintext,
        ciphertext_and_tag
    );
```

The output contains:

```text
ciphertext || 16-octet tag
```

The selected cipher is obtained from `CIPHER_ID` in the logical header.

---

### 7.2 Header serialization and AAD

BTP authenticated encryption uses the canonical logical header as associated data.

Because encryption occurs before physical-frame encoding, the library exposes header serialization separately through:

```text
encode_header()
```

The caller therefore does not need to encode a complete physical frame merely to obtain the header bytes required by the AEAD operation.

The canonicalization rules remain those defined by the encryption specification.

---

### 7.3 Opening

After complete encrypted-message reassembly:

```cpp
rc = btp::aead_open(
    key,
    message.header,
    static_cast<std::uint16_t>(message.payload.size),
    message.payload.data,
    plaintext
);
```

Authentication is performed before the plaintext is accepted.

If the operation returns:

```text
AeadError::TagMismatch
```

authentication failed.

The message must not be treated as valid plaintext.

---

### 7.4 Cipher dispatch

The generic:

```text
aead_seal()
aead_open()
```

functions dispatch according to the `CIPHER_ID` encoded in the header.

The library also exposes cipher-specific operations.

`aead_nonce()` derives the 12-octet BTP nonce from:

```text
source_id
boot_id
sequence
```

when direct access to the nonce representation is required.

The cryptographic wire rules remain independent of the backend used to perform those operations.

---

## 8. Build and test

The reference implementation uses CMake.

A normal build is:

```bash
cmake -S . -B build
cmake --build build
```

Tests are executed with:

```bash
ctest --test-dir build --output-on-failure
```

---

### 8.1 Build options

The main options are:

| Option                   | Default              | Effect                                |
| ------------------------ | -------------------- | ------------------------------------- |
| `BTP_ENABLE_AEAD`        | `ON`                 | Builds `btp::aead`                    |
| `BTP_BUILD_TESTS`        | Top-level build only | Builds the test suites                |
| `BTP_INSTALL`            | Top-level build only | Generates install rules               |
| `BTP_USE_SYSTEM_MBEDTLS` | `OFF`                | Uses an existing mbedTLS installation |

Disabling:

```text
BTP_ENABLE_AEAD
```

allows applications that do not use BTP encryption to build the codec without the crypto dependency.

`btp::codec`, `btp::messages`, `btp::telemetry`, `btp::session`, `btp::endpoint`
and `btp::receiver` have no build option: they have no external dependency and
are always built.

---

### 8.2 Installed package

The library may be installed with:

```bash
cmake -S . -B build
cmake --build build
cmake --install build --prefix /usr/local
```

A consuming CMake project may then use:

```cmake
find_package(btp 2.0 REQUIRED)

target_link_libraries(
    app
    PRIVATE
    btp::codec
)
```

Applications requiring the available AEAD target may additionally link it according to the selected build configuration.

---

### 8.3 Subproject

BTP can also be included as part of another CMake project through mechanisms such as:

```text
add_subdirectory()
```

or:

```text
FetchContent
```

When consumed as a subproject, BTP does not need to build its own tests or install rules unless explicitly required by the parent project.

If the parent project already defines the required:

```text
mbedcrypto
```

target, the BTP AEAD component can use that dependency rather than creating a second copy.

---

### 8.4 PlatformIO

The repository also contains:

```text
library.json
```

for PlatformIO integration.

The library layout uses:

```text
include/
src/
```

directly.

PlatformIO does not expose the same CMake:

```text
BTP_ENABLE_AEAD
```

option.

For that environment, the AEAD translation unit checks whether the mbedTLS headers required for the cryptographic implementation are available.

On platforms such as ESP32, where the SDK provides the required mbedTLS components, the AEAD implementation can be compiled.

On a target without those headers, the optional cryptographic implementation does not need to make the core BTP codec unusable.

The embedded compilation test is located under:

```text
tests/embedded
```

and can be built with:

```bash
pio run -d tests/embedded
```

---

## 9. Conformance vectors

The BTP repository contains machine-readable conformance vectors for the supported wire versions.

They are located under:

```text
test-vectors/v1/            frame vectors, wire v1
test-vectors/v2/            frame vectors, wire v2 (AEAD)
test-vectors/v2/messages/   payload vectors for btp::messages
test-vectors/v2/telemetry/  TELEMETRY sample vectors for btp::telemetry
```

The frame vectors are a whole datagram with an opaque payload; the message and
telemetry vectors are a logical payload with no frame around it (a telemetry
vector also carries the schema it is decoded against). They are checked by
`tools/test_vectors.py`, `tools/test_vectors_v2.py`, `tools/test_messages.py`
and `tools/test_telemetry.py` respectively -- four independent reference
implementations, so a bug in one and a bug in the C++ cannot hide each other.
See [the message-vector README](../test-vectors/v2/messages/README.md).

The vectors complement the written specification.

A vector describes:

```text
input fields
expected bytes
expected result
```

and is associated with the corresponding binary representation.

The directories separate:

```text
valid/
invalid/
```

cases.

---

### 9.1 Valid vectors

A valid vector defines a BTP object that must produce an exact wire representation.

A conforming implementation must be able to:

```text
description
    |
    v
encode
    |
    v
exact expected bytes
```

and:

```text
expected bytes
    |
    v
decode
    |
    v
expected fields
```

Wire compatibility is byte exact.

Producing a semantically similar but differently encoded frame is not conforming.

---

### 9.2 Invalid vectors

Invalid vectors also define the expected BTP error.

For example, it is not sufficient for a decoder to say:

```text
rejected
```

if the vector requires:

```text
CrcMismatch
```

or:

```text
InvalidFragmentation
```

The rejection reason is part of the conformance test because BTP defines validation order.

For example, a frame with an intentionally invalid semantic field but an invalid CRC must fail CRC validation first if the protocol specifies CRC validation before that semantic check.

---

### 9.3 Verification

Version-1 vectors are verified with:

```bash
python tools/test_vectors.py \
    --root test-vectors/v1 \
    --check
```

Version-2 vectors are verified with:

```bash
python tools/test_vectors_v2.py \
    --root test-vectors/v2 \
    --check
```

Without:

```text
--check
```

the tools regenerate the binary files from their vector descriptions.

---

### 9.4 AEAD vectors

The version-2 vector set also contains authenticated-encryption cases.

These publish the information required to independently reproduce the encrypted result, including values such as:

```text
key
nonce
AAD
plaintext
ciphertext
authentication tag
```

Fragmented encrypted vectors verify the required order:

```text
encrypt complete logical message
        |
        v
fragment encrypted payload
```

rather than encrypting individual fragments independently.

---

### 9.5 Minimum conformance

A new BTP implementation should at minimum be able to:

1. encode every valid vector to the expected bytes;
2. decode every valid vector successfully;
3. reject every invalid vector with the expected error;
4. fragment and reassemble according to the protocol;
5. reproduce the relevant AEAD vectors when encryption is implemented.

The conformance vectors therefore represent executable examples of the wire contract.

Changing their expected wire bytes is not merely changing a unit test.

It changes the protocol representation being tested.

---

## 10. Versioning and branches

The specification, reference implementation and conformance vectors are maintained together.

The project uses semantic versioning for the library release line.

The intended interpretation is:

```text
MAJOR
    incompatible wire change

MINOR
    backward-compatible addition

PATCH
    correction without changing
    the established wire representation
```

---

### 10.1 Wire version and library version are different

Several different concepts contain version numbers.

They must not be confused.

| Concept            | Example        | Meaning                                 |
| ------------------ | -------------- | --------------------------------------- |
| Wire version       | `0x01`, `0x02` | Value stored in the BTP header          |
| Library version    | `2.5.0`        | Version of the reference implementation |
| Release tag        | `v1.1.0-beta`  | Repository release                      |
| Maintenance branch | `1.x`          | Source branch maintaining a major line  |

The wire version is transmitted inside every BTP frame.

The library version is not.

For example:

```text
library 2.5.0
```

and:

```text
wire 0x02
```

are related project versions but are not the same protocol field.

A document should therefore prefer explicit wording such as:

```text
BTP wire version 2
```

or:

```text
BTP library 2.5.0
```

rather than an ambiguous:

```text
BTP v2
```

when the distinction matters.

---

### 10.2 Branch layout

The current branch model keeps the newest major line on:

```text
main
```

and retains previous major lines in maintenance branches.

For example:

| Branch | Wire                       |
| ------ | -------------------------- |
| `main` | current wire line          |
| `1.x`  | wire version 1 maintenance |

When a future incompatible wire version is introduced, the previous major line can be preserved on its own maintenance branch.

This allows an existing deployment to remain pinned to the implementation corresponding to the wire contract it uses.

---

### 10.3 Protocol changes and vectors

An incompatible wire change requires coordinated updates to:

```text
specification
library
conformance vectors
```

These artifacts must continue to describe the same bytes.

Conceptually:

```text
Specification
      |
      v
expected wire format
      ^
      |
Reference library
      ^
      |
Conformance vectors
```

If one of them describes different bytes from the others, the implementation line is inconsistent.

---

## 11. Known limitations and implementation boundary

The library implements the wire -- the frame and, with `btp::messages`, the
byte layout of every `COMMAND` and `CONTROL` payload. It intentionally stops
below the mechanisms that carry *state*. This section marks that line, so a
mechanism the specification defines is not mistaken for one the library runs
on its own.

---

### 11.1 The session state machine

`btp::messages` encodes and decodes:

```text
HELLO / HELLO_RESULT
SESSION_CLOSE / SESSION_CLOSE_RESULT
```

and `btp::negotiate()` computes the effective limits from two `HELLO`s
(the peer-to-peer minimum; a gateway clamps further for its path).

`btp::Session` ([§13.5](#135-the-session-state-machine-btpsession)) runs the
**responder** half on top of that: the `AwaitingHello` → `Active` lifetime, the
inactivity watchdog and the fall back to `Idle` on close, timeout or transport
loss. It does **not** run the **initiator** half — sending `ENTER` / `HELLO`,
the retry budget and awaiting `HELLO_RESULT` are a desktop tool's, and so is
the plain-ASCII `console`↔`protocol` text on serial and the priority scheduler.
`btp::Endpoint` ([§14](#14-the-endpoint-layer)) holds the transmit-side
identity and sequence counter a session's replies ride on, but not the
negotiation that decides *when* that identity may send. Command deduplication
is separate again — `btp::DedupCache` is boot-scoped precisely because [it
outlives a session](session-and-terminal.md#53-session-loss-and-command-deduplication).

---

### 11.2 The manifest catalogue is not stored

`btp::messages` provides `ManifestReader` / `ManifestWriter` for the byte
layout of `MANIFEST_DATA` -- the header, the `source_info` block, and every
topic, field, action and enum record, walked without allocation.

It does **not** keep a catalogue of what it read. Which schemas a consumer
caches, and for how long against `config_revision`, is the integration's
decision -- fixed-capacity tables on a microcontroller, dynamic containers on
a desktop. Runtime discovery means the information becomes known at runtime,
not that the library holds a database.

---

### 11.3 Telemetry schema *storage* is above the payload layer

`btp::messages` decodes a topic's `FieldRecord`s -- the type, unit, scale,
offset and enum labels that *describe* a schema. `btp::telemetry` decodes and
encodes a `TELEMETRY` sample *against* that schema
([§12.4](#124-decoding-and-encoding-telemetry-samples)).

```text
MANIFEST_DATA  --btp::messages---->  FieldRecord   --field_spec()-->  FieldSpec table
TELEMETRY      --btp::decode()----->  logical payload
                                          |  + FieldSpec table
                                          v
                                 btp::telemetry::SampleReader  -->  values
```

What is still the integration's:

* **schema storage.** Which schemas a consumer caches, in what structure and
  for how long against `config_revision`, is its decision -- the same as any
  other discovered state (§11.2). `btp::telemetry` takes a `FieldSpec` array
  wherever it came from.
* **`JSON_UTF8` / `CSV_UTF8`.** Parsing the text body needs an allocator;
  `SampleReader::body()` returns the raw bytes and the caller parses.
* **the enum-label lookup.** `SampleValue` hands back the raw integer; whether
  it names a known label is a lookup against the field's enum entries, which
  the caller holds.

---

### 11.4 Command *execution* is above the payload layer

`btp::messages` encodes and decodes `COMMAND_REQUEST` / `COMMAND_RESULT`,
including the request reference that correlates a result to its request.
`btp::DedupCache` ([§13](#13-the-session-layer)) keeps the bounded
`(request_source_id, request_boot_id, request_sequence)` cache and applies the
retransmission and conflict rules of
[Commands and discovery §2.5–2.6](commands.md#25-command-deduplication).

What is still the integration's: **executing the action**, **sealing and
sending** the `COMMAND_RESULT`, and **choosing the capacities** — the cache
takes a caller-owned slot array, one storage region per slot and a requester
table, and never allocates.

---

### 11.5 The priority scheduler is not implemented

`btp::messages` encodes and decodes `SUBSCRIBE` / `SUBSCRIBE_RESULT` /
`UNSUBSCRIBE` / `UNSUBSCRIBE_RESULT` and `STATUS` (versions 1 and 2), and
`priority_class()` ([§12.1](#121-what-it-covers)) says which of the six spec
classes a message belongs to.

It does **not** own a telemetry scheduler, a subscription aggregator, or the
priority queue itself. Those decide *when* an outgoing message is sent, how
many queues to keep, and how to drain them -- inherently transport-specific
(FreeRTOS queues on firmware, an event loop on a desktop), and
"implementation-dependent" by the spec.

---

### 11.6 Reassembly uses a transport-independent internal bound

The reassembler accepts frames only after they have already passed transport-specific validation through `decode()`.

Internally, the current implementation uses the serial payload ceiling as its fragment-storage backstop rather than binding each `Reassembler` instance to one transport profile.

This also allows a gateway to:

```text
receive on one transport
        |
        v
reassemble logical message
        |
        v
fragment for another transport
```

without requiring a different reassembler type for each input path.

Transport-specific frame-size validation still occurs before a fragment reaches reassembly.

---

### 11.7 Incremental decoding is serial-specific

The library exposes an incremental decoder for serial because serial provides an arbitrary byte stream.

There is no equivalent incremental decoder for:

```text
ESP-NOW
USB HID
```

because those transport APIs provide complete transport messages to the caller.

Their BTP integration normally calls:

```text
decode()
```

directly on the complete received message.

---

## 12. The message layer

`btp::messages` turns the byte layout of every `COMMAND` and `CONTROL` payload
-- the tables in [Commands and discovery](commands.md) and
[Session and terminal](session-and-terminal.md) -- into a C++ struct, and
back. It sits directly above `btp::codec`: `btp::decode()` gives you a logical
payload after reassembly, and `btp::messages` gives that payload a shape.

It adds no wire field. Every layout it serialises is already specified; this
is the specification's struct ⇄ bytes half written once, so a consumer does
not re-derive an offset table from the prose.

The `TELEMETRY` sample body is the one payload `btp::messages` does not carry:
it needs a schema in hand. `btp::telemetry` covers it, from the same schema
`btp::messages` decodes ([§12.4](#124-decoding-and-encoding-telemetry-samples)).

---

### 12.1 What it covers

| Object | API |
| --- | --- |
| `HELLO` / `HELLO_RESULT` | `decode_hello` / `encode_hello`, `decode_hello_result` / `encode_hello_result` |
| `SESSION_CLOSE` (`_RESULT`) | `decode_session_close` / …, `decode_session_close_result` / … |
| `COMMAND_REQUEST` / `COMMAND_RESULT` | `decode_command_request` / …, `decode_command_result` / … |
| `MANIFEST_REQUEST` | `decode_manifest_request` / `encode_manifest_request` |
| `MANIFEST_DATA` | `ManifestReader` / `ManifestWriter` (§12.3) |
| `SUBSCRIBE` (`_RESULT`), `UNSUBSCRIBE` (`_RESULT`) | `decode_subscribe` / …, `decode_unsubscribe_result` / … |
| `STATUS` v1 and v2 | `decode_status`, `status_topic_count`, `encode_status_v1` / `encode_status_v2` |
| `HELLO` negotiation | `negotiate(local, remote)` -> `EffectiveLimits` |
| Traffic priority | `priority_class(type, object_id, status_degraded)` -> `PriorityClass` |

`TERMINAL_IN` / `TERMINAL_OUT` have no decode entry: their payload is opaque
bytes with no structure ([Session and terminal](session-and-terminal.md#7-the-terminal)).
`is_message_object(type, object_id)` reports whether a `(type, object_id)`
pair names a payload this layer decodes -- a router uses it to tell a message
it should decode from a frame it only relays.

`priority_class(type, object_id, status_degraded = false)` returns which of the
six spec priority classes ([Model §8](model.md#8-traffic-priority),
[Session and terminal §8](session-and-terminal.md#8-priority)) a message falls
in -- `Session` (1) down to `Telemetry` (6), 1 highest. It is the
classification only: how many send queues an integration keeps and how it
drains them stays "implementation-dependent" (a dongle that never originates
`COMMAND_REQUEST` folds classes 1 and 2 into one queue; a robot keeps all
six). The `DEGRADED` bit lives in the `STATUS` payload, which the call does
not see, so `status_degraded` is passed in to lift a `STATUS` message from
`Bulk` (5) to `Subscription` (3). An unrecognised pair is classed `Telemetry`:
lowest priority, dropped first, never ahead of real control traffic.

---

### 12.2 The contract

Every `decode_*` / `encode_*` follows the same rules as `btp::codec`:

* **no allocation** -- `MANIFEST_DATA` is walked record by record, never
  decoded into one growing structure;
* **`noexcept`** -- errors are returned as `btp::MessageError`, a separate
  enum from `btp::Error` because a truncated `utf8_u16` inside a payload and a
  bad envelope CRC are failures at different layers;
* **zero-copy decode** -- a `ByteView` in an out-struct points into the
  payload buffer and is valid only while that buffer is;
* **no partial output on failure** -- on any non-`Ok` return, the out-struct
  and `*written` are not to be read.

`decode_*` validates in the order [Commands and discovery](commands.md#7-validation)
defines: the fixed portion, reserved fields and flag bits, every declared
length before the variable data it introduces, identifiers and versions and
counts, nested records within their bounds, the protocol limits of
[section 6](commands.md#6-limits), and finally exact consumption of the
payload. `encode_*` applies the same field rules, so a struct that would not
decode does not encode.

The layer validates each field against its own domain (`role`, a result
`status`, a close `reason`) but does not check cross-field or cross-record
semantics -- that a successful `HELLO_RESULT` implies non-zero limits, that a
`(source_id, topic_id)` pair does not repeat within one `STATUS`. Those
belong to the session and subscription layers above.

---

### 12.3 Reading and writing `MANIFEST_DATA`

A manifest can carry 1024 topics and 1024 actions, each with up to 256
fields, so it is walked, not returned whole. `ManifestReader` stays shallow --
header, `source_info`, topics, actions -- and hands each topic and action back
with the **raw bytes** of its nested records, which sub-readers walk:

```cpp
btp::ManifestReader r(payload, size);
btp::ManifestHeader h;
r.header(&h);

btp::SourceInfoEntry si;                              // format 2 only
while (r.next_source_info(&si) == btp::ManifestStep::Item) { /* ... */ }

btp::TopicRecord t;
btp::ByteView field_bytes;
while (r.next_topic(&t, &field_bytes) == btp::ManifestStep::Item) {
    btp::FieldRecordReader fr(field_bytes, t.field_count);
    btp::FieldRecord f;
    btp::ByteView enum_bytes;
    while (fr.next(&f, &enum_bytes) == btp::ManifestStep::Item) {
        btp::EnumEntryReader er(enum_bytes, f.enum_count);
        btp::EnumEntry e;
        while (er.next(&e) == btp::ManifestStep::Item) { /* ... */ }
    }
}

btp::ActionRecord a;
btp::ByteView params, results, errors;
while (r.next_action(&a, &params, &results, &errors) == btp::ManifestStep::Item) {
    // FieldRecordReader over params and results, ActionErrorReader over errors
}

r.finish();   // requires the whole payload to have been consumed
```

Nesting is explicit in the types -- a `FieldRecordReader` over a topic's
fields is a different object from one over an action's parameters -- never in
hidden reader state. `ManifestWriter` mirrors this: `begin`, `add_source_info`,
`begin_topic` / `add_field` / `add_enum` / `end_topic`, `begin_action` /
`add_action_param` / `add_action_result` / `add_action_error` / `end_action`,
`finish`. It backpatches every `record_size`, `info_count`, `field_count`,
`enum_count` and `error_count`, and `finish` checks the topic and action
counts against the header.

#### Verbatim relay

A cache that stores a source's catalog and re-emits it unchanged -- a gateway
that forwards manifests it never inspects -- has no use for the decoded
records. For that case `ManifestReader` hands back the still-framed spans
directly, and `ManifestWriter` splices them back without decomposing:

```cpp
btp::ManifestReader r(payload, size);
btp::ManifestHeader h;
r.header(&h);
btp::ByteView info, topics, actions;
r.raw_source_info(&info);          // the source_info block, info_count included
r.raw_records(&topics, &actions);  // the topic- and action-record runs

btp::ManifestWriter w(out, capacity);
w.begin(h);                        // h.topic_count / h.action_count are bounds
w.put_raw_source_info(info);       // format 2 only
w.put_raw_records(topics, actions);
w.finish(&written);
```

`raw_source_info` / `raw_records` are an alternative to the `next_*` walk,
called right after `header()` on their own reader. `put_raw_records` copies as
many leading whole records as `capacity` holds -- never a partial record --
and backpatches `topic_count` / `action_count` to what actually landed, so a
relay serving a catalog into a buffer smaller than the cache emits a
consistent short manifest rather than failing. `record_size` framing is
re-validated on both sides; record contents are trusted.

---

### 12.4 Decoding and encoding telemetry samples

`btp::telemetry` (`btp/telemetry.hpp`, target `btp::telemetry`) is the sample
half of [Telemetry payloads](telemetry.md): a `TELEMETRY` logical payload
(`schema_version` + `PACKED_LE` / `TLV_LE` body) against a schema the caller
holds. `btp::messages` gave the schema; this reads and writes a sample.

The schema is a `FieldSpec` array -- a lean subset of `FieldRecord` (the codec
never touches `name` / `unit` / enum entries). A producer fills a static
`FieldSpec[]`; a consumer converts each walked `FieldRecord` with
`field_spec()` and keeps the array in its own cache. `order` must be
contiguous from zero, in array order.

```cpp
// schema (from the manifest, or compiled in)
btp::FieldSpec fields[] = {
    { 1, 0, uint8_t(btp::WireType::Float32), 0, 1, 0, 1.0,  0.0 },
    { 2, 1, uint8_t(btp::WireType::Int16),   0, 1, 0, 0.01, 0.0 },
};

// decode
btp::SampleReader r(payload, size, fields, 2, btp::kEncodingPackedLe);
btp::SampleValue v;
while (r.next(&v) == btp::SampleStep::Item) {
    if (v.is_null) continue;
    double eng = v.f64(0);        // raw * scale + offset (raw for bool / enum)
    std::int64_t raw = v.i64(0);  // the wire integer, unscaled
}
if (r.finish() != btp::MessageError::Ok) { /* reject the whole sample */ }

// encode (PACKED_LE only)
btp::SampleWriter w(out, capacity, fields, 2);
w.begin(schema_version);
w.put_f64(12.5);                  // one call per field, in `order`
w.put_f64(3.14);                  // Int16 scale 0.01 -> raw 314
std::size_t written = 0;
w.finish(&written);
```

`next()` yields every schema field exactly once, in `order`, for both
encodings -- a `TLV_LE` field omitted on the wire still comes back as an
`is_null` item. `SampleValue` copies nothing: `f64(i)` / `i64(i)` / `u64(i)`
read element `i` straight from the payload. A structural fault
([telemetry.md §14.4](telemetry.md#144-structural-errors)) fails the whole
sample; there is no partial decode.

`SampleWriter` emits `PACKED_LE` only: `TLV_LE` entries go in `field_id`
order while a caller writes in schema order, which would need buffering.
`put_f64` takes the engineering value and applies the inverse conversion;
`put_i64` / `put_u64` take the raw wire value; `put_null` marks a nullable
field absent. `begin()` reserves the presence bitmap and `finish()` requires
every field to have been written.

For a text encoding (`OPAQUE_BYTES`, `UTF8`, `JSON_UTF8`, `CSV_UTF8`)
`next()` returns `End` and `body()` hands back the whole `encoded_body`; the
caller parses the text.

A consumer must read `schema_version` to pick the schema before it can decode
anything, so a router usually has it already. Passing
`btp::SampleLayout::BodyOnly` to the `SampleReader` constructor (and to
`SampleWriter::begin`) makes the buffer the `encoded_body` alone, with no
two-octet prefix to re-supply; `schema_version()` then returns 0.

---

## 13. The session layer

`btp::session` (`btp/session.hpp`, target `btp::session`) holds the stateful
session machinery that `btp::messages` deliberately leaves out. Its first
member is the one whose absence is a safety hazard rather than a parsing
inconvenience: **`btp::DedupCache`**, the command-deduplication cache of
[Commands and discovery §2.5–2.6](commands.md#25-command-deduplication) and
[Session and terminal §5.3](session-and-terminal.md#53-session-loss-and-command-deduplication).

The problem it removes: a requester sends `COMMAND_REQUEST` "fire", the
`COMMAND_RESULT` is lost, the requester retransmits the identical request. An
executor that does not remember it fires twice. The rule is that the executor
keeps the request identity
`(request_source_id, request_boot_id, request_sequence)` and the
`COMMAND_RESULT` it produced, replays that result on a retransmission, and
rejects a *different* request that reuses the identity.

### 13.1 The contract

Same guarantees as the rest of the library, plus **no clock**: the cache is
scoped to the executor boot with no time-based expiry
([commands.md §2.6](commands.md#26-deduplication-capacity)), so it needs no
time source (unlike `btp::Reassembler`). It never allocates — the caller owns:

* a `btp::DedupSlot` array — one entry per tracked command;
* a `btp::DedupStorage` region per slot — holds the request verbatim, then the
  `COMMAND_RESULT` after it;
* a `btp::DedupRequester` table — one row per `(source_id, boot_id)` device,
  carrying the sequence high-water marks that make eviction safe.

### 13.2 Using it

```cpp
btp::DedupSlot slots[16];
std::uint8_t bytes[16][768];
btp::DedupStorage storage[16];
for (std::size_t i = 0; i < 16; ++i) storage[i] = {bytes[i], sizeof(bytes[i])};
btp::DedupRequester requesters[4];
btp::DedupCache cache(slots, storage, 16, requesters, 4);

// on a COMMAND_REQUEST (payload is the reassembled, opened logical request):
btp::DedupKey key{header.source_id, header.boot_id, header.sequence};
std::size_t slot = 0;
btp::ByteView stored{};
switch (cache.classify(key, payload.data, payload.size, &slot, &stored)) {
    case btp::DedupVerdict::Fresh:
        run_action(payload);                       // execute exactly once
        encode_command_result(/* ... */ result, &n);
        cache.record_result(slot, result, n);
        send(result, n);
        break;
    case btp::DedupVerdict::DuplicateComplete:     // replay, do not execute
        send(stored.data, stored.size);
        break;
    case btp::DedupVerdict::DuplicateInFlight:     // first copy still running
        break;                                     // drop; the peer will retry
    case btp::DedupVerdict::Conflict:              // same identity, other bytes
        send_reject(REJECTED, REQUEST_CONFLICT);
        break;
    case btp::DedupVerdict::Evicted:               // handled, result aged out
    case btp::DedupVerdict::CapacityExhausted:
        send_reject(BUSY, CAPACITY_EXHAUSTED);
        break;
    case btp::DedupVerdict::InvalidArgument:
        break;
}
```

`record_result` turns a `Fresh` slot into a completed one; `release` frees a
`Fresh` slot whose execution was abandoned; `clear` drops everything (test
isolation — a running executor never calls it).

### 13.3 The ring and the high-water mark

The cache is bounded, so a fresh identity arriving with every slot full evicts
the **oldest completed** entry — the executor keeps working past its slot count
rather than wedging. Eviction is made safe by the requester table: each row
remembers the highest sequence **evicted** for that device, and a later lookup
whose sequence is at or below that mark, and which is no longer in a slot,
returns `Evicted` — the caller answers `BUSY / CAPACITY_EXHAUSTED` and **never
re-executes**. A well-behaved requester's sequence only increases, so this
never misfires on a genuinely new command; a replayed old sequence is
conservatively rejected, which is the safe direction for command traffic.

A new `boot_id` from a known `source_id` reuses that device's row (its old boot
is gone, and [a command for a stale boot is rejected
anyway](commands.md#22-target-boot-validation)). A requester table full of
*distinct* devices returns `CapacityExhausted` for a new one rather than risk
an unprotected identity.

### 13.4 What stays out of the cache

Executing the action and sealing/sending the `COMMAND_RESULT` are the
integration's, as [§11.4](#114-command-execution-is-above-the-payload-layer)
says.

---

### 13.5 The session state machine (`btp::Session`)

`btp::messages` already turns `HELLO` / `HELLO_RESULT` / `SESSION_CLOSE` /
`SESSION_CLOSE_RESULT` into structs and back and computes the effective limits
([`btp::negotiate`](#111-the-session-state-machine)). It does not *run* the
session: the lifetime, the inactivity watchdog ([Session and terminal
§5](session-and-terminal.md#5-the-watchdog)) and the `console`↔`protocol`
transition on serial were the integration's to keep, hand-rolled once per
consumer.

`btp::Session` is the **responder** half of that state machine —
the peer that *receives* `HELLO`, not the desktop tool that sends it.

```text
Idle  --arm(now)-->  AwaitingHello  --valid HELLO-->  Active
  ^                        |                            |
  |  HELLO deadline,       |  no common version,        |  SESSION_CLOSE,
  |  reject, reset()       |  malformed HELLO           |  watchdog, reset()
  +------------------------+----------------------------+
```

Same guarantees as the rest of the library, plus the one `btp::Reassembler`
also has: **it takes a clock reading, it does not read a clock.** Every call
that cares about time takes a `now_ms` the caller fills from its own monotonic
millisecond source — `millis()` or `esp_timer_get_time() / 1000` on an MCU,
`QElapsedTimer::elapsed()` under Qt, a plain counter in a test. `btp::Session`
only ever computes `now_ms >= deadline`; it never includes `<ctime>`.

```cpp
btp::Hello local{};
local.role = static_cast<std::uint8_t>(btp::Role::Producer);
local.version_count = 1; local.versions[0] = 1;
local.max_logical_payload = 2048;
local.max_inflight_reassemblies = 1;
local.max_subscriptions = 8;
local.max_dedup_entries = 32;
local.session_timeout_ms = 30000;
std::memcpy(local.peer_uuid, my_uuid, 16);
local.config_revision = manifest_revision;

btp::Session session(local, /*hello_deadline_ms=*/2000);  // 2000: serial §5.1

session.arm(now_ms());                    // after answering the console ENTER line

// per decoded frame, from btp::decode() / btp::Receiver:
std::uint8_t reply[btp::kSessionMaxReplySize];
btp::SessionOutcome o = session.on_frame(frame, now_ms(), reply, sizeof(reply));
switch (o.event) {
    case btp::SessionEvent::HelloAccepted:
    case btp::SessionEvent::HelloRejected:
    case btp::SessionEvent::SessionClosed:  send(reply, o.reply_size); break;
    case btp::SessionEvent::FrameAccepted:  route(frame);              break;
    default: break;
}

// from the main loop or a timer:
if (session.poll(now_ms()).event == btp::SessionEvent::TimedOut)
    back_to_console();
```

`on_frame()` sweeps the deadline first (like `btp::Receiver::submit` sweeping
timeouts), so a frame that arrives after the deadline is a `TimedOut`, not a
renewal. Once `Active`, *any* valid frame renews the watchdog before its object
is looked at — `SESSION_CLOSE` is handled here, everything else comes back as
`FrameAccepted` for the caller to route. A transport with no console phase
(ESP-NOW, USB HID) calls `arm()` on link-up instead of after an ENTER line;
`hello_deadline_ms = 0` disables the initial deadline entirely.

`set_local()` replaces the advertisement between sessions — a peer whose
`config_revision` moved when its manifest catalogue changed refreshes it so the
next `HELLO_RESULT` reports the current value.

**What stays out:** the **initiator** side — sending `ENTER` / `HELLO`, the
retry budget, awaiting `HELLO_RESULT` (a desktop tool's job; a future
`btp::SessionInitiator`); the plain-ASCII `BTP/1 ENTER|READY|CONSOLE` console
text on serial ([§3–4](session-and-terminal.md#3-entering-protocol-mode-on-serial),
link framing above this layer); and routing an accepted frame by `object_id`,
the priority scheduler and the `STATUS` counters — all the integration's.

---

## 14. The endpoint layer

`btp::Endpoint` (`btp/endpoint.hpp`, target `btp::endpoint`) is the transmit
side of a BTP endpoint written once. `btp::codec` encodes one frame and
`btp::fragmentation` slices a payload into frames; neither remembers who *this*
node is, hands out the next sequence number, or fixes the order the steps go in
when a message is both encrypted and fragmented. Every producer — `STATUS`,
`TELEMETRY`, `MANIFEST_DATA`, `COMMAND_RESULT`, `LOG`, `TERMINAL_OUT` — needs
exactly that, and before this each integration hand-rolled it.

### 14.1 The contract

Same guarantees as `btp::codec`: no internal allocation, `noexcept`, no I/O, no
clock, no global state. The only state is the identity and one sequence
counter. What it holds:

* **identity** — `configure(source_id, boot_id)` once at boot; both must be
  non-zero (BTP reserves `0` for each).
* **sequencing** — `reserve_sequence()` (a CAS loop that always succeeds while
  sequences remain) and `try_reserve_sequence()` (a single CAS for a hard
  non-blocking producer). `0` is never handed out and is the permanent
  "sequence space exhausted" sentinel. These two are the *only* methods safe to
  call from more than one context at once.

What the caller still owns:

* **addressing and routing** — which MAC / socket / peer a frame goes to, and
  any per-peer table, are hidden behind the send callback
  ([model.md §5](model.md#5-identity-and-transport-addressing): BTP defines no
  routing identifiers).
* **the seal** — an `EndpointSealFn` callback, so the class carries no
  cryptographic dependency (the AEAD backend, key selection and the fail-closed
  policy are the integration's — see [Encryption §7.1](#71-sealing)).
* **the transport** — an `EndpointSendFn` callback receives each encoded frame;
  the class never touches a socket or a radio.
* **the fragmented-and-encrypted scratch buffer** — a byte region big enough
  for `payload_size + 16`, whose size is a deployment choice (a
  microcontroller cannot afford the buffer a desktop can).

### 14.2 The pipeline

`send_logical()` reserves a sequence and then, for an encrypted message:

1. build the **canonical** logical header — `FRAGMENTED` cleared,
   `fragment_index` 0, `fragment_count` 1, `ENCRYPTED` set — so the associated
   data matches what a receiver reconstructs
   ([Encryption §7.2](#72-header-serialization-and-aad));
2. call the `EndpointSealFn` **once**, over the whole logical payload;
3. compute `fragment_count` from the **sealed** size (`payload + 16`), never
   the plaintext size — a message that fits one frame unsealed can need two
   once the tag is added, and hand-slicing the plaintext would lose the tail;
4. `make_fragment()` / `encode()` each wire frame from the sealed bytes and
   hand it to the `EndpointSendFn`.

An unencrypted message skips steps 1–3 and slices the plaintext directly. Both
paths **fail closed**: a `false` from the seal or the send callback, an
oversized payload, or an unconfigured endpoint stops the send and returns
`false`. There is no rollback of frames already handed out.

```cpp
btp::Endpoint endpoint;
endpoint.configure(source_id_from_mac(mac), boot_id);

std::uint8_t scratch[kMaxLogicalPayload + btp::kEndpointAeadTagSize];
btp::LogicalMessage msg{btp::MessageType::Control, kStatusObjectId, now_us,
                        {payload, payload_size}};
endpoint.send_logical(msg, btp::TransportProfile::EspNow,
                      &radio_send, &radio, scratch, sizeof(scratch),
                      &channel_c_seal, &seal_ctx);
```

`send_logical_reserved()` runs the same pipeline with a sequence a non-blocking
producer already took, so a queued large sample keeps its place among its
one-frame neighbours. `encode_fragment()` / `send_fragment()` build exactly one
physical frame (a sealed one must be the whole logical message —
`fragment_count == 1` — because the tag covers a whole payload, never a slice).
`send_encoded()` hands an already-encoded frame straight to the transport with
no re-encode and no CRC recomputation, for a relay or playback path carrying
another endpoint's AEAD nonce.

### 14.3 What stays out

The **receive** path is its own layer, `btp::Receiver`
([§15](#15-the-receive-layer)) — the decode + reassembly half — but the
routing decision on top of a completed message stays the integration's: a hub
relays an unrecognised frame by default, an endpoint drops it. Link **framing**
(COBS, HID report padding, ESP-NOW datagram boundaries) is `btp::stream`'s or
the transport's, applied to the bytes the send callback receives. The
**session** a reply rides in — `HELLO` negotiation and the watchdog — is
`btp::Session` ([§13.5](#135-the-session-state-machine-btpsession)) on the
responder, but the initiator side and the serial console↔protocol text stay
the integration's ([§11.1](#111-the-session-state-machine)).

---

## 15. The receive layer

`btp::Receiver` (`btp/receiver.hpp`, target `btp::receiver`) is the receive
side of a BTP endpoint written once — the mirror of `btp::Endpoint`
([§14](#14-the-endpoint-layer)). `btp::decode()` validates one frame and
`btp::Reassembler` puts fragments back together; the wiring between them is the
same everywhere and easy to get subtly wrong. Every consumer — an executor
running `COMMAND`, a plotter reading `TELEMETRY`, a hub relaying both — needs
that wiring, and before this each integration hand-rolled it.

### 15.1 The contract

Same guarantees as `btp::codec`: no internal allocation (the caller owns the
reassembly slots and their byte regions — exactly `btp::Reassembler`'s
storage — plus the buffer `submit()` copies a completed message into),
`noexcept`, no I/O, no global state. It takes a clock the way `btp::Reassembler`
does — `now_ms` as an argument — so the fragment timeout needs no time source
of its own.

It is **not** internally synchronised. One context calls `submit()` (the RX
path); `stats()` returns a by-value snapshot of plain 32-bit counters, which
is all a `STATUS` report needs.

### 15.2 Using it

```cpp
btp::ReassemblySlot slots[4];
std::uint8_t bytes[4][kMaxPayload];
btp::ReassemblyStorage storage[4];
for (std::size_t i = 0; i < 4; ++i) storage[i] = {bytes[i], kMaxPayload};
btp::Receiver receiver(slots, storage, 4, 4000, btp::TransportProfile::EspNow);

std::uint8_t out[kMaxPayload];
btp::ReceivedMessage msg{};
switch (receiver.submit(datagram, datagram_size, now_ms, out, sizeof(out), &msg)) {
    case btp::ReceiveOutcome::Complete:
        route(msg.header, msg.payload);   // the integration's one switch
        break;
    case btp::ReceiveOutcome::FragmentAccepted:
    case btp::ReceiveOutcome::DuplicateFragment:
        break;                            // nothing to deliver yet
    default:
        note_drop();                      // DroppedCrc / DroppedDecode / DroppedReassembly
        break;
}
```

`submit()` sweeps stale partials first (each counted as a
`reassembly_timeout`), then decodes, then — for a fragment — feeds the
reassembler. On `Complete` the logical payload is **copied** into `out` and the
slot released before returning, so a slow handler downstream can never hold a
slot another sender needs. `msg.payload` is that copy, valid until the next
`submit()`. `msg.reassembled` says whether it came out of the slot pool, since
the normalised header cannot.

A second `submit()` overload takes a `btp::DecodedFrame` — the output of
`btp::SerialDecoder` on the COBS / serial receive path — and runs only the
reassembly stage.

### 15.3 What stays out

The routing switch on a completed message; **opening** an encrypted payload
(`aead_open()` runs on the whole logical payload, after reassembly —
`btp::Receiver` hands back the sealed bytes); and link **framing**, which is
`btp::stream`'s or the transport's.

---

## 16. Putting the library together

A typical unencrypted transmit path is:

```text
application payload
       |
       v
build BTP header
       |
       v
fragment_count()
       |
       v
make_fragment()
       |
       v
encode()
       |
       v
transport owned by application
```

An encrypted transmit path is:

```text
application plaintext
       |
       v
aead_seal()
       |
       v
ciphertext || tag
       |
       v
fragment_count()
       |
       v
make_fragment()
       |
       v
encode()
       |
       v
transport
```

`btp::Endpoint` ([§14](#14-the-endpoint-layer)) is those two paths bundled
behind one call, with the identity and sequence filled in and the encrypted
ordering fixed; a producer that wants the steps by hand still has them.

A receive path for a packet-oriented transport is:

```text
transport receives frame
       |
       v
decode()
       |
       v
Reassembler::push()
       |
       v
complete logical message
       |
       +---- encrypted? ----> aead_open()
       |
       v
higher-level BTP payload handling
```

`btp::Receiver` ([§15](#15-the-receive-layer)) is the `decode()` and
`Reassembler::push()` steps bundled behind one `submit()`, with the timeout
sweep and the `STATUS` counters; `aead_open()` and the routing switch stay the
caller's. On the responder, each decoded frame also goes to `btp::Session`
([§13.5](#135-the-session-state-machine-btpsession)), which runs the `HELLO`
handshake, renews the inactivity watchdog and answers `SESSION_CLOSE` before
the routing switch sees the frame.

For serial:

```text
UART byte stream
       |
       v
SerialDecoder
       |
       v
DecodedFrame
       |
       v
Reassembler::push()
       |
       v
complete logical message
       |
       v
higher-level BTP payload handling
```

The higher-level payload handling may then process:

```text
TELEMETRY
COMMAND
CONTROL
TERMINAL
LOG
```

according to the corresponding BTP chapter -- for `COMMAND` and `CONTROL`,
through `btp::messages` (§12).

---

## 17. Summary

The BTP reference library implements the wire:

```text
frame serialization
CRC validation
transport limits
fragmentation
reassembly
serial COBS
optional AEAD
COMMAND / CONTROL payload layout      (btp::messages)
identity, sequencing, transmit path   (btp::endpoint)
decode + CRC + reassembly path        (btp::receiver)
session lifecycle + inactivity watchdog, responder side   (btp::session)
```

It deliberately does not own the surrounding application.

In particular, the library does not own:

```text
transport I/O

manifest catalogue storage

telemetry schema interpretation

the session initiator (sending ENTER / HELLO) and the serial console text

command execution

the subscription aggregator

the priority scheduler
```

This distinction is also what makes the library's memory model straightforward.

The library does not dynamically allocate hidden storage.

Instead:

```text
caller provides memory
        |
        v
BTP operates within that capacity
```

For reassembly, this means explicitly supplied slots and payload storage.

For encoding and decoding, this means caller-owned input and output buffers.

For serial decoding, this means caller-owned COBS and decoded-frame buffers.

For runtime objects such as manifest descriptors, the higher-level implementation chooses its own storage model.

Therefore:

```text
runtime discovery
```

does not imply:

```text
dynamic memory inside BTP
```

An embedded implementation can use fixed-capacity storage while a desktop application uses another strategy, and both can exchange exactly the same BTP wire representation.

The reference implementation defines and validates the bytes.

The application owns the transport and the higher-level protocol state.