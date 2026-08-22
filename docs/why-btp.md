# Why BTP exists

Nothing here is normative. This chapter answers the question the specification
chapters do not: **why choose BTP, and when not to.** If you arrived to
evaluate the protocol before adopting it, this is the chapter to read first and
the only one you can read on its own.

## 1. The problem

An embedded producer generates two classes of data with opposite requirements.
Periodic sensor samples are worth little individually and a lot in series, and
losing one is survivable. Control operations are worth a lot individually and
must never execute twice.

Both have to cross links with opposite characteristics: a low-bandwidth radio
with short datagrams and high loss, and a local bus with more bandwidth and a
byte-stream shape.

The improvised solutions to this fail in predictable ways:

- **Text over serial** — `printf` and line parsing — breaks on the first
  payload containing `0x0A`, cannot tell one channel from another, and mixes
  diagnostics with measurement.
- **Shipping the `struct`** works until the first different compiler,
  architecture or alignment flag.
- **Timestamping on arrival** produces a time series that measures transport
  latency, not the physical phenomenon.
- **One format per link** duplicates the semantics and turns every gateway into
  a translator, with one chance of reinterpretation per hop.

BTP solves those four at once, and that is what it is: a fixed-width binary
envelope, carrying an identity and an instant created at the source, that
crosses heterogeneous transports without changing meaning.

## 2. What the design buys

### 2.1 Time belongs to the source

`source_id`, `boot_id`, `sequence` and `timestamp_us` are created by the
producer and no intermediary rewrites them. A gateway is explicitly forbidden
from replacing the timestamp with the time of arrival.

The practical consequence is the one that matters: the time series is immune to
radio latency, gateway queue depth and consumer scheduling. A plot drawn
against `timestamp_us` shows the phenomenon. Drawn against arrival time, it
would show the transport.

### 2.2 One envelope, three transports

The 36-octet header, the CRC and the fragmentation invariants are identical on
ESP-NOW, serial and USB HID. Between profiles **only a limit constant changes**,
never the semantics — the same fragmentation function is parameterized by the
profile and does not switch behavior.

Adding a transport is therefore writing a profile section, not a second version
of the protocol.

### 2.3 Portability without an ABI

Every multi-octet integer is little-endian, the CRC included; no component ever
transmits the memory representation of a struct; and no size is ever derived
from `sizeof`, from alignment, or from an implicit enum type. The magic is a
sequence of four octets, not an integer.

The result is that the same frame is produced and consumed identically by a
microcontroller and by a desktop, in different languages, with no ABI to
negotiate.

### 2.4 It is cheap to embed

The shared library is C++11, **allocation-free** and free of any operating
system dependency: the caller supplies the buffers, the reassembly slots and
the storage capacity, and nothing grows at runtime.

The envelope codec depends on no crypto library at all — the cipher lives in a
separate target, and turning it off removes the entire dependency.

### 2.5 A gateway can re-fragment an encrypted message without the key

This is the least obvious gain in the design and it is worth calling out.

The cipher's associated data is the header of the **logical** message,
canonicalized: `payload_size` is the size of the complete encrypted payload,
the `FRAGMENTED` bit is cleared, and `fragment_index`/`fragment_count` go in as
`0` and `1`.

Because the fragmentation fields are excluded from the computation, the tag is
the same no matter how the message was cut. A gateway can therefore reassemble
an encrypted message that arrived over one transport and re-fragment it onto
another **without holding the key**, and without invalidating the tag. Without
that canonicalization, encryption and transport traversal would be mutually
exclusive. [Encryption](encryption.md) has the details.

### 2.6 Conformance is verifiable, not interpretable

The contract is not only prose. Canonical binary vectors ship with the
protocol, each one a readable `.json` beside a raw `.bin`, plus invalid
mutations carrying the exact reason each must be rejected.

An implementation is not ready when its author believes they understood the
text. It is ready when it produces and consumes the same octets as the vectors.
Changing a vector is declaring a change to the contract, with all the version
process that implies.

### 2.7 A command never queues behind telemetry

The logical channels have a defined priority order, with separate queues and
FIFO within each class. Under pressure, telemetry is dropped first, then logs
and periodic status, and the loss is counted. A sender must reserve capacity
for at least one message of each of the two highest classes.

In other words: a burst of telemetry cannot delay a command result, and that is
a rule rather than a recommendation.

### 2.8 Failure is deterministic

A reserved field is zero, and receiving one that is not **causes rejection** —
an unassigned value is never ignored. There is no legacy mode, no
autodetection, no alternative parser. A frame with a mismatched CRC, an invalid
tag or a violated invariant is discarded before routing, with no NACK.

This trades tolerance for diagnosability: an incompatible peer fails
immediately and legibly, instead of half-working for months.

## 3. What the design costs

None of the items below is an implementation defect. All are accepted
consequences, and several are explicit non-goals.

### 3.1 The security model is deliberately narrow

The full picture is in [Encryption](encryption.md). The summary of what is
**not** covered:

| Not covered | Consequence |
| --- | --- |
| Anti-replay | A captured valid frame can be reinjected. There is no written requirement today. |
| Metadata confidentiality | Only the payload is encrypted. `source_id`, `boot_id`, `sequence`, `timestamp_us`, `type` and `object_id` travel in the clear even with `ENCRYPTED` set. |
| Key rotation, forward secrecy | Neither exists. Compromising the key compromises the captured history. |
| Per-peer identity | Authentication is by possession of a shared key. Two peers holding the same key are indistinguishable to the cipher. |

