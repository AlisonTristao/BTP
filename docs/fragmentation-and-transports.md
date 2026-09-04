# Fragmentation and transports

BTP separates a logical message from the transport used to carry it.

The BTP frame format remains unchanged between transports. A transport is described to the codec as `btp::TransportLimits` -- not a fixed, closed list of named profiles, but two sizes and one policy bit:

```cpp
struct TransportLimits {
    std::size_t max_frame_size;
    std::size_t max_payload_size;
    bool allow_encrypted;
};
```

* `max_frame_size` -- the maximum frame size;
* `max_payload_size` -- the maximum payload carried by one frame;
* `allow_encrypted` -- whether this transport may carry an ENCRYPTED frame at all (section 7.3 below is why this exists as its own field, not something derived from the sizes).

How BTP frames are delimited or encapsulated on the link (section 6.1's COBS framing, section 7's HID report) is NOT part of `TransportLimits` -- it is caller code around the bytes the codec produces/consumes, described per transport below.

Three ready-made presets cover the transports this document describes -- `btp::kEspNowTransport`, `btp::kSerialTransport`, `btp::kUsbHidTransport` -- and a caller with a different link builds its own `TransportLimits` for it; there is no enum to extend.

When a logical message exceeds the payload capacity of the selected transport, BTP divides it into multiple frames.

## 1. Fragmentation

A logical message may be transmitted as one frame or as multiple fragments.

```text
Logical message
      |
      | payload fits
      +-----------------------> one BTP frame
      |
      | payload exceeds limit
      v
+------------+
| Fragment 0 |
+------------+
| Fragment 1 |
+------------+
|     ...    |
+------------+
| Fragment N |
+------------+
```

Each fragment is a complete BTP frame containing:

* the 36-octet BTP header;
* part of the logical payload;
* its own CRC-32.

Each fragment is validated independently before reassembly.

### 1.1 Fragment generation

The maximum payload carried by one frame is defined by the selected transport profile.

For a transport with payload limit `L`, the number of fragments required for a logical payload of size `S` is:

```text
fragment_count = ceil(S / L)
```

with a minimum value of 1.

If only one frame is required:

```text
FRAGMENTED     = 0
fragment_index = 0
fragment_count = 1
```

If more than one frame is required:

```text
FRAGMENTED     = 1
fragment_count >= 2
```

Fragments are indexed from zero:

```text
0, 1, 2, ..., fragment_count - 1
```

The reference fragmenter fills each fragment up to the transport payload limit before generating the next fragment.

For example, a 500-octet logical payload transmitted through a profile with a 210-octet payload limit produces:

```text
Fragment 0: 210 octets
Fragment 1: 210 octets
Fragment 2:  80 octets
```

with:

```text
fragment_count = 3
```

### 1.2 Fields preserved across fragments

All fragments belonging to one logical message have the same:

```text
source_id
boot_id
sequence
type
flags
timestamp_us
object_id
fragment_count
```

They may differ in:

```text
fragment_index
payload_size
payload
CRC-32
```

The identity of the logical message remains:

```text
(source_id, boot_id, sequence)
```

Fragmentation does not create new logical messages and does not consume additional sequence numbers.

### 1.3 Fragmentation limit

`fragment_count` is an 8-bit field.

A logical message can therefore contain at most:

```text
255 fragments
```

The maximum logical payload supported by the fragmentation layer depends on the payload capacity of the transport profile:

| Profile | Payload per frame | Maximum logical payload |
| ------- | ----------------: | ----------------------: |
| ESP-NOW |        210 octets |           53,550 octets |
| Serial  |      4,056 octets |        1,034,280 octets |
| USB HID |         22 octets |            5,610 octets |

These values are protocol ceilings.

A session, application, or implementation may impose a lower limit.

For encrypted messages, cryptographic overhead also occupies part of the logical payload and is described in [Encryption](encryption.md).

---

## 2. Reassembly

