# The datagram

This chapter is the wire contract. Everything here is what
[`write_header()`](../src/codec.cpp) writes and what
[`decode()`](../src/codec.cpp) checks — those two functions are the single
source of truth, and this text tracks them.

## 1. Shape

A frame is three parts with nothing between them:

```text
+---------------------------+------------------------+------------+
| header (36 octets)        | payload (N octets)     | CRC32 (4)  |
+---------------------------+------------------------+------------+
0                           36                       36+N     40+N
```

So `frame_size = 40 + N`. There is no padding, no alignment, no terminator and
no escape character inside the frame. Framing for the byte-stream transports is
added *around* the frame by the transport profile, never inside it.

`N` is the payload of **this frame**. For a fragmented message it is the size
of this fragment, not of the whole logical message.

## 2. The header, octet by octet

| Offset | Size | Field | Wire type | Notes |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | `magic` | bytes | Always `42 54 50 00`, i.e. `"BTP\0"` |
| 4 | 1 | `version` | `uint8` | **Derived, not chosen** — see 2.1 |
| 5 | 1 | `type` | `uint8` | The logical channel, `0x01`..`0x05` |
| 6 | 2 | `flags` | `uint16_le` | See 3 |
| 8 | 2 | `header_size` | `uint16_le` | Always `36` (`0x0024`) |
| 10 | 2 | `payload_size` | `uint16_le` | This frame only, excludes header and CRC |
| 12 | 4 | `source_id` | `uint32_le` | Non-zero |
| 16 | 4 | `boot_id` | `uint32_le` | Non-zero, changes every boot |
| 20 | 4 | `sequence` | `uint32_le` | Per logical message |
| 24 | 8 | `timestamp_us` | `uint64_le` | Source monotonic clock |
| 32 | 2 | `object_id` | `uint16_le` | Namespaced by `type` |
| 34 | 1 | `fragment_index` | `uint8` | |
| 35 | 1 | `fragment_count` | `uint8` | |

The magic is a four-octet sequence, deliberately not an integer, so it has no
byte order to get wrong.

`header_size` is present and fixed at 36. It is not there to allow a variable
header — a decoder rejects any other value outright. It is there so a frame
whose header length disagrees with the reader's expectation fails immediately
and legibly instead of being parsed at the wrong offsets.

### 2.1 The version octet is derived

`version` is not a field the caller sets. The encoder computes it:

- `0x02` when the `ENCRYPTED` flag is set
- `0x01` otherwise

This means the wire cannot express "encrypted but version 1". A decoder that
sees `ENCRYPTED` with a version other than 2 rejects the frame.

The rule is deliberately asymmetric in the other direction: `version == 0x02`
with `ENCRYPTED` clear **is valid**. Version 2 means "this peer speaks the v2
envelope", not "this frame is encrypted".

## 3. Flags

```text
bit   15                            4   3   2   1   0
     +---------------------------+-------+---+---+---+
     |        reserved           | CIPHER_ID | E | F |
     +---------------------------+-------+---+---+---+
```

| Mask | Name | Meaning |
| --- | --- | --- |
| `0x0001` | `FRAGMENTED` | This frame is one fragment of a larger logical message |
| `0x0002` | `ENCRYPTED` | The payload is AEAD ciphertext followed by a 16-octet tag |
| `0x000C` | `CIPHER_ID` | Two-bit enum at bits 2-3, see below |
| `0xFFF0` | reserved | Must be zero; a non-zero reserved bit is a rejection |

`CIPHER_ID` is **one two-bit enum, not two independent boolean flags**. That
choice matters: with two booleans the wire could express "both ciphers marked",
an ambiguous state a decoder would have to invent a rule for. As an enum it
cannot.

| Value | Cipher |
| ---: | --- |
| `0` | AES-128-GCM (the default) |
| `1` | ChaCha20-Poly1305 |
| `2`, `3` | Reserved — rejected |

With `ENCRYPTED` clear there is no cipher in use, so `CIPHER_ID` must be `0`. A
non-zero `CIPHER_ID` without `ENCRYPTED` is rejected rather than ignored.

## 4. Little-endian, field by field

Every multi-octet integer on the wire is little-endian. That includes the
trailing CRC.

The encoder writes each field individually, one octet at a time:

```cpp
void write_u16_le(std::uint8_t* destination, std::uint16_t value) noexcept {
    destination[0] = static_cast<std::uint8_t>(value);
    destination[1] = static_cast<std::uint8_t>(value >> 8U);
}
```

Nothing ever `memcpy`s a struct onto the wire, and no size is ever taken from
`sizeof`, from alignment, or from an implicit enum type. This is what removes
the ABI from the contract: the same octets are produced and consumed by a
microcontroller and a desktop, in different languages, with nothing to
negotiate. The in-memory `btp::Header` carries no packing attribute precisely
because its layout is irrelevant — only the serializer's output is.