If your environment requires any of those four, wire v2 does not offer them and
you will need a layer outside BTP.

### 3.2 Provisioning stays off the wire

Distributing `source_id` and keys is explicitly out of scope, and a key must
never travel in any field. The two ciphers use different, non-interchangeable
key sizes: 16 octets for AES-128-GCM, 32 for ChaCha20-Poly1305.

The operational consequence: before any deployment you need your own
provisioning mechanism, and the protocol will not help you build it.

### 3.3 The cipher is not negotiated at runtime

Setting `ENCRYPTED` is a static configuration decision made out of band. There
is no signalling, discovery or negotiation, not in `HELLO` and not anywhere
else. The `CIPHER_ID` sub-field identifies which cipher produced the payload;
it negotiates nothing.

A configuration mismatch between two endpoints is by definition a deployment
error: there is no wire case for "one side encrypts and the other does not",
and no fallback to cleartext within a channel.

### 3.4 There are size ceilings, and one of them is tight

| Profile | Frame | Payload per frame | Maximum logical payload |
| --- | ---: | ---: | ---: |
| Serial (COBS) | 4096 | 4056 | bounded by negotiation |
| ESP-NOW | 250 | 210 | 53550 (`255 x 210`) |
| USB HID | 62 | 22 | 5610 (`255 x 22`) |

The 255-fragment ceiling comes from the `fragment_count` field itself, which is
one octet. Above it there is no alternative fragmentation: a large manifest and
a command response use the same common fragmentation, or they do not pass.

USB HID is the tight case. With 22 octets of payload, **even a `HELLO`
fragments** — normal behavior for the profile, not a negotiation failure. It is
also why AEAD is out of scope there: a 16-octet tag over 22 octets of payload is
73% overhead, against ~7.6% on ESP-NOW and ~0.4% on serial. An encoder that
sets `ENCRYPTED` on a frame bound for HID is refused outright.

### 3.5 A boot has a finite number of messages

`sequence` identifies the logical message, is 32 bits wide, and must not wrap
within a boot. That is what makes the triple
(`source_id`, `boot_id`, `sequence`) a reliable identity — and it is what
imposes the ceiling: exhausting the sequence requires a new `boot_id`, not a
silent wrap.

For telemetry at 50 Hz the ceiling is remote. For a high-rate producer that
never restarts, it is a number to compute beforehand, not afterwards.

### 3.6 Telemetry is best-effort by design

There is no per-sample ACK and a lost sample is **never** retransmitted. A full
queue drops telemetry, preferring the most recent sample, and counts the loss.
End-to-end reliability exists only where the logical type defines it: a
`COMMAND_REQUEST` is confirmed by a `COMMAND_RESULT`, not by the transport.

If you need guaranteed delivery of every sample, BTP is not where you get it,
and `TELEMETRY` should not be used as though it were.

### 3.7 The consumer carries obligations

Because nothing allocates, the sizing is yours: how many reassembly slots, how
much storage per slot, how deep the queue per priority class. An overflow is a
counted rejection, not a `realloc`.

And there is one obligation that is easy to forget: a completed message
**keeps occupying its slot**, to keep its payload view stable, until the
consumer releases it. Completed slots also expire if they are never released.

### 3.8 Migration is coordinated, not incremental

There is no legacy mode, no alternative parser and no silent fallback. That
door is closed on purpose.

This is an advantage and a cost at the same time, and both halves are worth
stating. The advantage: no compatibility debt, no old path to keep alive, one
testable behavior. The cost: an incompatible change requires updating consumers
in a coordinated way, and one straggler blocks the set. A wire change is only
complete when **every** supported platform produces and consumes the same
octets.

## 4. Where BTP fits

The protocol was designed for this shape of problem, and this is where it pays
off most:

- **Periodic telemetry with control on the same channel**, from an embedded
  producer to one or a few known consumers.
- **Heterogeneous links in series** — typically a short radio hop followed by a
  local bus — where re-framing without reinterpreting is a requirement.
- **Time series that need the instant of origin**, not of arrival.
- **Environments where conformance must be auditable**: several
  implementations, several languages, and the need to prove equivalence in
  octets.
- **Memory-constrained targets**, where dynamic allocation is unwanted or
  forbidden.
- **A trusted physical network or a controlled perimeter**, with or without
  payload encryption as the case requires.

## 5. Where BTP does not fit

Equally important, with the reason for each:

- **Multi-hop networks with dynamic routing.** Topology, route discovery and
  network addressing are out of scope. The protocol assumes you know where the
  frame is going.
- **Hostile environments requiring anti-replay or strong per-peer identity.**
  See 3.1 — neither exists in this version.
- **Many peers with distinct keys.** There is no key management, rotation or
  cryptographic identity per source.
- **File transfer or large payload streaming.** The 255-fragment ceiling and
  the absence of retransmission make this the wrong job for this protocol.
- **Incremental evolution without coordinating consumers.** See 3.8. If you do
  not control both sides, the absence of a legacy mode is an expensive cost.
- **Service discovery on an open network.** The manifest describes the catalog
  of a known producer; it is not a network discovery mechanism.

## 6. One sentence

BTP trades **network flexibility and tolerance of a divergent peer** for
**determinism, portability and verifiable conformance**, in an envelope small
enough to fit a radio datagram. If your problem is moving measurement and
control from an embedded device to a computer across different links without
losing the origin of time, that trade is favorable. If your problem is
networking, it is not.
