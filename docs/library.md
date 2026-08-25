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

The reference implementation also deliberately does not implement every higher-level mechanism described by the BTP specification.

For example, the protocol defines:

* sessions;
* `HELLO`;
* manifests;
* telemetry schemas;
* command execution;
* command deduplication;
* subscriptions;
* priority queues.

Those mechanisms use the BTP frame format, but their application state is not owned by the current reference codec library.

This distinction is important when considering memory usage.

---

## 1. The four headers

The public API is divided into four headers.

| Header                  | Responsibility                                         |
| ----------------------- | ------------------------------------------------------ |
| `btp/codec.hpp`         | Frame encoding, decoding, CRC and header serialization |
| `btp/fragmentation.hpp` | Fragmentation and logical-message reassembly           |
| `btp/stream.hpp`        | COBS and incremental serial decoding                   |
| `btp/aead.hpp`          | Optional version-2 authenticated encryption            |

The implementation is exposed through two CMake targets.

### `btp::codec`

`btp::codec` contains:

```text
codec.cpp
fragmentation.cpp
stream.cpp
```

It has no external dependency.

Applications that do not require BTP encryption can use only this target.

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
       +-------------+-------------+
       |             |             |
     codec     fragmentation      stream


                  btp::aead
                     |
                     v
                 mbedcrypto
```

Encryption is therefore optional at build time.

The basic frame codec does not require the cryptographic component.

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
test-vectors/v1/
test-vectors/v2/
```

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
| Library version    | `2.0.0`        | Version of the reference implementation |
| Release tag        | `v1.1.0-beta`  | Repository release                      |
| Maintenance branch | `1.x`          | Source branch maintaining a major line  |

The wire version is transmitted inside every BTP frame.

The library version is not.

For example:

```text
library 2.0.0
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
BTP library 2.0.0
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

The reference library intentionally stops below several BTP protocol mechanisms.

Understanding this boundary prevents functionality defined by the specification from being mistaken for functionality automatically provided by the C++ library.

---

### 11.1 Session logic is not implemented by the codec

The BTP specification defines:

```text
HELLO
HELLO_RESULT

session lifetime
watchdog

SESSION_CLOSE
SESSION_CLOSE_RESULT
```

The current reference codec can encode and decode the BTP frames carrying those payloads, but it does not contain a complete session manager.

The application integrating BTP maintains that higher-level state.

---

### 11.2 Manifest storage is not implemented by the codec

The specification defines:

```text
MANIFEST_REQUEST
MANIFEST_DATA
```

and the structure of topic, field and action descriptors.

The current reference codec does not maintain an internal catalog of discovered schemas.

After decoding and reassembling a manifest message, descriptor parsing and storage belong to the higher-level implementation.

Therefore:

```text
BTP has runtime discovery
```

does not mean:

```text
btp::codec automatically owns
a dynamic manifest database
```

There is no contradiction with the library's no-allocation guarantee.

---

### 11.3 Telemetry schema interpretation is above the frame codec

`btp::decode()` exposes the logical BTP payload.

It does not automatically convert a telemetry payload into application variables according to a manifest schema.

Conceptually:

```text
BTP frame
   |
   v
btp::decode()
   |
   v
logical telemetry payload
   |
   v
schema-aware application layer
```

The wire representation of telemetry is specified by BTP.

The schema-aware object model used by a particular application is not stored inside the frame codec.

---

### 11.4 Command execution and deduplication are above the codec

BTP specifies:

* command request identity;
* action versions;
* command results;
* retry behavior;
* deduplication rules.

The reference codec provides the framing required to transport those messages.

It does not:

* execute application actions;
* maintain the deduplication cache;
* track command state.

Those mechanisms are implemented by the BTP integration above the codec.

---

### 11.5 Subscriptions and priority queues are above the codec

The protocol defines:

```text
SUBSCRIBE
SUBSCRIBE_RESULT
UNSUBSCRIBE
STATUS
```

and the BTP priority classes.

The current codec library does not own a telemetry scheduler or a priority queue.

The transport integration applies those rules when scheduling outgoing messages.

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

## 12. Putting the library together

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

according to the corresponding BTP chapter.

---

## 13. Summary

The BTP reference library implements the protocol mechanisms closest to the wire:

```text
frame serialization
CRC validation
transport limits
fragmentation
reassembly
serial COBS
optional AEAD
```

It deliberately does not own the surrounding application.

In particular, the library does not own:

```text
transport I/O

manifest database

telemetry object storage

session manager

command executor

command deduplication table

subscription scheduler

priority queues
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