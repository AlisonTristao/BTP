# Fragmentation and transports

BTP separates a logical message from the transport used to carry it.

The BTP frame format remains unchanged between transports. A transport is described to the codec as `btp::TransportLimits` -- not a fixed, closed list of named profiles, and not two independent sizes either, just the frame ceiling and one policy bit:

```cpp
struct TransportLimits {
    std::size_t max_frame_size;
    bool allow_encrypted;
};
```

* `max_frame_size` -- the maximum frame size;
* `allow_encrypted` -- whether this transport may carry an ENCRYPTED frame at all (section 7.3 below is why this exists as its own field, not something derived from the size).

There is no `max_payload_size` field to set. The maximum payload carried by one frame is always `max_frame_size` minus the 40-octet header+CRC floor -- every real transport already has exactly that relationship (250->210, 4096->4056, 62->22 below), so a caller has nothing to keep in sync, and `btp::max_payload_size(transport)` computes it on demand.

How BTP frames are delimited or encapsulated on the link (section 6.1's COBS framing, section 7's HID report) is NOT part of `TransportLimits` -- it is caller code around the bytes the codec produces/consumes, described per transport below.

Five ready-made presets cover the transports this document describes -- `btp::kEspNowTransport`, `btp::kSerialTransport`, `btp::kUsbHidTransport`, `btp::kBleTransport`, `btp::kTcpTransport` -- and a caller with a different link builds its own `TransportLimits` for it; there is no enum to extend.

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

The maximum payload carried by one frame is defined by the selected `TransportLimits`.

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

For example, a 500-octet logical payload transmitted through a transport with a 210-octet payload limit produces:

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

The maximum logical payload supported by the fragmentation layer depends on the payload capacity of the selected `TransportLimits`:

| Preset  | Payload per frame | Maximum logical payload |
| ------- | ----------------: | ----------------------: |
| ESP-NOW |        210 octets |           53,550 octets |
| Serial  |      4,056 octets |        1,034,280 octets |
| USB HID |         22 octets |            5,610 octets |
| BLE     |        472 octets |          120,360 octets |
| TCP     |      8,152 octets |        2,078,760 octets |

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

The reference library ships five ready-made `TransportLimits`:

| Property                               |      ESP-NOW |             Serial |             USB HID |                                                BLE |                 TCP |
| -------------------------------------- | -----------: | -----------------: | ------------------: | -------------------------------------------------: | ------------------: |
| Preset                                 | `kEspNowTransport` | `kSerialTransport` | `kUsbHidTransport` |                                     `kBleTransport` |     `kTcpTransport` |
| Maximum BTP frame                      |          250 |               4096 |                  62 |                                                 512 |                 8192 |
| Maximum BTP payload                    |          210 |               4056 |                  22 |                                                 472 |                 8152 |
| Link representation                    | One datagram | COBS-framed stream | 64-octet HID report | COBS-framed stream over GATT writes/notifications | COBS-framed stream |
| Message boundary provided by transport |          Yes |                 No |                 Yes |                                                  No |                   No |
| Authenticated encryption               |    Supported |          Supported |       Not supported |                                            Required |             Required |

Nothing about these five is privileged over a `TransportLimits` a caller builds for its own link -- they exist because these five are the ones this document (and the reference examples) describe.

The transport affects how a BTP frame reaches the peer.

It does not change:

* the BTP header layout;
* field encoding;
* message identity;
* timestamp semantics;
* message types;
* application payload semantics.

---

## 5. ESP-NOW

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

`kEspNowTransport`'s `TransportLimits` are:

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

## 6. Serial

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

`kSerialTransport`'s `TransportLimits` define:

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

## 7. USB HID

USB HID uses fixed-size 64-octet reports.

The BTP frame occupies part of the report:

```text
+---------------+--------------+-------------------+---------+
| report_id     | valid_length | BTP frame         | padding |
| 1 octet       | 1 octet      | up to 62 octets  |         |
+---------------+--------------+-------------------+---------+
                    64 octets total
```

`kUsbHidTransport`'s `TransportLimits` are:

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

`kUsbHidTransport` does not permit BTP authenticated encryption
(`allow_encrypted == false`).

A frame with:

```text
ENCRYPTED = 1
```

is rejected when encoded or decoded against this `TransportLimits`.

This is a restriction of that one preset's `TransportLimits`. It does not
change the general BTP frame format or the encryption capabilities of any
other transport.