A consumer reconstructs a fragmented logical message by collecting all fragments that belong to the same message identity.

Fragments are associated using:

```text
(source_id, boot_id, sequence)
```

After a fragment has been associated with an in-progress message, the following fields must also remain consistent:

```text
type
flags
timestamp_us
object_id
fragment_count
```

Two fragments with the same message identity but different values for one of these fields represent a conflict and must not be combined.

### 2.1 Fragment ordering

`fragment_index` defines the position of each fragment in the logical payload.

For:

```text
fragment_count = 4
```

the reconstructed payload is:

```text
payload =
    payload[fragment 0]
    || payload[fragment 1]
    || payload[fragment 2]
    || payload[fragment 3]
```

where `||` represents byte concatenation.

The fragment index therefore defines ordering independently from arrival order.

A transport does not need to deliver fragments in sequence for the reference reassembler to reconstruct the message.

### 2.2 Duplicate fragments

A fragment may be received more than once.

If a fragment with the same `fragment_index` has already been stored and the new fragment contains byte-identical payload data, it is treated as a duplicate.

```text
same identity
same fragment_index
same payload
        |
        v
    Duplicate
```

The existing fragment remains valid.

### 2.3 Conflicting fragments

If the same fragment position is received with different contents, the fragments conflict.

```text
same identity
same fragment_index
different payload
        |
        v
     Conflict
```

The receiver cannot determine which payload is correct.

The reference reassembler therefore rejects the conflict and discards the incomplete message.

A conflict is also reported when fragments using the same message identity disagree on message invariants such as `type`, `flags`, `timestamp_us`, `object_id`, or `fragment_count`.

---

## 3. Reference reassembler

The BTP reference library provides a bounded reassembly implementation.

It uses a fixed number of reassembly slots supplied by the application.

Each slot contains:

* storage for one logical payload;
* the logical message header;
* received-fragment state;
* fragment sizes;
* the last activity time.

The library does not allocate reassembly storage dynamically.

The amount of memory available for simultaneous fragmented messages is therefore defined by the application.

### 3.1 Reassembly results

Submitting a fragment to the reference reassembler produces one of the following results:

| Result            | Meaning                                                          |
| ----------------- | ---------------------------------------------------------------- |
| `Accepted`        | Fragment stored; message is incomplete                           |
| `Complete`        | All fragments have been received                                 |
| `Duplicate`       | Identical fragment already received                              |
| `Conflict`        | Fragment conflicts with data already associated with the message |
| `InvalidFragment` | Fragment header is invalid                                       |
| `MessageTooLarge` | Reassembled payload exceeds available slot capacity              |
| `NoSlot`          | No reassembly slot is available                                  |
| `InvalidArgument` | Invalid API argument                                             |

### 3.2 Completed messages

After all fragments have been received, the reference reassembler returns a logical message.

The fragmentation fields are normalized to the unfragmented representation:

```text
FRAGMENTED     = 0
fragment_index = 0
fragment_count = 1
```

Other logical-message properties are preserved.

For an encrypted message:

```text
ENCRYPTED
CIPHER_ID
```

remain unchanged.

The resulting object represents the complete logical message rather than one of its transport fragments.

This normalized representation is also used by the authenticated-encryption model described in [Encryption](encryption.md).

### 3.3 Slot lifetime

The payload of a completed message remains stored in its reassembly slot.

The application must release the slot when the message is no longer required.

Until release:

```text
slot -> completed message storage
```

remains occupied.

If completed messages are not released, new fragmented messages may eventually produce:

```text
NoSlot
```

### 3.4 Reassembly timeout

Each reassembly slot has a timeout.

The reference implementation receives the current time from the caller as `now_ms`.

If:

```text
now_ms - last_activity_ms >= timeout_ms
```

the slot is released.

This applies to incomplete and completed slots.

The reassembler therefore has no dependency on an operating-system clock.

The caller controls the time source used by the library.

---

## 4. Transport presets

