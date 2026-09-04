# The datagram

This chapter defines the BTP frame format and the validation rules applied to received frames.

A BTP frame is the unit exchanged with a transport. A logical message may consist of one frame or, when fragmentation is required, multiple frames.

## 1. Frame structure

A BTP frame consists of three contiguous regions:

```text
+---------------------------+------------------------+------------+
| Header                    | Payload                | CRC-32     |
| 36 octets                 | N octets               | 4 octets   |
+---------------------------+------------------------+------------+
0                           36                       36+N       40+N
```

The total frame size is:

```text
frame_size = 36 + N + 4
           = 40 + N
```

where `N` is the payload size of the current frame.

There is no padding or alignment between fields.

The frame itself does not contain transport delimiters, escape sequences, or terminators. The transport itself may add external framing when required.

For a fragmented logical message, `N` represents the payload carried by the current fragment, not the complete logical payload.

---

## 2. Header format

The BTP header has a fixed size of 36 octets.

| Offset | Size | Field            | Wire type   | Description                     |
| -----: | ---: | ---------------- | ----------- | ------------------------------- |
|      0 |    4 | `magic`          | bytes       | BTP frame identifier            |
|      4 |    1 | `version`        | `uint8`     | Wire-format version             |
|      5 |    1 | `type`           | `uint8`     | Logical message channel         |
|      6 |    2 | `flags`          | `uint16_le` | Frame flags                     |
|      8 |    2 | `header_size`    | `uint16_le` | Header size in octets           |
|     10 |    2 | `payload_size`   | `uint16_le` | Payload size of this frame      |
|     12 |    4 | `source_id`      | `uint32_le` | Producer identifier             |
|     16 |    4 | `boot_id`        | `uint32_le` | Producer boot identifier        |
|     20 |    4 | `sequence`       | `uint32_le` | Logical message sequence        |
|     24 |    8 | `timestamp_us`   | `uint64_le` | Source monotonic timestamp      |
|     32 |    2 | `object_id`      | `uint16_le` | Object identifier within `type` |
|     34 |    1 | `fragment_index` | `uint8`     | Fragment index                  |
|     35 |    1 | `fragment_count` | `uint8`     | Total fragment count            |

All multi-octet integer fields are encoded in little-endian order.

### 2.1 `magic`

The first four octets are:

```text
42 54 50 00
```

which corresponds to:

```text
"BTP\0"
```

`magic` is defined as a four-octet sequence, not as an integer value.

A decoder must reject a frame if these octets do not match the BTP magic sequence.

### 2.2 `version`

`version` identifies the BTP wire format used by the frame.

The currently supported values are:

|  Value | Meaning                |
| -----: | ---------------------- |
| `0x01` | BTP version 1 envelope |
| `0x02` | BTP version 2 envelope |

The reference encoder selects the version automatically:

* `0x01` when `ENCRYPTED` is clear;
* `0x02` when `ENCRYPTED` is set.

An encrypted frame must use version 2.

Therefore:

```text
ENCRYPTED = 1  -> version MUST be 0x02
```

A decoder may accept a version-2 frame with `ENCRYPTED` clear.

Therefore, version 2 indicates support for the version-2 envelope; it does not by itself indicate that the payload is encrypted.

Unsupported versions are rejected.

### 2.3 `type`

`type` identifies the logical channel associated with the frame.

|  Value | Type        |
| -----: | ----------- |
| `0x01` | `TELEMETRY` |
| `0x02` | `LOG`       |
| `0x03` | `COMMAND`   |
| `0x04` | `TERMINAL`  |
| `0x05` | `CONTROL`   |

`0x00` is invalid.

Values not assigned by the current protocol version are reserved and must be rejected.

The payload interpretation depends on both:

```text
(type, object_id)
```

### 2.4 `flags`

`flags` is a 16-bit field.

```text
bit   15                         4    3   2   1   0
     +----------------------------+-------+---+---+
     |          reserved          |  CID  | E | F |
     +----------------------------+-------+---+---+
```

The currently defined bits are:

| Mask     | Name         | Meaning                                       |
| -------- | ------------ | --------------------------------------------- |
| `0x0001` | `FRAGMENTED` | Frame belongs to a fragmented logical message |
| `0x0002` | `ENCRYPTED`  | Payload contains authenticated encrypted data |
| `0x000C` | `CIPHER_ID`  | Cipher identifier                             |
| `0xFFF0` | Reserved     | Must be zero                                  |

Reserved bits must be zero.

A frame containing an unsupported reserved flag is rejected.

### 2.5 `CIPHER_ID`

`CIPHER_ID` is a two-bit enumeration stored in bits 2 and 3 of `flags`.

| Value | Cipher            |
| ----: | ----------------- |
|   `0` | AES-128-GCM       |
|   `1` | ChaCha20-Poly1305 |
|   `2` | Reserved          |
|   `3` | Reserved          |

When `ENCRYPTED` is clear:

```text
CIPHER_ID MUST be 0
```

When `ENCRYPTED` is set, `CIPHER_ID` selects the authenticated-encryption algorithm.

Reserved cipher identifiers are rejected.

Encryption behavior is defined in [Encryption](encryption.md).

### 2.6 `header_size`

`header_size` is encoded as:

```text
36
```

or:

```text
0x0024
```

The current BTP frame format uses a fixed 36-octet header.

A decoder must reject any frame whose `header_size` differs from 36.

The field provides an explicit consistency check between the received frame and the header layout expected by the decoder.

### 2.7 `payload_size`

`payload_size` defines the number of payload octets contained in the current frame.

It excludes:

* the 36-octet header;
* the 4-octet CRC.

Therefore:

```text
frame_size = 40 + payload_size
```

For a fragmented message, `payload_size` describes only the current fragment.

The maximum permitted value may be further restricted by the selected `TransportLimits`.

### 2.8 `source_id`

`source_id` is a non-zero 32-bit identifier associated with the producer of the logical message.

```text
source_id != 0
```

Its provisioning is external to BTP.

A gateway forwarding an existing logical message preserves its original `source_id`.

### 2.9 `boot_id`

`boot_id` is a non-zero 32-bit identifier associated with one execution of a producer.

```text
boot_id != 0
```

A producer must use a new `boot_id` after restart.

Together with `source_id` and `sequence`, it forms the logical message identity:

```text
(source_id, boot_id, sequence)
```

### 2.10 `sequence`

`sequence` is a 32-bit identifier assigned to a logical message.

All frames belonging to the same fragmented logical message use the same `sequence`.

The value applies to the logical message rather than to an individual fragment.

### 2.11 `timestamp_us`

`timestamp_us` is a 64-bit timestamp expressed in microseconds.

It is generated from the producer's monotonic clock and represents the source time associated with the logical message.

Gateways preserve this field when forwarding a message.

BTP does not interpret this value as wall-clock time.

### 2.12 `object_id`

`object_id` is a 16-bit identifier whose namespace is defined by `type`.

The same numerical `object_id` may therefore identify different objects in different logical channels.

For example:

```text
(TELEMETRY, 5)
```

and:

```text
(COMMAND, 5)
```

are independent identifiers.

Object semantics are defined by the corresponding message-type chapters.

### 2.13 `fragment_index` and `fragment_count`

These fields describe fragmentation.

For an unfragmented logical message:

```text
FRAGMENTED     = 0
fragment_index = 0
fragment_count = 1
```

For a fragmented logical message:

```text
FRAGMENTED     = 1
fragment_count >= 2
fragment_index < fragment_count
```

Fragment indexes start at zero.

For example, a logical message divided into three frames uses:

```text
fragment_index = 0, fragment_count = 3
fragment_index = 1, fragment_count = 3
fragment_index = 2, fragment_count = 3
```

All fragments of the same logical message preserve the logical-message fields defined by the fragmentation specification.

See [Getting it across the link](fragmentation-and-transports.md).

---

## 3. Byte order

All multi-octet integer fields use little-endian encoding.

For example, the 32-bit value:

```text
0x11223344
```

is transmitted as:

```text
44 33 22 11
```

The same rule applies to:

* `flags`;
* `header_size`;
* `payload_size`;
* `source_id`;
* `boot_id`;
* `sequence`;
* `timestamp_us`;
* `object_id`;
* CRC-32.

The wire format is independent of native processor endianness.

Implementations must serialize fields according to their specified wire representation rather than transmitting the memory representation of native structures.

For example:

```cpp
void write_u16_le(std::uint8_t* destination, std::uint16_t value) noexcept {
    destination[0] = static_cast<std::uint8_t>(value);
    destination[1] = static_cast<std::uint8_t>(value >> 8U);
}
```

The layout of an in-memory C or C++ structure is not part of the BTP wire format.

---

## 4. Payload

The frame envelope treats the payload as an opaque sequence of octets.

```text
payload = payload_size octets
```

No byte value has special meaning to the frame layer.

Values such as:

```text
00
0A
0D
42 54 50 00
```

may appear anywhere inside the payload.

A sequence inside the payload that matches the BTP magic value does not start another frame.

Payload semantics are defined by:

```text
type + object_id
```

and by the message format associated with that object.

Payload definitions are specified in the corresponding chapters:

* [Telemetry payloads](telemetry.md);
* [Commands and discovery](commands.md);
* [Session and terminal](session-and-terminal.md).

---

## 5. CRC-32

Every BTP frame ends with a four-octet CRC-32.

BTP uses CRC-32/ISO-HDLC, also commonly identified as CRC-32/IEEE.

| Parameter                     | Value        |
| ----------------------------- | ------------ |
| Polynomial                    | `0x04C11DB7` |
| Reflected polynomial          | `0xEDB88320` |
| Initial value                 | `0xFFFFFFFF` |
| Reflect input                 | `true`       |
| Reflect output                | `true`       |
| Final XOR                     | `0xFFFFFFFF` |
| Check value for `"123456789"` | `0xCBF43926` |

The CRC covers:

```text
header + payload
```

and does not include the CRC field itself.

For a payload of `N` octets, CRC coverage is:

```text
[0, 36 + N)
```

The resulting 32-bit CRC is encoded in little-endian order at offset:

```text
36 + N
```

### 5.1 CRC and fragmentation

CRC validation is performed independently for every frame.

A fragmented logical message therefore contains one CRC for each fragment.

```text
Fragment 0 -> header + payload + CRC
Fragment 1 -> header + payload + CRC
Fragment 2 -> header + payload + CRC
```

The frame CRC does not provide a second CRC over the complete reassembled logical payload.

### 5.2 CRC and security

CRC-32 detects accidental corruption.

It does not provide cryptographic authentication.

A device capable of deliberately modifying a frame can also calculate a new valid CRC.

When cryptographic integrity or confidentiality is required, BTP uses authenticated encryption as defined in [Encryption](encryption.md).

---

## 6. Frame validation

A received frame must pass structural, integrity, transport, and header validation before its payload is delivered to the application.

The reference decoder performs validation in the following order:

1. validate input arguments and `TransportLimits`;
2. verify the minimum frame size;
3. verify the maximum size permitted by `TransportLimits`;
4. verify `magic`;
5. verify that `version` is supported;
6. verify `header_size`;
7. verify `payload_size` against the selected `TransportLimits`;
8. verify the total frame size;
9. verify CRC-32;
10. verify version and encryption consistency;
11. verify transport-specific restrictions;
12. verify header invariants;
13. expose the decoded payload.

The corresponding errors are:

| Condition                                  | Error                            |
| ------------------------------------------ | -------------------------------- |
| Invalid argument or `TransportLimits`      | `InvalidArgument`                |
| Frame smaller than 40 octets               | `FrameTooShort`                  |
| Frame exceeds transport limit              | `FrameTooLarge`                  |
| Invalid magic sequence                     | `InvalidMagic`                   |
| Unsupported version                        | `UnsupportedVersion`             |
| `header_size != 36`                        | `InvalidHeaderSize`              |
| Payload exceeds transport limit            | `PayloadTooLarge`                |
| `input_size != 40 + payload_size`          | `SizeMismatch`                   |
| Invalid CRC                                | `CrcMismatch`                    |
| `ENCRYPTED` with incompatible version      | `EncryptedVersionMismatch`       |
| Encryption prohibited by `TransportLimits` | `EncryptedNotAllowedOnTransport` |
| Invalid message type                       | `InvalidType`                    |
| Reserved flag set                          | `InvalidFlags`                   |
| Invalid `CIPHER_ID`                        | `InvalidCipherId`                |
| `source_id == 0`                           | `InvalidSourceId`                |
| `boot_id == 0`                             | `InvalidBootId`                  |
| Invalid fragmentation fields               | `InvalidFragmentation`           |