USB HID is point-to-point and does not define an additional BTP peer-addressing mechanism at the link layer.

---

## 8. BLE

BLE connects TraceView, acting as the GATT **central** (client), directly to the ESP32-S3 running `bally_OS`, acting as the GATT **peripheral** (server). This is a direct link between the two, alongside the existing dongle/hub path and a direct TCP transport -- it does not replace either. Classic Bluetooth / SPP is explicitly out of scope; this section covers BLE/GATT only.

### 8.1 The BTP GATT service

The peripheral advertises one custom 128-bit GATT service and exposes two characteristics on it. These UUIDs are randomly generated (v4) and are the official identifiers for the BTP GATT contract -- an implementation must use them verbatim, not placeholders:

| Name                    | UUID                                   | Properties        | Direction            |
| ------------------------ | --------------------------------------- | ------------------ | --------------------- |
| BTP service              | `547a1aae-676e-4b68-8e20-bace26cd0726` | --                  | --                     |
| RX characteristic        | `f9160b78-c242-42f4-8f5e-88df2c51cbe6` | Write With Response | TraceView -> robot    |
| TX characteristic        | `20f96ede-2f3b-4e02-b2cf-be6bb75dbe35` | Notify              | robot -> TraceView    |

TraceView writes outgoing bytes to the RX characteristic using **Write With Response**, and the robot streams incoming bytes to TraceView as **Notify** events on the TX characteristic. Write With Response confirms, at the GATT/ATT level, that the peripheral's BLE stack accepted the bytes for the RX attribute. It does **not** confirm that a BTP command carried inside those bytes was parsed, accepted, or executed -- that confirmation, if any, is a `COMMAND_RESULT` (or the relevant BTP response) arriving later on the TX characteristic, exactly as with every other transport. A GATT write response is a delivery acknowledgement at the link layer, not an application-layer acknowledgement.