The reference library ships three ready-made `TransportLimits`:

| Property                               |      ESP-NOW |             Serial |             USB HID |
| -------------------------------------- | -----------: | -----------------: | ------------------: |
| Preset                                 | `kEspNowTransport` | `kSerialTransport` | `kUsbHidTransport` |
| Maximum BTP frame                      |          250 |               4096 |                  62 |
| Maximum BTP payload                    |          210 |               4056 |                  22 |
| Link representation                    | One datagram | COBS-framed stream | 64-octet HID report |
| Message boundary provided by transport |          Yes |                 No |                 Yes |
| Authenticated encryption               |    Supported |          Supported |       Not supported |

Nothing about these three is privileged over a `TransportLimits` a caller builds for its own link -- they exist because these three are the ones this document (and the reference examples) describe.

The transport affects how a BTP frame reaches the peer.

It does not change:

* the BTP header layout;
* field encoding;
* message identity;
* timestamp semantics;
* message types;
* application payload semantics.

---

## 5. ESP-NOW profile

ESP-NOW provides datagram boundaries.

One ESP-NOW datagram carries exactly one BTP frame:

```text
+----------------------------------+
|         ESP-NOW datagram         |
|                                  |
|  BTP header + payload + CRC-32   |
+----------------------------------+
```

No additional BTP delimiter, length prefix, or padding is added.

The profile limits are:

```text
maximum frame   = 250 octets
maximum payload = 210 octets
```

A received datagram must therefore satisfy:

```text
40 <= frame_size <= 250
```

and:

```text
frame_size = 40 + payload_size
```

The MAC address associated with the ESP-NOW peer belongs to the transport layer.

It does not replace:

```text
source_id
boot_id
sequence
```

and is not part of BTP message identity.

Likewise, successful delivery reported by the radio does not indicate that the remote application processed a BTP message.

Transport delivery and application completion are separate events.

---

## 6. Serial profile

A serial connection is a byte stream.

It does not provide message boundaries.

A receiver may obtain:

```text
partial frame
```

or:

```text
frame A + frame B
```

or:

```text
end of frame A + frame B + start of frame C
```

in a single read operation.

BTP therefore adds framing around each serial frame using COBS.

The wire representation is:

```text
0x00 || COBS(BTP frame) || 0x00
```

where `||` represents byte concatenation.

### 6.1 COBS framing

COBS removes `0x00` values from the encoded body.

The value:

```text
0x00
```

can therefore be used exclusively as a frame boundary.

The original BTP frame may contain any byte value. COBS is applied only for serial transport and is removed before the BTP frame decoder processes the frame.

The serial processing sequence is:

```text
BTP frame
    |
    v
COBS encode
    |
    v
add 0x00 boundaries
    |
    v
serial stream
```

Reception performs the inverse operation:

```text
serial stream
    |
    v
detect 0x00 boundaries
    |
    v
COBS decode
    |
    v
BTP frame
    |
    v
BTP decode
```

### 6.2 Serial limits

The BTP serial profile defines:

| Quantity               |     Maximum |
| ---------------------- | ----------: |
| BTP frame              | 4096 octets |
| BTP payload            | 4056 octets |
| COBS block             | 4113 octets |
| Complete serial packet | 4115 octets |

The complete serial packet includes both `0x00` delimiters.

### 6.3 Stream synchronization

A serial receiver may begin reading in the middle of a packet.

The decoder therefore starts in an unsynchronized state.

It discards bytes until it receives:

```text
0x00
```

After that boundary, it begins collecting the next COBS block.

This allows the decoder to recover framing after:

* opening an active serial connection;
* receiving an incomplete packet;
* losing bytes;
* discarding malformed serial data.

An empty block between consecutive delimiters is ignored.

Therefore:

```text
00 00
```

does not represent an empty BTP frame.

### 6.4 Console mode

A serial interface may also support a human-readable console mode.

Binary BTP framing and console mode are separate states.

