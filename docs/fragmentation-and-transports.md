# Getting it across the link

The envelope from the previous chapter is identical on every transport. What
changes between profiles is a size ceiling and how frames are delimited on the
physical link. Nothing else — which is why this is one chapter instead of
three.

## 1. Fragmentation

A logical message larger than the transport's payload ceiling is cut into
fragments. Every fragment is a complete, independently valid frame: full
header, own `payload_size`, own CRC.

All fragments of one message share the same identity triple
(`source_id`, `boot_id`, `sequence`) and the same `type`, `flags`,
`timestamp_us` and `object_id`. They differ only in `fragment_index`,
`payload_size` and CRC.

The cut is by position: fragment `i` carries the slice starting at
`i * limit`, so every fragment except the last is exactly the ceiling size.
Reassembly relies on that — a fragment's offset in the reassembled message is
implied by its index and the sizes before it, not carried on the wire.

`fragment_count` is one octet, so **a logical message is at most 255
fragments**. That is a hard ceiling with no alternative mechanism above it: a
large manifest or a large command result uses this same fragmentation or does
not pass at all.

| Profile | Max logical payload |
| --- | ---: |
| ESP-NOW | 53550 (`255 x 210`) |
| USB HID | 5610 (`255 x 22`) |
| Serial | bounded by negotiation, not by the fragment count |

Fragmenting is zero-copy: a fragment is a view over a slice of the logical
payload, not a copy of it.

## 2. Reassembly

The receiver side is allocation-free, out-of-order tolerant, bounded, and
clocked by the caller. It works in *slots*: a fixed number of in-progress
messages, each with a fixed storage capacity, both supplied by the consumer.

A fragment is matched to a slot by the identity triple. Once matched, **every
other header field must agree too** — `type`, `flags`, `timestamp_us`,
`object_id` and `fragment_count`. A fragment claiming the same message but
disagreeing about any of them is not that message.

Pushing a fragment produces one of these outcomes:

| Outcome | Meaning |
| --- | --- |
| `Accepted` | Stored; the message is still incomplete |
| `Complete` | This fragment finished the message |
| `Duplicate` | This index arrived before with **byte-identical** content; ignored |
| `Conflict` | This index arrived before with *different* content; the slot is destroyed |
| `InvalidFragment` | The header is not a valid fragment header |
| `MessageTooLarge` | The message exceeds the slot's storage capacity |
| `NoSlot` | Every slot is busy with another message |

The `Duplicate` versus `Conflict` distinction is the interesting one. A retry
of the same fragment is harmless and is silently absorbed. Two different
payloads claiming the same position in the same message means one of them is
wrong and there is no way to tell which — so the whole in-progress message is
thrown away rather than assembled from a mix.

### Completion normalizes the header

When the last fragment arrives, the reassembled message is presented with the
**logical** header: `FRAGMENTED` cleared, `fragment_index` 0,
`fragment_count` 1. `ENCRYPTED` and `CIPHER_ID` survive.

That is not cosmetic. It is exactly the canonical form the AEAD tag was
computed over, so the reassembled message can be handed straight to
`aead_open` — see [Encryption](encryption.md).

### The slot stays yours until you release it

A completed message keeps owning its slot. Its payload view points into the
slot's storage and stays valid until the consumer explicitly releases the slot.

This is the obligation that is easiest to forget. A consumer that reads
completed messages and never releases them will run out of slots and start
seeing `NoSlot`. Completed slots do eventually expire on the timeout like any
other, but relying on that is relying on data loss.

Timeouts are driven by a `now_ms` value the caller passes in on every push, so
the library has no clock dependency of its own and behaves identically in a
test harness and on hardware.

## 3. The three transport profiles

| | ESP-NOW | Serial | USB HID |
| --- | ---: | ---: | ---: |
| Max frame | 250 | 4096 | 62 |
| Max payload | 210 | 4056 | 22 |
| Link framing | one datagram per frame | `00` + COBS(frame) + `00` | 64-octet report |
| Human console mode | no | yes | never |
| `ENCRYPTED` allowed | yes | yes | **no** |

### 3.1 ESP-NOW

The datagram *is* the frame: header, payload, CRC, and nothing else. No length
prefix, no delimiter, no padding, no struct.

A receiver rejects anything under 40 or over 250 octets and then requires the
length to match `40 + payload_size` exactly.

The source MAC address may be used for routing and diagnostics. It does not
replace `source_id` and `boot_id`, and it does not authenticate anything.

Note the three delivery levels here in particular: a successful send call means
the radio accepted the buffer, the transmit callback means the link reported
delivery, and neither means a peer interpreted the message. Only a BTP reply
means that.

### 3.2 Serial

A serial port is a byte stream with no message boundaries, and a BTP payload
can contain any octet including `0x00`, CR and LF. So the frame is wrapped:

```text
0x00 || COBS(frame) || 0x00
```

COBS — consistent overhead byte stuffing — removes every `0x00` from the
encoded body, which leaves `0x00` free to mean "boundary" and nothing else. The
cost is bounded and tiny: one octet per 254, so at most 17 octets on a maximum
4096-octet frame.

| Quantity | Value |
| --- | ---: |
| Max frame | 4096 |
| Max COBS block | 4113 (`4096 + 4096/254 + 1`) |
| Max packet on the wire | 4115 (block plus two delimiters) |

Both delimiters are sent, not just the trailing one. A receiver that joins the
stream mid-frame — because it just opened the port, or because bytes were lost
— discards everything until it sees a delimiter, and only then starts
collecting. The incremental decoder starts in exactly that unsynchronized
state by design.

An empty run between two delimiters is ignored rather than treated as an error,
so back-to-back delimiters are harmless.

Serial is also the only profile with a human console mode. The transition into
and out of binary framing is a session concern and is described in
[Session and terminal](session-and-terminal.md). The rule that matters here:
**binary framing is never autodetected.** The port is in one mode or the other,
and it changes only by an explicit exchange.

### 3.3 USB HID

A HID interrupt report is fixed-size and always zero-padded by the USB stack.
That padding is indistinguishable from payload, so the report carries its own
length:

```text
report_id (1) || valid_length (1) || BTP frame (up to 62) || padding
```

64 octets total. The length octet is not redundancy — without it the receiver
cannot tell where the frame ends and the stack's padding begins. Both sides of
the link must agree to prepend it.

There is no COBS inside a report: the report boundary is already a message
boundary, so there is nothing to delimit.

This profile is tight. With 22 octets of payload per frame, **even a `HELLO`
fragments**. That is normal for the profile and not a negotiation failure.

It is also why encryption is refused here. A 16-octet AEAD tag over a 22-octet
payload ceiling is 73% overhead, against roughly 7.6% on ESP-NOW and 0.4% on
serial. Both `encode()` and `decode()` reject `ENCRYPTED` on this profile with
`EncryptedNotAllowedOnTransport` — the decode side matters too, because it stops
a gateway from relaying an encrypted frame from another transport onto HID.

USB HID is point to point. There is no peer concept and no addressing at the
link level, and the interface is always in protocol mode from the moment the
host opens it.

## 4. Crossing between transports

A gateway that receives on one profile and sends on another does not translate
anything. It reassembles the logical message, then re-fragments it under the
outgoing profile's ceiling. The envelope fields are copied unchanged — the
identity triple, the timestamp and the object id are the producer's and stay
the producer's.

Because the AEAD tag is computed over the canonicalized logical header, an
encrypted message can make this crossing **without the gateway holding the
key**. The one exception is the HID profile, which does not accept encrypted
frames at all.