## 5. CRC-32

The trailing four octets are CRC-32/ISO-HDLC, also known as CRC-32/IEEE — the
same one `zlib.crc32` computes.

| Parameter | Value |
| --- | --- |
| Polynomial | `0xEDB88320` (reflected form of `0x04C11DB7`) |
| Initial value | `0xFFFFFFFF` |
| Reflect in / out | true / true |
| Final XOR | `0xFFFFFFFF` |
| Check, over `"123456789"` | `0xCBF43926` |

It covers bytes `[0, 36+N)` — the header and the payload, not itself — and is
written little-endian at offset `36+N`.

The CRC is **per frame, which means per fragment**. A fragmented message has
one CRC per fragment and none over the reassembled whole. Integrity of the
logical message, when that is needed, comes from the AEAD tag instead.

A CRC detects accidental corruption. It is not authentication and does not
pretend to be: anyone who can change the payload can recompute it. See
[Encryption](encryption.md).

## 6. How a decoder validates

The order below is the order the code actually applies, and it is observable:
which error you get for a frame that is wrong in two ways depends on it.

1. Null arguments or an unknown transport profile → `InvalidArgument`
2. Fewer than 40 octets → `FrameTooShort`
3. More than the profile's frame limit → `FrameTooLarge`
4. Magic is not `42 54 50 00` → `InvalidMagic`
5. `version` outside the supported range → `UnsupportedVersion`
6. `header_size` is not 36 → `InvalidHeaderSize`
7. `payload_size` above the profile's payload limit → `PayloadTooLarge`
8. `input_size` is not `40 + payload_size` → `SizeMismatch`
9. Computed CRC differs from the stored one → `CrcMismatch`
10. `ENCRYPTED` set with `version != 2` → `EncryptedVersionMismatch`
11. `ENCRYPTED` set on the USB HID profile → `EncryptedNotAllowedOnTransport`
12. Header invariants, in this order:
    - `type` outside `0x01`..`0x05` → `InvalidType`
    - any reserved flag bit set → `InvalidFlags`
    - `CIPHER_ID` inconsistent or reserved → `InvalidCipherId`
    - `source_id == 0` → `InvalidSourceId`
    - `boot_id == 0` → `InvalidBootId`
    - fragmentation fields inconsistent → `InvalidFragmentation`
13. Success: the payload is a view into the caller's input buffer

Two things are worth noting about this list.

**The CRC check comes before the semantic checks.** A frame whose version octet
was corrupted in transit reports `CrcMismatch`, not `UnsupportedVersion`,
because the corruption is what actually happened. To build a test vector that
exercises a semantic rejection you have to recompute the CRC after mutating the
field.

**The fragmentation invariant has two halves.** With `FRAGMENTED` set,
`fragment_count` must be at least 2 and `fragment_index` must be below it. With
`FRAGMENTED` clear, `fragment_index` must be 0 and `fragment_count` must be
exactly 1 — not 0. A zero-initialized header is therefore not a valid one,
which is the single most common mistake when first using the library.

## 7. A worked example

A `LOG` message with no payload, `source_id = 0x11223344`,
`boot_id = 0xA1B2C3D4`, `sequence = 1`, `timestamp_us = 1000000`,
`object_id = 2`:

```text
42 54 50 00   magic "BTP\0"
01            version 1 (ENCRYPTED is clear)
02            type = LOG
00 00         flags = 0x0000
24 00         header_size = 36
00 00         payload_size = 0
44 33 22 11   source_id = 0x11223344
d4 c3 b2 a1   boot_id = 0xA1B2C3D4
01 00 00 00   sequence = 1
40 42 0f 00 00 00 00 00   timestamp_us = 1000000
02 00         object_id = 2
00            fragment_index = 0
01            fragment_count = 1
3a 15 e7 df   CRC32 = 0xDFE7153A
```

As a continuous 40-octet frame:

```text
42 54 50 00 01 02 00 00 24 00 00 00 44 33 22 11
d4 c3 b2 a1 01 00 00 00 40 42 0f 00 00 00 00 00
02 00 00 01 3a 15 e7 df
```

Note `fragment_count = 1` on an unfragmented message, and the CRC appearing
byte-reversed relative to how the value is written — both are the rules above
in practice. These are the exact octets of `kEmptyLogVector` in
[`tests/test_codec.cpp`](../tests/test_codec.cpp), which the test suite
re-encodes and compares byte for byte, and then mutates one field at a time to
check every rejection in section 6.

## 8. The payload is opaque to the envelope

The envelope carries `payload_size` octets and asks nothing about them. `0x00`,
CR and LF are ordinary data. A payload may contain a byte sequence that looks
like another BTP header; it means nothing.

What the bytes mean is decided by `type` and `object_id` together, and is
specified in [Telemetry payloads](telemetry.md),
[Commands and discovery](commands.md) and
[Session and terminal](session-and-terminal.md).