The receiver does not attempt to determine automatically whether incoming bytes represent console text or BTP frames.

Mode transitions occur through the session mechanism described in [Session and terminal](session-and-terminal.md).

---

## 7. USB HID profile

The USB HID profile uses fixed-size 64-octet reports.

The BTP frame occupies part of the report:

```text
+---------------+--------------+-------------------+---------+
| report_id     | valid_length | BTP frame         | padding |
| 1 octet       | 1 octet      | up to 62 octets  |         |
+---------------+--------------+-------------------+---------+
                    64 octets total
```

The profile limits are:

```text
maximum BTP frame   = 62 octets
maximum BTP payload = 22 octets
```

### 7.1 Valid length

HID reports have a fixed size and may contain padding after the BTP frame.

The `valid_length` field identifies how many report bytes belong to the BTP frame.

Without this value, the receiver could not distinguish BTP data from report padding.

The HID report boundary already provides message framing, so COBS is not used.

### 7.2 Fragmentation

The USB HID payload ceiling is 22 octets.

Any logical payload larger than this limit is fragmented using the normal BTP fragmentation mechanism.

The logical message format does not change because the transport has a smaller frame capacity.

### 7.3 Encryption restriction

The current USB HID profile does not permit BTP authenticated encryption.

A frame with:

```text
ENCRYPTED = 1
```

is rejected when encoded or decoded using this transport profile.

This is a transport-profile restriction. It does not change the general BTP frame format or the encryption capabilities of other profiles.

USB HID is point-to-point and does not define an additional BTP peer-addressing mechanism at the link layer.

---

## 8. Crossing transport profiles

A gateway may receive a logical message using one transport profile and transmit it using another.

For example:

```text
            small frame limit             large frame limit

Producer --------------------> Gateway --------------------> Consumer
                Profile A                   Profile B
```

The number and size of fragments may change across the gateway.

The gateway first reconstructs the logical message and then fragments it according to the outgoing transport profile.

For example:

```text
Incoming profile limit = 22 octets

Fragment 0
Fragment 1
Fragment 2
Fragment 3
Fragment 4
     |
     v
  Gateway
     |
     | reassembly
     v
Logical message: 100 octets
     |
     | outgoing limit = 210 octets
     v
One outgoing frame
```

The reverse is also valid:

```text
One incoming frame
        |
        v
     Gateway
        |
        | reassembly / logical message
        | re-fragmentation
        v
Fragment 0
Fragment 1
Fragment 2
...
```

Re-fragmentation does not create a new logical message.

The gateway preserves:

```text
source_id
boot_id
sequence
timestamp_us
type
object_id
logical payload
```

Only transport-dependent fragmentation changes.

---

## 9. Encrypted messages across transports

Authenticated encryption is applied to the logical message rather than independently to each transport fragment.

The authentication data uses a canonical representation of the logical header.

As a result, a gateway can:

1. receive fragments;
2. reassemble the protected logical payload;
3. fragment it according to another transport profile;
4. forward it without decrypting the application payload.

The gateway does not require the encryption key for this operation.

```text
Encrypted logical message
          |
          v
    fragmentation A
          |
          v
       Gateway
          |
      reassembly
          |
    fragmentation B
          |
          v
Encrypted logical message
```

The authentication tag remains associated with the logical message.

A gateway cannot forward an encrypted BTP message through a transport profile that prohibits encrypted frames.

The complete cryptographic procedure is defined in [Encryption](encryption.md).

---

## 10. Summary

BTP uses one frame format across all transport profiles.

Transport profiles define how that frame is carried and how much payload can be placed in one frame.

When the logical payload exceeds the selected profile limit, BTP fragmentation divides it into multiple independently validated frames.

The receiver reconstructs these fragments using the logical message identity:

```text
(source_id, boot_id, sequence)
```

Gateways may reassemble and re-fragment messages when moving between transports without changing the original message identity, timestamp, or application semantics.

Transport-specific framing remains outside the logical BTP message.