### 6.1 CRC validation precedes semantic validation

CRC validation is performed before most semantic header checks.

This distinction separates accidental corruption from a structurally intact but semantically invalid frame.

For example, if a header field is modified during transmission without updating the CRC:

```text
CRC validation -> failure
```

If the same field contains an invalid value but the CRC correctly represents the received bytes:

```text
CRC validation      -> success
semantic validation -> failure
```

This ordering is also relevant when constructing negative conformance vectors. A vector intended to test a semantic rejection must contain a valid CRC for the intentionally invalid frame.

### 6.2 Fragmentation validation

Fragmentation fields are validated together.

The following combination is valid for an unfragmented frame:

```text
FRAGMENTED     = 0
fragment_index = 0
fragment_count = 1
```

The following conditions are required for a fragmented frame:

```text
FRAGMENTED     = 1
fragment_count >= 2
fragment_index < fragment_count
```

Any other combination is rejected.

---

## 7. Encoding requirements

An implementation generating a BTP frame must:

1. validate the header fields;
2. validate restrictions imposed by the selected `TransportLimits`;
3. determine the payload size;
4. verify that the payload fits the selected `TransportLimits`;
5. serialize the 36-octet header;
6. append the payload;
7. calculate CRC-32 over the header and payload;
8. append the CRC in little-endian order.

The encoded frame is therefore:

```text
frame =
    header
    || payload
    || crc32(header || payload)
```

where `||` denotes byte concatenation.

Implementations must not derive the wire format from:

* `sizeof(struct)`;
* native alignment;
* structure packing;
* native byte order;
* compiler-specific enum size.

---

## 8. Example frame

Consider an unencrypted `LOG` frame with no payload:

```text
source_id    = 0x11223344
boot_id      = 0xA1B2C3D4
sequence     = 1
timestamp_us = 1000000
object_id    = 2
```

The encoded frame is:

```text
42 54 50 00   magic
01            version = 1
02            type = LOG
00 00         flags = 0x0000
24 00         header_size = 36
00 00         payload_size = 0
44 33 22 11   source_id = 0x11223344
d4 c3 b2 a1   boot_id = 0xA1B2C3D4
01 00 00 00   sequence = 1
40 42 0f 00
00 00 00 00   timestamp_us = 1000000
02 00         object_id = 2
00            fragment_index = 0
01            fragment_count = 1
3a 15 e7 df   CRC32 = 0xDFE7153A
```

As a continuous 40-octet sequence:

```text
42 54 50 00 01 02 00 00 24 00 00 00 44 33 22 11
d4 c3 b2 a1 01 00 00 00 40 42 0f 00 00 00 00 00
02 00 00 01 3a 15 e7 df
```

This is the minimum BTP frame size because the payload contains zero octets.

The example also shows the required fragmentation fields for an unfragmented message:

```text
fragment_index = 0
fragment_count = 1
```

---

## 9. Frame summary

The BTP wire frame has the following form:

```text
+------------------+
| 36-octet header  |
+------------------+
| N-octet payload  |
+------------------+
| 4-octet CRC-32   |
+------------------+
```

Its minimum size is 40 octets.

The header provides:

* protocol version;
* logical message type;
* frame flags;
* payload size;
* source identity;
* boot identity;
* message sequence;
* source timestamp;
* object identification;
* fragmentation information.

The payload is opaque to the frame layer.

CRC-32 validates the integrity of each transmitted frame against accidental corruption.

The selected `TransportLimits` define additional constraints such as maximum frame and payload sizes.

Higher-level chapters define payload semantics, fragmentation, authenticated encryption, commands, telemetry, and protocol control.