There is no separate version/identity characteristic. Exposing `btpVersion` or a stable identifier as a plain GATT characteristic value, readable before `HELLO`, would create a second source of truth for identity alongside `peer_uuid` in `HELLO_RESULT` ([Session and terminal §1.1](session-and-terminal.md#11-peer_uuid)) -- one that is not authenticated the same way and that a peripheral could misreport without the AEAD protection described in section 8.9 below. BTP already has a canonical, transport-agnostic identity and capability handshake; BLE does not need to duplicate it a layer earlier. A central that wants to filter candidates before connecting uses GAP advertising data (service UUID presence, local name) as a *discovery hint* only -- see section 8.5.

### 8.2 Wire representation: a fragmented stream, not one packet per operation

BLE does not guarantee a large ATT MTU. The default, unnegotiated ATT MTU is 23 octets, leaving 20 usable octets per Write or Notify operation (3 octets of ATT opcode/handle overhead); after an ATT MTU exchange, ESP32-class NimBLE stacks commonly negotiate up to 247 octets (244 usable), but a central must not assume that negotiation succeeds or that the peripheral requests it.

Because of this, one BTP frame is **not** assumed to fit in one GATT write or one GATT notification. BTP treats the RX and TX characteristics the same way it treats a serial byte stream ([section 6](#6-serial)): as an ordered, boundary-less byte pipe that the application frames itself with COBS.

```text
0x00 || COBS(BTP frame) || 0x00
```

is written to RX (respectively received from TX) split across as many Write With Response (respectively Notify) operations as the negotiated ATT MTU requires:

```text
COBS-framed BTP frame
        |
        v
+----------+----------+----------+     +----------+
| GATT op 1| GATT op 2| GATT op 3| ... | GATT op N|
+----------+----------+----------+     +----------+
```

The receiver on either end reconstructs the COBS-framed stream by concatenating the payloads of successive GATT operations in delivery order (BLE, unlike ESP-NOW, is an ordered link within one connection) and applying the same `0x00`-delimited COBS decoder described in [section 6.1](#61-cobs-framing) and [section 6.3](#63-stream-synchronization), including unsynchronized-start recovery after a fresh connection. A receiver must never assume "one GATT write == one BTP frame" -- a single small write may be a fragment of a much larger COBS block, and a single COBS block may itself already be one fragment of a BTP-level fragmented logical message ([section 1](#1-fragmentation)). These are two independent, stacked fragmentation layers: GATT-operation-level (this section) and BTP-logical-message-level (section 1).

### 8.3 `kBleTransport` limits

```text
maximum BTP frame   = 512 octets
maximum BTP payload = 472 octets
```

| Quantity     |     Maximum |
| ------------ | ----------: |
| BTP frame    |  512 octets |
| BTP payload  |  472 octets |
| COBS block   |  516 octets |
| GATT operation payload | 20-244 octets (MTU-dependent, see 8.2) |

512 octets is deliberately not "the same size as one GATT operation" -- section 8.2 already establishes that a frame spans many operations, so there is no hard ceiling forcing `kBleTransport` down to USB HID's 62-octet order of magnitude the way there is for USB HID's single-report framing. 512 is chosen instead as a middle ground: large enough that a `MANIFEST_DATA` or `TELEMETRY` message rarely needs BTP-level fragmentation on top of the GATT-level fragmentation it already pays for, but small enough that one logical frame does not monopolize the connection's notification queue for long relative to [priority class 1-2 traffic](session-and-terminal.md#8-priority) -- a `kSerialTransport`-sized 4096-octet frame would take roughly 8x longer to drain through 20-octet GATT operations than through a full-duplex serial UART, which would let one low-priority message block session/command traffic in flight ([session-and-terminal.md §8.3](session-and-terminal.md#83-frames-already-in-flight)) for an outsized fraction of a second. `kBleTransport.allow_encrypted` is `true` -- see section 8.9.

### 8.4 Starting a session: RX/TX must be usable before `HELLO`

BLE has no `ENTER` / `READY` textual handshake -- there is no serial console state to leave. But unlike ESP-NOW and USB HID, where `HELLO` may be sent the instant the link exists ([Session and terminal §3.2](session-and-terminal.md#32-other-transports)), BLE has GATT preconditions that must complete first. [Session and terminal §3.3](session-and-terminal.md#33-ble) is the normative definition of this gate; in summary, the central must not send `HELLO` until, in order:

```text
GATT connection established
        |
        v
BTP service discovered (547a1aae-676e-4b68-8e20-bace26cd0726)
        |
        v
RX characteristic located (f9160b78-c242-42f4-8f5e-88df2c51cbe6)
        |
        v
TX characteristic located (20f96ede-2f3b-4e02-b2cf-be6bb75dbe35)
        |
        v
TX notifications enabled (CCCD written)
        |
        v
HELLO may be sent
```

A `HELLO` sent before TX notifications are enabled would race the peripheral's `HELLO_RESULT`: the notification could be generated before the central's CCCD write completes and be silently lost by the BLE stack, since notifications are not queued for a subscriber that has not yet subscribed. Enabling notifications is therefore a hard precondition, not an optimization.

### 8.5 Identity: advertised name and address are discovery hints, not identity

A peripheral's BLE advertising payload (local name, the BTP service UUID, the GAP address) lets a central *find* candidate robots before connecting. None of it is the robot's authoritative identity:

* a GAP/public or random BLE address can rotate (privacy features, address randomization) or collide across devices;
* a local name is operator-configurable and unauthenticated;
* GATT service/characteristic presence only proves the peripheral advertises the right shape, not which specific robot it is.

The authoritative identity remains exactly what every other BTP transport uses: `peer_uuid`, exchanged in `HELLO` / `HELLO_RESULT` ([Session and terminal §1.1](session-and-terminal.md#11-peer_uuid)) after the session in section 8.4 begins. TraceView should treat the advertised name/address as a pre-connection filter and re-validate `peer_uuid` on every connection (including reconnections) before trusting that it is talking to the same robot as before -- the same rule TCP direct connections follow ([section 9.6](#96-identity-revalidate-on-every-connection)), and the same rule the existing dongle/hub path already follows for its ESP-NOW peers.

### 8.6 One control session per robot

A single-peripheral BLE role, as used here, accepts one active central connection at a time by construction: the peripheral advertises while unconnected and stops advertising as soon as a central connects, so a second central has nothing to connect to at the GAP layer -- it cannot complete a connection, let alone reach GATT discovery or `HELLO`. This is the primary mechanism enforcing "one control session per robot" for BLE, and it means BLE never has to exercise the BTP-level admission rejection at all: `HELLO_RESULT`'s `status` field does carry a `BUSY` value for exactly this situation ([Session and terminal §2.4](session-and-terminal.md#24-second-concurrent-session-tcp), introduced for TCP, where the link layer offers no equivalent single-connection guarantee), but the GAP layer already keeps a second BLE central from ever reaching `HELLO` in the first place, so BLE simply never needs it.

If a future hardware/stack revision allows the peripheral to accept more than one simultaneous central link (some BLE controllers can), that capability must not be used to allow two concurrent BTP sessions against the same robot. The peripheral application must still refuse a second incoming connection at the GAP/link layer -- reject or immediately disconnect it -- before GATT discovery can begin, rather than letting it proceed to `HELLO` and relying on BTP itself to reject it. This preserves the same externally observable behavior (second attempt rejected, first session undisturbed) regardless of what the underlying radio is capable of, and keeps BLE from having to rely on the `HELLO_RESULT` `BUSY` path that TCP uses, for something the link layer already prevents in the common case.

### 8.7 Disconnection and reconnection

A BLE link-layer disconnection is treated exactly like transport loss on any other BTP transport ([Session and terminal §5](session-and-terminal.md#5-the-watchdog)): the session ends immediately, without waiting for `SESSION_CLOSE` / `SESSION_CLOSE_RESULT`. On disconnect, the peripheral:

* discards incomplete reassemblies and any queued-but-unsent TX notifications for that connection;
* resumes advertising the BTP service so a new central (the same robot's owner, reconnecting, or a different one) can connect;
* does **not** retain BTP session state across the disconnect.

A subsequent connection -- whether the same central reconnecting or a different one -- repeats the full sequence from section 8.4 (GATT discovery, CCCD, `HELLO`) and produces a new, independent session. Consistent with [Session and terminal §5.3](session-and-terminal.md#53-session-loss-and-command-deduplication), the new session does not retransmit commands from the previous one, and command deduplication state (scoped to the executor boot, not the session) is unaffected by the BLE disconnect itself.

### 8.8 Flow control and notification saturation

The robot's BLE stack has a bounded queue for outgoing notifications; the application must not enqueue TX notifications faster than that queue -- and the underlying radio link -- can drain, or memory grows unbounded and/or the BLE stack starts rejecting notification calls. When that queue is under pressure, `kBleTransport` follows the same congestion policy already defined for every BTP transport ([Session and terminal §8.5](session-and-terminal.md#85-congestion-behavior)): `TELEMETRY` (priority 6) is the first traffic dropped, and a frame that has already begun transmission across GATT operations is never truncated mid-stream ([Session and terminal §8.3](session-and-terminal.md#83-frames-already-in-flight)) -- an in-flight COBS block is completed or abandoned as a whole, never cut at an arbitrary byte boundary that would corrupt COBS framing for every later frame on the same stream.

Queue depths, backpressure signaling, and reassembly timeouts are, like the priority scheduler itself ([Using the library §11.5](library.md#115-the-priority-scheduler-is-not-implemented)), an implementation detail rather than part of the wire contract -- two conforming peers do not need to agree on them, only on the frame format and the priority-drop rule above. What follows are this document's recommended defaults for `bally_OS` and TraceView; an implementation may use different values as long as it still honors the congestion policy.

#### Queue depth

`bally_OS`, as the GATT peripheral, keeps one outgoing queue per connection, sized in **complete BTP frames already split into GATT operations**, not in raw GATT operations (their count depends on the negotiated ATT MTU, section 8.2). A recommended depth is:

```text
8 frames queued for notification, per connection
```

At `kBleTransport`'s 512-octet frame ceiling, 8 frames is at most 4 KiB of buffered outgoing data -- a small, bounded footprint appropriate for the ESP32-S3's RAM budget. TraceView, as the central, mirrors this with an 8-frame outgoing queue for RX writes, and additionally serializes RX writes one at a time -- waiting for each Write With Response before issuing the next -- since the GATT operation itself is already a confirmed, one-at-a-time exchange on most BLE host stacks; there is no benefit to keeping more than one write in flight at the ATT layer, only at the BTP-frame layer above it.

This queue is independent of, and smaller in scope than, the `HELLO`-negotiated `max_inflight_reassemblies` ([Session and terminal §1.3](session-and-terminal.md#13-announced-limits)): that field bounds how many logical messages may be *mid-reassembly* on the receive side at once, while the queue depth here bounds how many complete outgoing frames a sender buffers before applying the congestion policy below. The two are not required to match.

#### A central that does not drain notifications fast enough

If the central stops confirming/draining notifications -- it is slow to process them, the connection is momentarily stalled, or the connection interval is large relative to the data rate -- the peripheral's outgoing queue fills. `bally_OS` must not block the rest of the system waiting for queue space. Concretely:

1. A new outgoing frame that would exceed the queue depth is handled per the congestion policy: if it is `TELEMETRY`, it is dropped and counted (never partially enqueued); if it is priority 1-5 traffic, `bally_OS` keeps it pending and applies backpressure to its own producers instead of dropping it, the same way it would for any other saturated transport.
2. A frame already accepted into the queue and partially sent across GATT operations is completed or abandoned as a whole (as above); it is never truncated to make room for something else.
3. If the queue remains saturated with priority 1-5 traffic long enough that `HELLO` / `SESSION_CLOSE` / command traffic cannot be delivered, the session's own watchdog ([Session and terminal §5](session-and-terminal.md#5-the-watchdog)) is the backstop: a session that cannot exchange a valid frame within `session_timeout_ms` is torn down like any other unresponsive peer, rather than BTP defining a second, BLE-specific stall timeout.

#### Reassembly timeout

BLE uses the same reassembly-timeout mechanism as every other transport ([section 3.4](#34-reassembly-timeout)): a local, receive-side, application-supplied `timeout_ms` per reassembly slot, not a value negotiated between peers. No BLE-specific override is required. A recommended default for both `bally_OS` and TraceView over `kBleTransport` is:

```text
5000 ms
```

longer than a wired-serial default would typically need to be, to absorb the extra latency a stalled notification queue (see above) or a large connection interval can add between fragments of the same logical message, while still bounding the worst-case memory held by an abandoned peer's incomplete message to a few seconds.

### 8.9 Encryption is required, not merely supported

The hub's existing channels rely on ESP-NOW's keyed link for protection; that protection must **not** be assumed to carry over to a direct BLE connection. BLE pairing/bonding, even when used, authenticates the *link* between two BLE stacks -- it does not authenticate the *BTP application payload*, and TraceView/`bally_OS` do not currently plan to depend on OS-level BLE pairing UX for this feature. `kBleTransport.allow_encrypted = true`, and unlike ESP-NOW/Serial (where BTP AEAD, [Encryption](encryption.md), is optional), a `kBleTransport` session is required to use it: every BTP message exchanged over BLE, `HELLO` included, must be sent with `ENCRYPTED` set and authenticated through the existing `btp::aead` mechanism (AES-128-GCM or ChaCha20-Poly1305, [Encryption §3](encryption.md#3-supported-algorithms)), under a key provisioned out-of-band the same way BTP already expects for any untrusted medium ([Encryption §4](encryption.md#4-key-configuration)) -- nothing about `HELLO` prevents it from being sealed like any other message, since the AEAD key comes from external provisioning rather than from the handshake itself. This reuses the protocol's existing authenticated-encryption mechanism rather than introducing a BLE-specific one: AEAD's tag already gives BLE the message authentication that GATT/ATT and BLE pairing do not provide at the application layer, exactly as described in [Encryption §1](encryption.md#1-security-model) for any untrusted-medium link. [Section 9.9](#99-key-provisioning-for-direct-tcpble-connections) proposes a concrete provisioning mechanism for this key, pending human confirmation.

---

## 9. TCP

TCP connects TraceView, acting as the TCP **client**, directly to the ESP32-S3 running `bally_OS`, acting as the TCP **server**, over the local Wi-Fi network. This is a direct link alongside the existing dongle/hub path and the direct BLE transport (section 8) -- it does not replace either.

### 9.1 Roles and addressing

TraceView opens the TCP connection; `bally_OS` listens and accepts it. TraceView is configured with the server's address as an IP address or hostname on the local network; BTP itself does not define a discovery mechanism for that address (manual entry, a config file, mDNS, etc. are integration choices outside this contract).

```text
TraceView (client)  ----TCP connect---->  ESP32-S3 / bally_OS (server)
```

### 9.2 Default port

BTP-TCP's default port is:

```text
44300
```

**Decision, may be revisited:** no port was previously reserved for BTP, so `44300` is chosen here inside the 40000-50000 range to avoid the IANA well-known range (0-1023) and the ports most commonly already registered to other services. It is documented as the default for the `bally_OS` TCP server and the value TraceView pre-fills, not a protocol requirement -- either side may be configured to use a different port, as long as both agree.

### 9.3 Framing: a stream, exactly like serial

TCP is a byte stream. It does not preserve message boundaries -- a single read may return a partial frame, more than one frame, or the tail of one frame followed by the start of the next, exactly as described for serial in [section 6](#6-serial).

BTP therefore reuses serial's COBS framing unchanged; the wire representation of a TCP-carried BTP frame is:

```text
0x00 || COBS(BTP frame) || 0x00
```

No TCP-specific framing byte, length prefix, or additional envelope is added on top of this. [Stream synchronization (section 6.3)](#63-stream-synchronization) applies identically: a receiver that starts reading mid-stream (for example, right after the TCP connection completes) discards bytes until the next `0x00` boundary before collecting the next COBS block.

### 9.4 `kTcpTransport` limits

`kTcpTransport`'s `TransportLimits` are:

```text
maximum BTP frame   = 8192 octets
maximum BTP payload = 8152 octets
```

| Quantity            |     Maximum |
| -------------------- | ----------: |
| BTP frame            | 8192 octets |
| BTP payload          | 8152 octets |
| COBS block            | 8225 octets |
| Complete TCP packet   | 8227 octets |

The complete TCP packet includes both `0x00` delimiters, exactly as for [serial's equivalent figure](#62-serial-limits).

TCP has no physical per-message size ceiling the way BLE's GATT operations or USB HID's report do; the limit above is a protocol-level ceiling for reassembly-slot sizing on both TraceView and `bally_OS`, not a link constraint. **Decision, may be revisited:** it is set to twice `kSerialTransport`'s frame size -- the same order of magnitude, since `bally_OS` runs the same fragmentation/reassembly code path across serial and TCP and a much larger ceiling would only move memory pressure from the link to the reassembly slots. A concrete workload that needs a different ceiling can revisit this value; it does not have to track `kSerialTransport` exactly.

### 9.5 Encryption is required, not merely supported

The ESP-NOW hop protected by the shared key in `bally_channels.h` (see `bally_OS` / `bally_dongle`) is a link-layer protection specific to that radio; it must **not** be assumed to protect a TCP connection opened directly to the robot. A BTP-TCP port is reachable by anything on the same Wi-Fi network, so BTP's own authentication has to be turned on explicitly rather than inherited from the hub's existing channels.

**Decision:** `kTcpTransport.allow_encrypted = true`, and unlike ESP-NOW/Serial (where BTP AEAD, [Encryption](encryption.md), is optional), a TCP session **must** use it: every BTP frame exchanged over a TCP connection, including `HELLO` / `HELLO_RESULT`, is sent with `ENCRYPTED = 1`. A peer that receives an unencrypted frame (`ENCRYPTED = 0`) as the first frame of a TCP connection treats it as a protocol violation and closes the socket without a `HELLO_RESULT` -- there is no unauthenticated fallback.

This reuses the AEAD mechanism [Encryption](encryption.md) already defines instead of inventing a TCP-specific handshake: both peers must already share the same pre-provisioned key and `CIPHER_ID` before the TCP connection is opened, exactly as [Key configuration](encryption.md#4-key-configuration) already describes for any other transport. `AES-128-GCM` (`CIPHER_ID = 0`) is the recommended default, since the ESP32-S3 has hardware AES acceleration ([Encryption §3](encryption.md#3-supported-algorithms)); `ChaCha20-Poly1305` remains an equally valid choice as long as both peers agree. This key is independent of, and must not reuse, the ESP-NOW channel key in `bally_channels.h` -- the two protect different links under different threat models. Because `HELLO` itself is encrypted, TraceView and `bally_OS` must already hold the shared key before the first `HELLO` is sent; there is no cleartext bootstrap step. [Section 9.9](#99-key-provisioning-for-direct-tcpble-connections) proposes a concrete mechanism for getting this key onto both peers in the first place, pending human confirmation.

The connection-establishment behavior that follows from this -- exactly when `HELLO` must be sent, what happens when a second control session is attempted, and what a closed socket means for the session -- is defined in [Session and terminal §3.4](session-and-terminal.md#34-tcp-connections), the normative counterpart to this section.

### 9.6 Identity: revalidate on every connection

TCP has no advertising payload to provide a pre-connection hint the way BLE does ([section 8.5](#85-identity-advertised-name-and-address-are-discovery-hints-not-identity)) -- a TraceView user configures a robot's TCP connection by IP address or hostname, and that address is not authoritative identity either: DHCP can reassign it, a hostname can resolve to a different device after a reconnect, and nothing before `HELLO` cryptographically ties the socket to a specific robot. The authoritative identity remains `peer_uuid`, exchanged in `HELLO` / `HELLO_RESULT` ([Session and terminal §1.1](session-and-terminal.md#11-peer_uuid)) once the encrypted session begins.

TraceView must re-validate `peer_uuid` on every TCP connection, including a reconnect to the same configured address -- the same rule BLE direct connections follow ([section 8.5](#85-identity-advertised-name-and-address-are-discovery-hints-not-identity)), and the same rule the existing dongle/hub path already follows for its ESP-NOW peers. A changed `peer_uuid` at a previously-known address means TraceView is now talking to a different robot, not the one the user last configured, and should be surfaced to the user rather than silently trusted.

This is a special case of the general session-loss rule ([Session and terminal §5.3](session-and-terminal.md#53-session-loss-and-command-deduplication)): a new TCP connection is always a new session, `HELLO` is mandatory before any application traffic, and TraceView must not resend or replay commands from a previous connection automatically -- the new session starts from empty command-tracking state on the TraceView side, exactly as [§3.4's "Session end and reconnection"](session-and-terminal.md#34-tcp-connections) already describes from the `bally_OS` side.

### 9.7 Flow control: queue depth and a client that does not drain

`bally_OS`'s TCP server keeps one outgoing queue per connection, sized in complete BTP frames, mirroring the recommendation for BLE ([section 8.8](#88-flow-control-and-notification-saturation)) but larger, since a TCP socket send buffer and a desktop-class TraceView peer both tolerate more outstanding data than a BLE notification queue on an ESP32-S3:

```text
bally_OS (server):  16 frames queued per connection
TraceView (client): 32 frames queued per connection
```

At `kTcpTransport`'s 8192-octet frame ceiling, 16 frames is at most 128 KiB of buffered outgoing data on the firmware side -- larger in absolute terms than BLE's budget, but still a small, bounded fraction of the ESP32-S3's RAM, and on the same order as what the underlying TCP stack's own send buffer already tolerates. As with BLE ([section 8.8](#88-flow-control-and-notification-saturation)), this queue is independent of the `HELLO`-negotiated `max_inflight_reassemblies`, which governs receive-side in-progress reassembly, not the outgoing queue defined here. These are recommended defaults, not wire requirements; two conforming peers only need to agree on the frame format and the priority-drop rule below, not on queue depth.

#### A client that does not drain the socket

TraceView is expected to keep reading its TCP socket continuously, but `bally_OS` cannot assume that: a stalled TraceView process, a suspended machine, or a saturated Wi-Fi link can all leave the client not draining data the server has already written, which backs up the kernel's TCP send buffer and then `bally_OS`'s own outgoing frame queue above. `bally_OS` must not block indefinitely on a `send()` call waiting for buffer space. Concretely:

1. `bally_OS` writes to the TCP socket using a non-blocking or bounded-timeout send. When the socket cannot accept more data and the outgoing queue is already at its configured depth, the same congestion policy used everywhere else in BTP applies ([Session and terminal §8.5](session-and-terminal.md#85-congestion-behavior)): a new `TELEMETRY` frame is dropped and counted rather than queued; priority 1-5 traffic is kept pending and backpressures its own producers instead.
2. A frame already partially written to the socket is never truncated mid-COBS-block ([Session and terminal §8.3](session-and-terminal.md#83-frames-already-in-flight)) -- `bally_OS` either finishes writing that one frame (blocking only its own completion, not the rest of the system) or, if the connection is being abandoned outright, drops the connection rather than the tail of the frame.
3. If a non-draining client leaves priority 1-5 traffic unable to make progress long enough that no valid frame can be exchanged within `session_timeout_ms`, the session watchdog ([Session and terminal §5](session-and-terminal.md#5-the-watchdog)) closes the session, exactly as it would for a BLE stall or any other unresponsive peer -- TCP does not need its own separate stall timeout for this case.

#### Reassembly timeout

TCP uses the same local, receive-side reassembly-timeout mechanism as every other transport ([section 3.4](#34-reassembly-timeout)); no TCP-specific override is required. The recommended default for both `bally_OS` and TraceView over `kTcpTransport` is the same as BLE's:

```text
5000 ms
```

TCP's loss characteristics on a local Wi-Fi network are usually better than BLE's, so nothing forces a shorter value; keeping the same default across both direct transports is simpler to implement and to reason about than justifying a second number, and 5 s already bounds how long an abandoned partial reassembly -- up to `kTcpTransport`'s larger 8192-octet frame -- holds memory before it is released.

### 9.8 OTA is out of scope

Firmware OTA update continues to use `bally_OS`'s existing HTTP-based OTA mechanism. It is unrelated to the BTP-TCP contract in this section and out of scope here.

### 9.9 Key provisioning for direct TCP/BLE connections

Both direct transports require the AEAD key before the first `HELLO` can be sent ([section 8.9](#89-encryption-is-required-not-merely-supported), [section 9.5](#95-encryption-is-required-not-merely-supported)), and [Encryption §4](encryption.md#4-key-configuration) deliberately leaves *how* that key reaches both peers to "an external mechanism" -- appropriate for a protocol specification, but not yet a concrete answer for `bally_OS` / TraceView, where there is no cloud backend, account system, or provisioning service to lean on. This section proposes one.

**Decision, needs human confirmation before implementation:**

1. `bally_OS` generates a random per-robot AEAD key (or is provisioned with one during manufacturing/assembly) and stores it in local non-volatile storage, associated with that robot's `peer_uuid`. This key is independent of, and must not reuse, the ESP-NOW hub channel key in `bally_channels.h` -- already required by [section 8.9](#89-encryption-is-required-not-merely-supported) / [section 9.5](#95-encryption-is-required-not-merely-supported) -- since it protects a different link under a different threat model.
2. The key is surfaced to a human operator through a channel that does not depend on the not-yet-secured TCP/BLE link itself: displayed as text and/or a QR code on hardware that already has a screen during setup (for example a T-Dongle-S3's display, if that is the device present), or printed on a physical label applied during assembly, the same way a consumer Wi-Fi router ships its passphrase on a label. Which of these `bally_OS` implements is a hardware/manufacturing decision outside this protocol document.
3. The first time a TraceView user configures a direct TCP or BLE connection to a specific robot, TraceView prompts for this key -- typed in, or scanned if a QR code and camera access are available -- and stores it locally, associated with that robot's `peer_uuid`, in the same per-device local configuration store TraceView already keeps for its other per-device connection state.
4. TraceView never transmits this key over the network to provision another device, and never derives it from a BTP message exchange -- consistent with [Encryption §4](encryption.md#4-key-configuration)'s warning that a key cannot be safely provisioned by sending it through the channel it is meant to protect.
5. If the key is ever regenerated or rotated (lost device, suspected compromise), the same first-time-setup flow runs again; BTP itself defines no rotation or revocation mechanism ([Encryption §15](encryption.md#15-key-lifetime)), so this remains purely a TraceView/`bally_OS` integration concern, not a protocol one.

This is a design proposal, not a protocol requirement, and the specific delivery mechanism in step 2 (QR code vs. printed label vs. something else) depends on hardware decisions -- what screen, if any, is present on the robot or its dongle at setup time -- that this document cannot make on its own. It is written down here, concretely, so that this open pendency has a default answer instead of an unresolved gap; a human (hardware/firmware owner) should confirm or override it before `bally_OS` and TraceView implement key provisioning.

---

## 10. Crossing transports

A gateway may receive a logical message using one `TransportLimits` and transmit it using another.

For example:

```text
            small frame limit             large frame limit

Producer --------------------> Gateway --------------------> Consumer
              TransportLimits A            TransportLimits B
```

The number and size of fragments may change across the gateway.

The gateway first reconstructs the logical message and then fragments it according to the outgoing `TransportLimits`.

For example:

```text
Incoming limit = 22 octets

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

## 11. Encrypted messages across transports

Authenticated encryption is applied to the logical message rather than independently to each transport fragment.

The authentication data uses a canonical representation of the logical header.

As a result, a gateway can:

1. receive fragments;
2. reassemble the protected logical payload;
3. fragment it according to another `TransportLimits`;
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

A gateway cannot forward an encrypted BTP message through a transport whose `TransportLimits` prohibits encrypted frames (`allow_encrypted == false`).

The complete cryptographic procedure is defined in [Encryption](encryption.md).

---

## 12. Summary

BTP uses one frame format across every transport.

`TransportLimits` -- one of the five presets, or a caller's own -- defines how that frame is carried and how much payload can be placed in one frame.

When the logical payload exceeds the selected limit, BTP fragmentation divides it into multiple independently validated frames.

The receiver reconstructs these fragments using the logical message identity:

```text
(source_id, boot_id, sequence)
```

Gateways may reassemble and re-fragment messages when moving between transports without changing the original message identity, timestamp, or application semantics.

Transport-specific framing remains outside the logical BTP message.
