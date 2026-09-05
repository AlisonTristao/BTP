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

The public API is divided into eleven headers.

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
| `btp/catalog.hpp`       | `btp::Catalog` — the schema catalogue behind `MANIFEST_DATA` |
| `btp/node.hpp`          | `btp::Node` — endpoint + receiver + session + catalogue in one object |
| `btp/aead.hpp`          | Optional version-2 authenticated encryption                 |

The implementation is exposed through nine CMake targets.

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

It has four members, two pairs of responder / initiator. `btp::DedupCache` is
the bounded, caller-owned command-deduplication cache of [Commands and
discovery §2.5–2.6](commands.md#25-command-deduplication): an executor feeds
it each `COMMAND_REQUEST` identity and it says whether to execute, replay a
stored `COMMAND_RESULT`, or reject a conflict — and it holds **no clock**
(boot-scoped, no time-based expiry). `btp::CommandClient` is its initiator
counterpart: send `COMMAND_REQUEST`, correlate the eventual `COMMAND_RESULT`,
time out with no retry budget if none comes.
`btp::Session` is the responder state machine of [Session and terminal
§3–5](session-and-terminal.md#3-entering-protocol-mode-on-serial): `HELLO`
negotiation, `SESSION_CLOSE`, and the inactivity watchdog. `btp::SessionInitiator`
is the other end: send `HELLO`, await a correlated `HELLO_RESULT`, run the
same watchdog once `Active`. All four take a `now_ms` reading on every timed
call, the same way `btp::Reassembler` does, but read no clock themselves.
[§16.3](#163-the-session-initiator-connect) and
[§16.7](#167-commands-btpdedupcache-btpcommandclient) cover how `btp::Node`
wires them in.

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

### `btp::node`

`btp::node` contains:

```text
node.cpp
```

It links `btp::endpoint`, `btp::receiver`, `btp::session`, `btp::catalog` and
`btp::subscription` — the layers it wires together — and, like them, needs no
mbedtls and is always built. Same guarantees as the rest of the library.

It is `btp::Node`: `btp::Endpoint`, `btp::Receiver` and — opt-in —
`btp::Session`, a `btp::Catalog` and subscriptions in one object, sharing one
identity, one `now_ms` notion and one set of caller-owned buffers, with every
external dependency (`send`, `clock`, `seal`, `open`, ...) a virtual method on
`NodeConfig`, an abstract class a consumer inherits from once. It adds no wire
field and holds no new state — the members stay reachable for a caller that
wants a layer directly. [The node layer](#16-the-node-layer) covers it.

### `btp::catalog`

`btp::catalog` contains:

```text
catalog.cpp
```

It links `btp::telemetry` (for `FieldSpec` / `field_spec`, which brings
`btp::messages`) and, like it, needs no mbedtls and is always built. Same
guarantees as the rest of the library.

It is `btp::Catalog`: the TELEMETRY topics a producer exposes, or the ones a
consumer has learned from a `MANIFEST_DATA` — the `ManifestReader` walk and the
`FieldSpec` cache that [§11.2](#112-the-manifest-catalogue-is-not-stored) left
above the wire, with the storage still the caller's (fixed-capacity on a
microcontroller). `btp::Node` attaches one and keeps it current.
[§16.4](#164-the-schema-catalogue-btpcatalog) covers it.

### `btp::subscription`

`btp::subscription` contains:

```text
subscription.cpp
```

It links `btp::catalog` (topic validation, `max_rate_millihz`) and, like it,
needs no mbedtls and is always built. Same guarantees as the rest of the
library.

It is `btp::SubscriptionTable` / `btp::SubscriptionClient`: `SUBSCRIBE` /
`SUBSCRIBE_RESULT` / `UNSUBSCRIBE` ([Commands and discovery
§4](commands.md#4-subscriptions)) as state above the wire, the responder /
initiator split mirroring the session layer. `btp::Node` attaches them
opt-in, storage still the caller's. [§16.6](#166-subscriptions-btpsubscriptiontable-btpsubscriptionclient)
covers it.

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
    ^     ^          ^                 ^             |
    |     +----+     |     +-----------+             v
    |          |     |     |                    mbedcrypto
btp::telemetry btp::session |
    ^     ^                 |
    |     +-----------------+---------- btp::node
btp::catalog ------------------------------/
```

`btp::messages`, `btp::endpoint` and `btp::receiver` each link `btp::codec`;
`btp::telemetry` and `btp::session` each link `btp::messages`; `btp::catalog`
links `btp::telemetry`; `btp::node` links `btp::endpoint`, `btp::receiver`,
`btp::session` and `btp::catalog`; `btp::aead` additionally links mbedcrypto.
Encryption is therefore optional at build time, and the basic frame codec
requires neither the payload layer nor the cryptographic component.

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
        btp::kEspNowTransport,
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

### 4.2 Transport limits

`TransportLimits` (a plain `{max_frame_size, allow_encrypted}`, not a fixed enum of named profiles) selects the frame-size restriction -- and whether an ENCRYPTED frame is allowed at all -- that applies to the operation. There is no `max_payload_size` field: `btp::max_payload_size(transport)` always derives it as `max_frame_size` minus the 40-octet header+CRC floor, the same relationship every real transport already has, so there is nothing to set independently and no way for the two to disagree.

The codec uses it to validate whether the frame can be represented on that transport.

It does not perform transport I/O.

Three presets cover the transports section 4 below documents:

```cpp
btp::kEspNowTransport   // max_frame_size 250 (-> payload 210), encryption allowed
btp::kSerialTransport   // max_frame_size 4096 (-> payload 4056), encryption allowed
btp::kUsbHidTransport   // max_frame_size 62 (-> payload 22), encryption NOT allowed
```

A caller with a different link builds its own `btp::TransportLimits{max_frame_size, allow_encrypted}` -- there is no enum to extend.

For example:

```cpp
btp::kEspNowTransport
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

For transport-wide limits: the maximum physical frame size is `transport.max_frame_size` itself (a plain field, no function needed); the maximum payload size is derived, via

```text
max_payload_size(transport)
```

These are useful for:

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
        btp::kEspNowTransport,
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

`fragment_count()` calculates how many BTP frames are required for a logical payload under the selected `TransportLimits`.

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

The specification, the reference library and the conformance vectors are one
thing with one version: a single SemVer line, `MAJOR.MINOR.PATCH`.

```text
MAJOR   the wire format changed incompatibly
MINOR   a backward-compatible addition -- a new library layer, an optional
        field. The wire is unchanged.
PATCH   a fix with no effect on the wire or the public API
```

The git tag is `vMAJOR.MINOR.PATCH` -- the same number. There is no "library
version" distinct from the release; the two are the same, and this chapter's
old distinction between them is gone.

### 10.1 One source of truth

The number lives in one file:

```text
include/btp/version.hpp   kLibraryVersion{Major,Minor,Patch}
```

Everything else is derived from or checked against it:

* `CMakeLists.txt` parses those three lines for `project(VERSION)` -- it never
  spells the number itself;
* `library.json` (which PlatformIO reads, and which never runs CMake) carries a
  copy that `python tools/version.py` writes and that a plain `cmake -S . -B
  build` re-checks, refusing to configure on a mismatch;
* `kMaximumProtocolVersion` -- the highest wire-version byte this library
  accepts -- equals `kLibraryVersionMajor` by a `static_assert`, because a new
  wire version *is* what a MAJOR bump is.

To cut a release: `python tools/version.py X.Y.Z`, commit, then `git tag
vX.Y.Z`. A pre-release suffix (`-beta`) is only for a still-settling `MAJOR.x`
line and only in `library.json`; CMake's `project(VERSION)` is numeric-only.

### 10.2 The wire-version byte is a frame tag, not a release number

The octet at header offset 4 ([The datagram §2](frame.md#2-header-format)) is a
frame-format tag:

```text
0x01   the base frame
0x02   the payload is AEAD-sealed (ENCRYPTED set, docs/encryption.md)
```

A library on the `MAJOR.x` line **encodes** the lowest tag a given frame needs
(so a cleartext frame from a `2.x` build still goes out as `0x01`) and
**decodes** every tag up to `MAJOR`. So `wire 2` and "the `2.x` line" now name
the same thing; the one phrasing still worth avoiding is a bare `v2` when you
specifically mean the byte -- write `wire 2` there.

### 10.3 Branches

`main` always carries the newest major. When a new major lands, the previous one
is cut to a `MAJOR.x` branch and maintained there.

| Branch | Holds | Newest wire byte |
| ------ | ----- | ---------------- |
| `main` | the `2.x` line | `0x02` |
| `1.x`  | maintenance of the `1.x` line (`v1.1.0-beta`) | `0x01` |

The `1.x` branch is for a deployment that cannot take the whole `2.x` library
surface, not a sign that `main` dropped wire 1 -- a `2.x` build still speaks it.
A branch is named after the major it holds, never the one still coming: when
wire 3 arrives, a `2.x` branch is cut from that day's `main` and `main` moves to
`3.0`.

---

### 10.4 Protocol changes and vectors

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

Internally, the current implementation uses the serial payload ceiling as its fragment-storage backstop rather than binding each `Reassembler` instance to one `TransportLimits`.

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

`btp::HelloBuilder(role, peer_uuid)` builds a wire-valid `Hello` with sane
defaults for every field a deployment rarely touches -- one protocol version,
`max_logical_payload` 2048, 4 concurrent reassemblies, 8 subscriptions, 32
dedup entries, a 30 s watchdog, no manifest advertised (`config_revision`
0). Chain `.session_timeout_ms(...)` / `.max_logical_payload(...)` / ... to
override only the fields that matter for this deployment:

```cpp
const btp::Hello h = btp::HelloBuilder(btp::Role::Consumer, my_uuid)
                          .session_timeout_ms(15000U)
                          .max_logical_payload(4096U)
                          .build();
```

`role` and `peer_uuid` have no safe default -- an all-zero `peer_uuid` is
explicitly invalid on the wire -- so both are constructor arguments, not
chain calls: a `HelloBuilder` always builds a wire-valid `Hello`, never a
half-filled one waiting on a call that was forgotten. `Hello h = {}; h.role =
...;` field by field is still there for anything the builder does not wrap
(`version_count` / `versions[]` for more than one announced protocol
version, say).

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
never touches `name` / `unit` / enum entries). A consumer converts each walked
`FieldRecord` with `field_spec()` and caches the array; a producer that also
publishes a manifest keeps the schema as `FieldRecord[]` and gets the
`FieldSpec[]` from `field_spec()` (or hands the `FieldRecord[]` to
`btp::Catalog`, [§16.4](#164-the-schema-catalogue-btpcatalog)). `order` is the
array position.

The `btp::f32` / `btp::u16` / `btp::i16` / … helpers write a `FieldRecord`
schema one readable line per field -- `field_id` and `order` default to the
position:

```cpp
static const btp::FieldRecord kDriveStatus[] = {
    btp::f32("left_rpm", "rpm"),
    btp::f32("right_rpm", "rpm"),
    btp::u16("battery_v", 0.001, "V"),          // stored as millivolts
    btp::nullable(btp::i16("temp_c", 0.1, "Cel")),
};
```

The raw form -- a `FieldSpec[]` literal, for the sample codec with no manifest:

```cpp
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
// btp::HelloBuilder (§12) fills every field a deployment rarely touches --
// only max_inflight_reassemblies and config_revision differ from its
// defaults here.
const btp::Hello local =
    btp::HelloBuilder(btp::Role::Producer, my_uuid)
        .max_inflight_reassemblies(1U)
        .config_revision(manifest_revision)
        .build();

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
endpoint.send_logical(msg, btp::kEspNowTransport,
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
btp::Receiver receiver(slots, storage, 4, 4000, btp::kEspNowTransport);

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

## 16. The node layer

`btp::Node` (`btp/node.hpp`, target `btp::node`) is the friendly facade:
`btp::Endpoint`, `btp::Receiver` and — opt-in — `btp::Session` and a
`btp::Catalog` in **one object**. Those layers already un-hand-roll the
transmit, receive, responder-session and schema-discovery mechanisms; what
stayed the integration's, and what every consumer then wrote once by hand, is
the *wiring* between them — give the endpoint an identity, size and bind the
reassembly storage, thread one `now_ms` through both, feed each decoded frame to
the session before routing it, ingest each `MANIFEST_DATA` into the catalogue,
answer
`HELLO` / `SESSION_CLOSE` through the same send path. `btp::Node` is that wiring
written once.

It adds **no wire field** and holds **no new state** — it forwards to the three
objects it owns, which stay reachable (`endpoint()`, `receiver()`, `session()`)
for a caller that wants a layer directly. Same guarantees as the rest of the
library.

### 16.1 The contract

`NodeConfig` is an abstract class a consumer **inherits from once** — not a
struct of `(function pointer, void* ctx)` pairs to fill in by hand, which is
what it was through library 2.33. `source_id`, `boot_id` and `transport` (a
`TransportLimits`, [§4.2](#42-transport-limits)) stay plain data, set in your
constructor; every external dependency is a virtual method, each an optional
`has_X()` / `X()` pair Node calls in that order — it calls `X()` only when
`has_X()` says `true` — except `send()`, the one axis every node needs:

| method | when | what it does |
| --- | --- | --- |
| `send()` | REQUIRED | one encoded frame → your radio / UART / HID; a receive-only node still overrides it, returning `false` |
| `has_clock()` / `clock()` | optional | → `now_ms`; not overridden means you pass `now_ms` explicitly to `receive()` / `tick()` / `routine()` |
| `has_seal()` / `seal()` | optional | encrypt one logical payload; not overridden → `send()` is cleartext |
| `has_open()` / `open()` | optional | decrypt one received payload; not overridden → `receive()` hands back the sealed bytes |
| `has_terminal()` / `terminal()` | optional | answer a `TERMINAL_IN` / `TERMINAL_OUT` frame directly ([§16.5](#165-terminal-nodeon_terminal-nodeconfigterminal)) |
| `has_command()` / `command()` | optional, `SizedNode<>` / `StaticNode<>` only | run a Fresh `COMMAND_REQUEST`, synchronously or not ([§16.7](#167-commands-btpdedupcache-btpcommandclient)) |
| `reply_seal()` | optional | picks the seal for ONE automatic reply (`SUBSCRIBE_RESULT` / `UNSUBSCRIBE_RESULT` / `COMMAND_RESULT` / `MANIFEST_DATA`) from the *original request's* header; default falls through to `has_seal()` / `seal()` |

```cpp
class RobotLink : public btp::NodeConfig {
public:
    RobotLink(const AeadKey& key) : key_(key) {
        source_id = 0x00CAFE01U;
        boot_id   = 0x0000B001U;
        transport = btp::kEspNowTransport;
    }
    bool send(const std::uint8_t* frame, std::size_t n) override {
        return esp_now_send(peer_mac_, frame, n) == ESP_OK;
    }
    bool has_seal() const override { return true; }
    bool seal(const btp::Header& h, std::uint16_t n, const std::uint8_t* pt,
              std::uint8_t* out) override {
        return btp::aead_seal(key_, h, n, pt, out) == btp::AeadError::Ok;
    }
    // has_open()/open() the same, if this node also receives.
private:
    AeadKey key_;
};
```

The **key never enters BTP** — `seal()` / `open()` select it from
`header.source_id` (real: `RadioSeal` / `bally-seal`) and call
`btp::aead_seal` / `btp::aead_open`. The nonce (`source_id ‖ boot_id ‖
sequence`) and the AAD (the canonical header) are `btp::aead`'s.

`Node` holds `cfg` by **reference**, reading it live at each call rather than
copying it in at construction — a caller whose identity is only known after
something else exists (a `TxScheduler` configured later than the `Node`
itself) just mutates `source_id` / `boot_id` / `transport` directly, any time
before `begin()`. (This is also what replaced the short-lived
`Node::reconfigure()` method — see the [README](../README.md)'s version
history.) The usual shape both inherits `NodeConfig` and owns its
`Node` as a member ([`example/sender.cpp`](../example/sender.cpp),
[`example/receiver.cpp`](../example/receiver.cpp), via
[`example/node_config.hpp`](../example/node_config.hpp)): base-before-member
construction order means `*this` is already a live `NodeConfig` by the time
the `Node` member's own constructor runs, and a member outlives the object it
is declared in, so the reference stays valid for the `Node`'s whole lifetime
with no manual bookkeeping. Don't call any method on the `Node` you're
mid-constructing from your own mem-initializers or constructor body.

Storage stays caller-owned, exactly as `btp::Receiver` and `btp::Endpoint`
expect: the reassembly slots and their byte regions, a buffer `receive()`
copies a completed message into, a seal scratch for the fragmented-encrypted
path, a buffer `open()` writes plaintext into, and a scratch buffer
`serve_catalog()` / `publish()` build into. **`btp::SizedNode<NodeSize::Low |
Medium | High>`** owns all of it as members, pre-sized to one of three memory
tiers — `Low` ≈ 7.2 KiB (one small topic, a battery-powered sensor), `Medium`
≈ 17.4 KiB (the ESP32-class robot this library was sized for, and identical to
`StaticNode<>`'s own bare defaults), `High` ≈ 67.7 KiB (a hub or desktop
aggregator) — the right starting point for the common embedded case
([§16.2](#162-using-it)). **`btp::StaticNode<Slots, SlotBytes, SealBytes,
ScratchBytes, CatalogTopics, CatalogFields, CatalogStringBytes,
MaxSubscriptions, MaxCommands, CommandBytes>`** is the same storage with every
dimension spelled out by hand (`SizedNode`'s own alias target) — the escape
hatch for a node that needs one dimension off that curve, a desktop hub with a
huge catalogue but few concurrent reassemblies, say.

**One context** calls `send` / `receive` / `tick` / `routine`, as
`btp::Receiver` and `btp::Session` require. A timer may *pace* `tick()` /
`routine()` — but by raising a flag the loop reads, never by calling either
from the ISR. Reading the *clock* from an ISR is fine.

### 16.2 Using it

```cpp
class RobotLink : public btp::NodeConfig { /* send() etc. -- §16.1 */ };

RobotLink link;
link.source_id = 0x00CAFE01U;
link.boot_id   = 0x0000B001U;
link.transport = btp::kEspNowTransport;

// SizedNode<Low | Medium | High> owns every buffer as a member, pre-sized to
// one of three memory tiers (§16.1). `link` is held by REFERENCE and must
// outlive `node` -- true here simply because it is declared first.
btp::SizedNode<btp::NodeSize::Medium> node(link);

// producer: declare a topic on the node's own catalogue, with the callback
// that fills a sample registered in the SAME statement (§16.6 has the fill
// function's own shape) --
node.topic(0x0101U, /*schema_version=*/3U, "drive_status", &fill_drive_status)
    .f32("left_rpm", "rpm")
    .u16("battery_v", 0.001, "V")
    .end();

// Every other Hello field takes btp::HelloBuilder's own default (messages.hpp)
// -- override one with e.g. .session_timeout_ms(15000U) if this robot needs a
// shorter watchdog. StaticNode<>::begin(name, hello) -- SizedNode<> included,
// it is a StaticNode<> alias -- serves the catalogue, arms the session and
// announces MANIFEST_DATA in the one call.
if (!node.begin("example-robot",
                btp::HelloBuilder(btp::Role::Producer, my_uuid).build())) {
    /* bad identity or storage */
}

// consumer instead: connect() OUT rather than accept a connection -- §16.3 --
//   if (!node.begin(hello, /*connect_deadline_ms=*/2000U)) { ... }

std::uint64_t now_ms = 0U;
for (;;) {
    const std::size_t n = link_poll(datagram, sizeof datagram);
    // datagram (if any) -> receive(); either way, publish due topics and
    // sweep the session watchdog. COMMAND_REQUEST / TERMINAL_IN are both
    // handled INSIDE routine() -- RobotLink::command() / RobotLink::terminal()
    // (if overridden, §16.5 / §16.7) already ran by the time it returns.
    node.routine(datagram, n, now_ms);
    now_ms = read_clock();
}
```

If identity/`send`/`seal`/... is only known after something else exists (say,
a `TxScheduler` configured later than the config object itself), there is no
placeholder-then-reconfigure dance to do: `cfg` is read live at each call
(§16.1), so a caller in that shape just assigns `link.source_id = ...` /
`link.transport = ...` directly, any time before `begin()`.

`send()` reserves a sequence, seals once over the whole payload when
`cfg.has_seal()`, fragments, and hands each frame to `cfg.send()` — a `false`
from `seal()` sends nothing (fail-closed). `send_with()` takes a seal for one
message (a hub sealing channel C and channel B with different keys) — that
covers the caller's OWN sends, but not the node's automatic replies to a
SUBSCRIBE / UNSUBSCRIBE / COMMAND_REQUEST / MANIFEST_REQUEST, which always
used `has_seal()`/`seal()` alone until `reply_seal()` (§16.1): a hub-shaped
responder that answers a request with whichever key matches ITS ORIGIN (not
one key for every automatic reply) overrides `reply_seal()` instead — see its
own doc comment in `node.hpp`. The default (falling through to
`has_seal()`/`seal()`) is exactly today's single-key behavior.

`receive()` sweeps stale partials, decodes, checks CRC and reassembles; with a
session enabled it also runs the `HELLO` handshake, renews the watchdog and
answers `SESSION_CLOSE` (framing the reply and sending it through `cfg.send()`)
*before* a frame is routed. `receive(const DecodedFrame&, …)` (2.35.0) is the
same, minus the `btp::decode()` — for a caller that owns its link framing and
has already turned a COBS block / HID report into one whole BTP frame. It
returns a `NodeRx` — the outcomes collapsed from `btp::ReceiveOutcome` and
`btp::SessionEvent`:

| `NodeRx` | meaning |
| --- | --- |
| `Complete` | `*out` is a whole logical message (plaintext if `open()` ran) — route it |
| `Pending` | a fragment stored or a duplicate absorbed — nothing yet |
| `SessionHandled` | a `HELLO` / `SESSION_CLOSE` / session timeout — the node already replied; see `session_event()` |
| `InitiatorHandled` | `connect()`'s `HELLO_RESULT` arrived, or the connection watchdog timed out; see `initiator_event()` ([§16.3](#163-the-session-initiator-connect)) |
| `SubscriptionServed` | a `SUBSCRIBE` / `UNSUBSCRIBE` against `subscriptions()` — the node already replied ([§16.6](#166-subscriptions-btpsubscriptiontable-btpsubscriptionclient)) |
| `SubscriptionHandled` | a `SUBSCRIBE_RESULT` for a `subscribe()` / renewal this node holds; see `subscription_event()` |
| `CommandServed` | a `COMMAND_REQUEST` against `commands()` -- the node already ran / replayed / rejected it and replied ([§16.7](#167-commands-btpdedupcache-btpcommandclient)) |
| `CommandHandled` | a `COMMAND_RESULT` for a `command()` this node holds; see `command_outcome()` |
| `CatalogUpdated` | a `MANIFEST_DATA` — ingested into the attached learn catalogue ([§16.4](#164-the-schema-catalogue-btpcatalog)) |
| `SampleDelivered` | a `TELEMETRY` sample of a known topic — decoded and handed to `on_sample()` |
| `TerminalDelivered` | a `TERMINAL_IN` / `TERMINAL_OUT` frame — handed to `terminal()` / `on_terminal()` ([§16.5](#165-terminal-nodeon_terminal-nodeconfigterminal)) |
| `RequestServed` | a `MANIFEST_REQUEST` — the node built and sent `MANIFEST_DATA` |
| `Ignored` | a frame the node would manage but cannot yet: a session not `Active`, or a sample for a topic the catalogue has not learned |
| `DroppedFrame` | `btp::decode` / reassembly / a malformed managed payload rejected it — the breakdown is in `stats()` |
| `NoDatagram` | `routine()` only, below — nothing arrived this pass, `receive()` did not run, but the housekeeping still did |

`tick()` sweeps reassembly timeouts and polls the session watchdog, returning
the `SessionEvent` (`TimedOut` once when a dead session is noticed).

`routine()` — library 2.26 — is `receive()` plus the per-pass housekeeping
every loop needs regardless of whether a datagram arrived this pass:
`size == 0` skips `receive()` (returning `NodeRx::NoDatagram`) instead of
decoding nothing, and either way `publish_subscribed_topics(now_ms)`
([§16.6](#166-subscriptions-btpsubscriptiontable-btpsubscriptionclient)) and `tick(now_ms)` run right after — due topics get sent, then the
session/connection watchdog and subscription lease renewal, each a no-op
when the matching axis is not enabled. The SAME call covers a producer, a
consumer, or a node that is both — see `example/sender.cpp` /
`example/receiver.cpp`, both down to one `routine()` call in their loop.

Three overloads: `routine(datagram, size, now_ms)` — the one in the example
above, and in both — drops `Complete`'s payload, for a caller with a callback
attached for everything it cares about (every OTHER outcome already ran its
own — `on_sample()` / `on_publish()` / `terminal()` — before returning);
`routine(datagram, size, now_ms, out)` keeps it, for a caller that still
wants to read an unmanaged message type by hand; `routine(now_ms)` runs the
housekeeping alone, with no `receive()` at all, for a caller whose datagrams
arrive on their own task/thread/ISR and call `receive()` there directly.
`receive()` / `tick()` stay reachable on their own for a caller that wants
every piece apart.

### 16.3 The session initiator (`connect()`)

`enable_session()` above is the RESPONDER half — a peer that accepts a
connection. `connect()` is the other end: a node that reaches OUT to one,
wrapping `btp::SessionInitiator` (`btp/session.hpp`, target `btp::session`) the
same way `enable_session()` wraps `btp::Session`. No separate opt-in call is
needed — `connect()` itself is the opt-in, `Idle` until then — and a node may
run both at once (a hub bridging two links: responder toward one peer,
initiator toward another).

```cpp
node.connect(my_hello, /*deadline_ms=*/2000);
for (;;) {
    const btp::NodeRx rx = node.receive(datagram, n, &msg);
    if (rx == btp::NodeRx::InitiatorHandled &&
        node.initiator_event() == btp::InitiatorEvent::Connected) {
        // node.effective_limits() / node.connected_peer_source_id() /
        // node.connected_peer_config_revision() (2.37.0 -- the peer's own
        // manifest-catalogue revision, HELLO_RESULT's own field, not part of
        // effective_limits() because it is reported as-is, never negotiated)
        // now set
    }
    node.tick();   // TimedOut if HELLO_RESULT, or the peer, goes quiet
}
```

`connect()` sends a `HELLO` (needs `cfg.send()`) and waits up to `deadline_ms`
for `HELLO_RESULT`. `receive()` feeds every decoded frame to the initiator the
same way it does the responder session: while awaiting a result, only a
correlated `HELLO_RESULT` progresses the state (`NodeRx::InitiatorHandled`,
`initiator_event()` one of `Connected` / `Rejected` / `TimedOut`) — anything
else is ignored and does not renew the deadline; once `Active`, any valid
frame renews the connection watchdog and is a normal `NodeRx::Complete` /
discovery outcome for the caller to route, exactly like a `FrameAccepted`
frame on the responder side. `disconnect()` sends `SESSION_CLOSE` and tears the
connection down locally without waiting for `SESSION_CLOSE_RESULT`.

Deliberately narrow, the same way `btp::Session` is on the other end: no
`ENTER` / `READY` console text (link framing, above this), no retry budget for
a `HELLO` that gets no answer (call `connect()` again after `TimedOut` /
`Rejected`), and nothing past the handshake — `request_manifest()`,
`subscribe()`-to-be and a command's correlation are the caller's, once
`connected()` is true.

### 16.4 The schema catalogue (`btp::Catalog`)

A `TELEMETRY` body is `PACKED_LE` octets against a schema
([§12.4](#124-decoding-and-encoding-telemetry-samples)); a consumer that has
never met the producer gets that schema from a `MANIFEST_DATA`
([Commands and discovery §3](commands.md#3-the-manifest)). `btp::messages` walks
the manifest and `btp::telemetry` decodes the sample — the piece between,
"which schemas a consumer caches"
([§11.2](#112-the-manifest-catalogue-is-not-stored)), is `btp::Catalog`
(`btp/catalog.hpp`, target `btp::catalog`), with the storage still the caller's.

`btp::Catalog` (or `btp::StaticCatalog<Topics, Fields, StringBytes,
SourceInfoEntries>`, which owns the pools) holds one topic per entry —
`topic_id`, `schema_version`, encoding, `max_rate_millihz`, subscribable, a
`FieldSpec[]`, the topic + field names, and (since 2.28.0) each field's unit and
description, read back with `field_unit()`/`field_description()` next to
`field_name()` — `""` when that pool was not kept or the field is out of range,
never `nullptr`. A topic's own description does not round-trip yet.
`add_topic()` fills it from `FieldRecord`s (a producer's own schema);
`ingest(payload, size)` fills it by walking a `MANIFEST_DATA`;
`write_topics(ManifestWriter*)` serialises it back. Same guarantees as the rest
of the library.

Since 2.35.0 it also carries the **format-2 `source_info` block** — the
informational `key` / `label` / `value` rows a `MANIFEST_DATA` puts ahead of
the topics (`fw_version`, chip id, running partition; [Commands and discovery
§3.12](commands.md#312-source-info)). A producer adds them one at a time with
`add_source_info(key, label, value)` (an empty `value` is skipped, not an
error) and `node.serve_catalog()` emits them for you — `write_source_info()` is
called between `begin(header)` and `write_topics()`, the header goes out as
format 2 on every SUCCESS reply (a full response and a `NOT_MODIFIED` one
alike, [commands.md §3.3](commands.md#33-not_modified)), and a row that would
not fit is dropped so the topic records keep their space. A `REJECTED` reply
(stale boot, unknown source) describes nothing and stays format 1. On the
consumer, `ingest()` walks the block into the catalogue's own
`SourceInfoEntries` pool (when one was sized) and `source_info_at(i)` /
`source_info_count()` read it back — the `ByteView`s point into the string
pool, valid until the next `ingest()`; a `NOT_MODIFIED` response keeps whatever
was already learned. A consumer with no such pool still ingests the topics; the
block is simply skipped.

**On the consumer**, `node.learn_catalog(&catalog)` hands it to the node:

```cpp
btp::StaticCatalog<> catalog;
node.learn_catalog(&catalog);
node.on_sample(&on_drive_status, &ui);   // per-topic, values already converted
node.request_manifest(robot_source_id, robot_boot_id, /*known_revision=*/0);
// ... then, per datagram:
switch (node.receive(datagram, n, &msg)) {
    case btp::NodeRx::CatalogUpdated:  break;  // a MANIFEST_DATA was ingested
    case btp::NodeRx::SampleDelivered: break;  // on_sample already ran
    case btp::NodeRx::Ignored:         break;  // a sample before its manifest
    // ...
}
```

`receive()` ingests every `MANIFEST_DATA` into the catalogue
(`NodeRx::CatalogUpdated`), and — with `on_sample` set — decodes each
`TELEMETRY` sample of a known topic against the learned `FieldSpec[]` and calls
the callback with a `SampleReader` positioned at the first field
(`NodeRx::SampleDelivered`); a sample for a topic not yet in the catalogue is
`NodeRx::Ignored`. `NOT_MODIFIED` keeps the current contents.

**On the producer**, `node.serve_catalog(&catalog, role, uuid, name)` hands over
a catalogue the node answers from:

```cpp
static const btp::FieldRecord kDriveStatus[] = {           // one line per field
    btp::f32("left_rpm", "rpm"),
    btp::u16("battery_v", 0.001, "V"),
    btp::nullable(btp::i16("temp_c", 0.1, "Cel")),
};

btp::StaticCatalog<> catalog;
catalog.set_config_revision(1);
catalog.add_topic(0x0101, /*schema_version=*/3, "drive_status", kDriveStatus);
node.serve_catalog(&catalog, uint8_t(btp::Role::Producer), uuid, "robot");

node.announce_catalog();                            // unsolicited MANIFEST_DATA
node.publish(0x0101, &fill_drive_status, &ctx, now_us());   // a typed sample
```

`add_topic()` deduces the array length; `encoding` defaults to `PackedLe`,
`subscribable` to `true`, `max_rate_millihz` to `0` -- the eight-argument
primitive is there for the rest. `field_id` and `order` come from the array
position; `btp::field(id, type, name, ...)` sets an explicit `field_id` for a
schema that must survive a rename.

The chained alternative to naming that `FieldRecord[]` is `Catalog::topic()`,
which returns a `TopicBuilder` -- the same one-line-per-field helpers, called
in sequence instead of listed, committed with one `end()`:

```cpp
catalog.topic(0x0101, /*schema_version=*/3, "drive_status")
    .f32("left_rpm", "rpm")
    .u16("battery_v", 0.001, "V")
    .i16("temp_c", 0.1, "Cel", /*is_nullable=*/true)
    .end();
```

And `fill_drive_status` above is positional -- `put_f64()` per field, in
schema order, with nothing stopping two calls from landing swapped and
sending the wrong value on the wire, silently. `btp::NamedSampleWriter`
(built from a `CatalogTopic`, which already carries the field names) turns
that into a loud `InvalidArgument` instead: `put("battery_v", 3.72)` must name
the schema's *next* field, or it fails right there. `node.publish_named(topic,
fill, ctx, ts)` is `publish()`'s counterpart taking a `NodeNamedFillFn` (a
`NamedSampleWriter&` instead of a `SampleWriter&`).

**`StaticNode`** additionally owns its own `btp::StaticCatalog` (the
`CatalogTopics` / `CatalogFields` / `CatalogStringBytes` template arguments
above) -- `node.catalog()` reaches it directly, `node.topic(...)` forwards to
`catalog().topic(...)`, and `node.serve_catalog(role, uuid, name)` /
`node.learn_catalog()` with no `Catalog*` argument point at it, so the common
one-topic case needs no separate `btp::StaticCatalog` declared and threaded
through by pointer in the caller. `Node::serve_catalog(&external, ...)` /
`Node::learn_catalog(&external)` stay reachable (`StaticNode` keeps both
overload sets, via `using`) for a caller managing several catalogues -- a hub
learning one schema set per peer, say -- or sharing one across nodes.

`receive()` answers a `MANIFEST_REQUEST` for this source (or a full-catalog
request) by building a `MANIFEST_DATA` from the catalogue and sending it
(`NodeRx::RequestServed`) — a `NOT_MODIFIED` reply when the request's
`known_config_revision` matches, a `STALE_TARGET_BOOT` rejection for a wrong
boot. `announce_catalog()` sends the same body unsolicited (request reference
zero) so a late-joining consumer needs no request. `publish(topic_id, fill)`
finds the topic in the served catalogue, runs a `SampleWriter` against its
schema — `fill` writes the values in schema order — and sends the `TELEMETRY`
frame; the `serve_catalog` / `publish` scratch is the node's fourth buffer
(`StaticNode`'s `ScratchBytes`, default 512).

`publish()` / `publish_named()` always seal with `cfg.seal()` — no override,
the one gap `send()` (which has `send_with()`) did not share until 2.30.0:
`publish_with(topic_id, fill, ctx, ts, seal, seal_ctx)` /
`publish_named_with(...)` are the same pipeline with a seal for THIS sample,
for a producer whose TELEMETRY key differs from whatever `cfg.seal()`
returns (a hub sealing its own automatic replies with one key and a topic
with another). A null override forces cleartext regardless of `cfg.seal()`,
the same rule `send_with()` already follows.

### 16.5 Terminal (`node.on_terminal()` / `NodeConfig::terminal()`)

`TERMINAL_IN` / `TERMINAL_OUT` carry opaque bytes, no struct — [Session and
terminal §7.2](session-and-terminal.md#72-opaque-payload) is the whole reason
why, and `btp::Node` mirrors that: no serve/learn split (there is no
schema to negotiate) and no send sugar either — sending stays plain
`node.send(btp::MessageType::Terminal, btp::object_id::kTerminalIn` (or
`kTerminalOut`) `, payload, size, timestamp_us)`. The only thing `Node` adds
is a place to answer FROM:

```cpp
bool has_terminal() const noexcept override { return true; }
void terminal(btp::Node& node, const btp::Header& header,
              btp::ByteView payload, std::uint64_t now_ms) override {
    node.send(btp::MessageType::Terminal, btp::object_id::kTerminalOut,
              payload.data, payload.size, now_ms * 1000ULL);   // echo
}
```

`receive()` (and `routine()`) call this for EITHER direction — `kTerminalIn`
and `kTerminalOut` both reach it, check `header.object_id` if this node only
expects one — and report `NodeRx::TerminalDelivered` instead of `Complete`.
`node` is this same node, already mid-`receive()` — `send()` / `send_with()`
are fine to call from inside `terminal()`, the same reentrant-from-inside-
`receive()` pattern the automatic SUBSCRIBE / COMMAND / MANIFEST_REQUEST
replies already use ([§16.6](#166-subscriptions-btpsubscriptiontable-btpsubscriptionclient), [§16.7](#167-commands-btpdedupcache-btpcommandclient)), so a handler that talks back (the usual
case: `TERMINAL_IN` answered with `TERMINAL_OUT`) needs nothing threaded in
via a `ctx` to reach it. `now_ms` is `receive()`'s own now, for a reply's
timestamp; do not keep `payload` past the callback, same lifetime as
`ReceivedMessage::payload`.

Unlike commands ([§16.7](#167-commands-btpdedupcache-btpcommandclient)), this wires on ANY `Node`, not only a `SizedNode<>` /
`StaticNode<>` — there is no dedicated storage `terminal()` needs, so the base
`Node` constructor reads `cfg.has_terminal()` once and sets its internal
callback straight from `cfg.terminal()` when it says `true`. Without an
override, a `TERMINAL` frame comes back as `NodeRx::Complete` for the caller
to route by hand — the same fallback every unmanaged message type gets.
`Node::on_terminal(callback, ctx)` stays reachable to set or replace a plain
function-pointer handler after construction, independent of `cfg`'s own
override — the form a raw `Node` with no `NodeConfig::terminal()` override
(or a handler that needs an unrelated `ctx`) uses instead.

### 16.6 Subscriptions (`btp::SubscriptionTable` / `btp::SubscriptionClient`)

`SUBSCRIBE` / `SUBSCRIBE_RESULT` / `UNSUBSCRIBE` ([Commands and discovery
§4](commands.md#4-subscriptions)) get the same responder / initiator split as
the session layer, in `btp/subscription.hpp` (target `btp::subscription`):
`btp::SubscriptionTable` grants subscriptions on a producer's own topics,
`btp::SubscriptionClient` holds and renews the ones a consumer wants from a
peer. Both are state above the wire (§11), with the storage the caller's --
attached to the node, not owned by it, since a hub aggregating subscriptions
across several source peers needs its own shape here.

```cpp
// producer:
btp::SubscriptionRecord slots[8];
btp::SubscriptionTable table(slots, 8);
node.enable_subscriptions(&table);          // needs serve_catalog() too

// per tick():
table.expire(now_ms);
if (table.due(0x0101, now_ms) && node.publish(0x0101, &fill, nullptr, ts_us)) {
    table.note_published(0x0101, now_ms);
}
```

That `due()` / `publish()` / `note_published()` triplet is also what
`on_publish()` + `publish_subscribed_topics()` (2.21.0) do for every topic at
once, so a producer with more than one does not repeat it per topic:

```cpp
node.topic(0x0101, 3, "drive_status").f32("left_rpm", "rpm")./*...*/.end();
node.on_publish(0x0101, &fill_drive_status, nullptr);   // once, next to the topic

// per tick(), instead of the table.due()/publish()/note_published() block above:
node.publish_subscribed_topics(now_ms);
```

`StaticNode<>::topic(topic_id, schema_version, name, fill, fill_ctx, ...)`
folds those two statements into one -- declaring a topic and saying how to
fill it become a single call (`example/sender.cpp`'s own shape, [§16.2](#162-using-it)):

```cpp
node.topic(0x0101, 3, "drive_status", &fill_drive_status)
    .f32("left_rpm", "rpm")./*...*/.end();
```

`fill` defaults to `nullptr` -- every call site written before this
parameter existed still compiles, declaring a topic with nothing
`on_publish()`-registered for it, same as calling the two-argument
`catalog().topic(...)` form directly.

`on_publish(topic_id, fill, ctx)` registers a `NodeNamedFillFn` for a topic
already in the served catalogue -- `StaticNode<>` has storage for the
registry ready with no setup call (sized by its own `CatalogTopics`
template argument); a raw `Node` needs `enable_publish_registry(slots,
slot_count)` first, the same "attach caller-owned storage" shape
`enable_subscriptions()` itself has. `publish_subscribed_topics(now_ms)`
walks every topic registered that way, `due()`-checks it against the
attached `SubscriptionTable`, and `publish_named()` + `note_published()`s
the ones that are -- returning how many it actually sent.

`receive()` answers a `SUBSCRIBE` against the served catalogue --
`NodeRx::SubscriptionServed` -- checking the topic is subscribable and
resolving `effective_rate_millihz` against its local rate policy (2.40.0):
capped at `max_rate_millihz` when set, else at `default_rate_millihz` (the
nominal rate for a non-periodic topic); a request resolving under
`min_rate_millihz` -- when set -- is REJECTED/INVALID_ARGUMENT rather than
granted anyway (§4's MUST NOT allows clamping down, never up). All three are
`Catalog::add_topic()`'s trailing arguments (or `TopicBuilder::min_rate()` /
`.default_rate()`) -- local policy only, none is a wire field. A second
`SUBSCRIBE` from the same requester for the same topic is a RENEWAL
(commands.md §4.3) and reuses the `subscription_id`. Before any of that,
every OTHER subscription this same `source_id` holds under a DIFFERENT
`boot_id` is evicted -- a rebooted peer has no session left to keep
publishing for. `table.due(topic_id, now_ms)` is true once any of that
topic's *local* subscribers is due by its own granted rate -- the fastest
one sets the cadence, and one `publish()` satisfies every subscriber of that
topic at once; `note_published()` resets it. `UNSUBSCRIBE` answers the same
way; removing an absent subscription is still success (commands.md §4.4).
`subscriber_count(topic_id)` / `aggregate_rate_millihz(topic_id)` (2.40.0)
read back the granted slots for a caller's own observability (e.g. a
per-topic STATUS block) without a parallel table of its own.

```cpp
// consumer:
btp::ClientSubscription slots[4];
btp::SubscriptionClient client(slots, 4);
node.enable_subscription_client(&client);

const std::uint32_t id = node.subscribe(robot_source_id, robot_boot_id,
                                        0x0101, 10000 /*mHz*/, 60000 /*ms*/);
// ... node.receive() feeds SUBSCRIBE_RESULT to it (NodeRx::SubscriptionHandled,
//     subscription_event() one of Granted / Rejected -- subscription_outcome()
//     is the full struct behind it, since 2.36.0: local_id/peer/topic/
//     requested_rate always set, plus effective_rate_millihz on Granted or
//     status/error_code on Rejected, mirroring command_outcome()), tick()
//     renews the grant at 80% of its lease on its own (another SUBSCRIBE,
//     commands.md §4.3) ...
node.unsubscribe(id);
```

`node.subscribe()` sends `SUBSCRIBE` and returns a local id stable across
renewals (0 on failure); `tick()` renews an `Active` subscription before its
lease runs out and expires one whose `SUBSCRIBE_RESULT` never arrived, with no
retry budget beyond that cadence -- the same "no retry budget" stance as
`connect()` ([§16.3](#163-the-session-initiator-connect)). `node.unsubscribe()`
sends `UNSUBSCRIBE` and drops the local state right away, without waiting for
`UNSUBSCRIBE_RESULT`.

### 16.7 Commands (`btp::DedupCache` / `btp::CommandClient`)

`COMMAND_REQUEST` / `COMMAND_RESULT` ([Commands and discovery
§2](commands.md#2-commands)) get the same responder / initiator split as
everything else in this chapter, both living in `btp::session`: the
RESPONDER side reuses `btp::DedupCache` ([§13](#13-the-session-layer)) --
already the executor's memory of "did I run this identity before" -- and the
INITIATOR side is `btp::CommandClient`, new alongside it.

On a `SizedNode<>` / `StaticNode<>`, overriding `has_command()` / `command()`
on the `NodeConfig` (§16.1) is enough -- the constructor reads
`cfg.has_command()` once and wires `command()` straight into its own
`btp::DedupCache`, no `enable_commands()` call needed. This is the ONE axis
that only takes effect there: `command()` needs a `DedupCache` a bare `Node`
has no storage to bind, so on a bare `Node`, `cfg.command()` is ignored and
`enable_commands(cache, handler, ctx)` below is the only way in, same as
always.

```cpp
// responder, a raw Node -- a SizedNode<>/StaticNode<> either overrides
// NodeConfig::command() (above) or calls its own two-argument
// enable_commands(handler, ctx) sugar, reaching its already-owned
// DedupCache without repeating &commands:
btp::DedupSlot slots[8]; std::uint8_t bytes[8][256]; btp::DedupStorage storage[8];
btp::DedupRequester requesters[4];
btp::DedupCache commands(slots, storage, 8, requesters, 4);
node.enable_commands(&commands, &arm_motors, &robot);

void arm_motors(void* ctx, std::uint16_t action_id, std::uint16_t version,
               btp::ByteView parameters, btp::NodeActionOutcome* outcome,
               const btp::NodeCommandTicket& ticket) {
    // ... do it, synchronously ...
    outcome->status = static_cast<std::uint8_t>(btp::ResultStatus::Success);
}
```

`receive()` classifies a `COMMAND_REQUEST` against the cache
(`NodeRx::CommandServed` either way): Fresh runs the handler once;
`DuplicateComplete` replays the stored result verbatim, no second run;
`DuplicateInFlight` drops silently (the peer retries); `Conflict` /
`Evicted` / `CapacityExhausted` get an automatic `REJECTED` / `BUSY` reply.

A Fresh handler answers two ways. Synchronously (the original shape): fill
`outcome` and return -- the node builds and sends `COMMAND_RESULT` from it
and records it, same as always. Or asynchronously (`NodeActionOutcome::
pending`, 2.42.0): set `outcome->pending = true` and return having saved a
copy of `ticket` (a plain POD -- the original request's `Header`, the action
id/version, and its reserved `DedupCache` slot) -- nothing is sent yet, and a
retransmission in the meantime classifies `DuplicateInFlight` exactly as it
would during a slow synchronous call, just held open longer. Once the real
work finishes, on any task, `node.complete_command(ticket, real_outcome)`
does what returning synchronously would have:

```cpp
void arm_motors(void* ctx, std::uint16_t action_id, std::uint16_t version,
               btp::ByteView parameters, btp::NodeActionOutcome* outcome,
               const btp::NodeCommandTicket& ticket) {
    outcome->pending = true;
    queue_for_the_motor_task(ticket, action_id, parameters);  // returns now
}

// ... later, on the motor task, once it actually finishes:
btp::NodeActionOutcome done = {};
done.status = static_cast<std::uint8_t>(btp::ResultStatus::Success);
node.complete_command(saved_ticket, done);
```

`complete_command()` returns `false` and sends nothing for a ticket that is
not `valid()` (default-constructed) or was already spent -- calling it twice
on the same ticket only sends the first time, the second finds the slot no
longer `Reserved` and stops there.

```cpp
// initiator:
btp::ClientCommand slots[4];
btp::CommandClient commands(slots, 4);
node.enable_command_client(&commands);

const std::uint32_t id = node.command(robot_source_id, robot_boot_id, kArmMotors,
                                      /*version=*/1, params, params_size);
// ... node.receive() feeds COMMAND_RESULT to it (NodeRx::CommandHandled,
//     node.command_outcome() -- status / error_code / message / result,
//     valid the same "until the next receive()" way ReceivedMessage::payload
//     is), tick() times it out after kCommandTimeoutMs with no retry --
//     the same stance connect() / subscribe() take ...
```

### 16.8 STATUS (`node.enable_status()`)

`node.enable_status(period_ms)` builds and sends a v1 `STATUS`
([Commands and discovery §5](commands.md#5-status)) from counters the node
already keeps -- `receiver().stats()`, `frames_tx()`, and, once attached,
`commands()`'s replay count -- on a cadence `tick()` drives, the same "cadence
bounds how late it's noticed" rule as the session watchdog. `period_ms == 0`
(the default) disables it. Spontaneous like the wire format itself: no
result, no correlation.

Best-effort, not exact -- documented in full on `enable_status()`: `frames_rx`
/ `reassembly_completed` both read the reassembled-logical-message counter
(the closest thing to "a frame" this layer keeps), `frames_tx` only counts
`send()` / `send_with()` (not the bootstrap `HELLO` / `SUBSCRIBE` /
`COMMAND_REQUEST` traffic), and `telemetry_dropped` is not tracked separately
yet (reads 0).

`node.enable_status_topics(callback, ctx)` (2.41.0) upgrades the next
`emit_status()` from plain v1 to v2
([Commands and discovery §5.2](commands.md#52-status-version-2)): one
`TopicStatusRecord` per topic of the **served** catalog (`serve_catalog()`)
that currently has at least one active subscriber
(`enable_subscriptions()`). `source_id` (this node's own), `subscriber_count`
and `effective_rate_millihz` all come from the attached `SubscriptionTable`
directly -- `callback` is only asked for the two counters genuinely outside
this layer's own state, `bytes_total` and `samples_dropped_total`, the same
boundary `publish()` itself draws around the caller's own TX path. At most
`Node::kMaxStatusTopics` (8) topics are reported per emission, in the served
catalog's own order; a catalog with more than that concurrently subscribed
has the rest silently left out of *that* message. No callback attached, no
served catalog, no subscription table, or simply nothing subscribed right
now: falls back to v1, unchanged from before this existed.

### 16.9 What stays out

Everything §11 keeps above the wire, unchanged: **routing policy** (`receive()`
hands back a message it does not manage; relay-or-drop is the caller's one
switch), the **hub subscription aggregator** (one subscription upstream per N
downstream subscribers of the same source + topic -- §16.6 is the per-node
piece; a hub layers its aggregation on top via `Node::subscriptions()`), the
priority scheduler, and **key derivation** (the body of your `seal` / `open`).
§16.3's `connect()` is only the `HELLO` → `HELLO_RESULT` handshake -- a
subscription and a command are still the caller's to make, once `connected()`
is true. Plus: **link framing** — a `Node` does not own COBS / HID-report
de-padding / a serial byte stream. `receive(datagram, size)` wants one whole
BTP frame; a caller that owns its own framing (feeds bytes through
`btp::SerialDecoder`, de-pads a HID report) hands the decoded frame to
`receive(const DecodedFrame&, …)` (2.35.0) instead, and gets the identical
session / reassembly / discovery path. A single encoded frame must still fit
the ESP-NOW ceiling — large-Serial TX is a later addition.

---

## 17. Putting the library together

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

`btp::Node` ([§16](#16-the-node-layer)) is the transmit path, the receive path
and the responder session above bundled into one object with one identity, one
`now_ms` and one set of buffers -- for a packet transport, and with every
dependency a function pointer. A caller that wants the layers by hand still has
them, and `node.endpoint()` / `node.receiver()` / `node.session()` reach them
through the facade.

---

## 18. Summary

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
the MANIFEST_DATA schema catalogue, caller-owned   (btp::catalog)
endpoint + receiver + session + catalogue in one object   (btp::node)
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