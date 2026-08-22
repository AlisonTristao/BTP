# The model

This chapter defines the vocabulary the rest of the book uses: who talks, what
the channels are, how a message is identified, and what delivery does and does
not guarantee.

## 1. Roles

A BTP path has two required roles and one optional one. A single device may
hold more than one.

### Producer

Creates messages. It owns the identity and the instant of everything it emits:
it assigns `source_id`, picks a fresh `boot_id` at every boot, advances
`sequence`, and stamps `timestamp_us` from its own monotonic clock at the
moment the event happened.

A producer does not reinterpret a `schema_version` it has already emitted.
Changing a field, a type, a unit, an order or the meaning of a value requires a
new version, never a redefinition of an old one.

### Gateway (optional)

Forwards messages between two links. A BTP path may have no gateway at all.

A gateway re-frames but never reinterprets. It does not redefine a telemetry
schema and it does not replace the origin timestamp with the time of arrival.
It may re-fragment a message to fit the outgoing transport — including an
encrypted one, without holding the key, as [Encryption](encryption.md)
explains.

When a gateway also publishes its own catalog of sources, topics and actions,
it becomes a producer of that catalog and takes on a producer's obligations for
it.

### Consumer

Receives, validates and interprets. It owns all the sizing: how many reassembly
slots, how much storage, how deep each queue. It decides what to do with a
sample, but it does not get to invent one — a rejected message is rejected
whole, never partially decoded.

```text
producer  ------------->  gateway  ------------->  consumer
           ESP-NOW                  serial / USB HID
```

The number of hops, the transport of each hop and whether a gateway exists at
all are deployment choices. None of them changes the envelope.

## 2. The five logical channels

The envelope's `type` field selects the channel. Each carries a different
guarantee, and mixing them is the thing the design exists to prevent.

| `type` | Name | Direction | Guarantee |
| ---: | --- | --- | --- |
| `0x01` | `TELEMETRY` | producer to consumer | Best-effort samples, identified by source, topic and schema. Never retransmitted. |
| `0x02` | `LOG` | producer or gateway to consumer | Events and diagnostics. Best-effort. Does not replace telemetry. |
| `0x03` | `COMMAND` | both ways | Request and result. Deduplicated, confirmed end to end. |
| `0x04` | `TERMINAL` | both ways | Opaque bytes. The protocol does not interpret them. |
| `0x05` | `CONTROL` | both ways | Session, discovery, subscriptions and status. |

`0x00` is `INVALID` and is rejected. Values above `0x05` are reserved and are
also rejected — never ignored.

The separation is load-bearing. A terminal must not parse or wait for a
telemetry frame, and terminal bytes must not be published as telemetry or as a
log. Diagnostics and measurement do not share a channel.

Within a channel, `object_id` names the specific object: a topic id for
telemetry, a request or result for commands, one of the control messages for
`CONTROL`. Each channel has its own `object_id` namespace, described in the
chapter for that channel.

## 3. Identity

Every message is identified by a triple, all three parts created by the
producer:

```text
(source_id, boot_id, sequence)
```

**`source_id`** identifies the producer within a routing domain. It must be
non-zero. It is not a link address — see below.

**`boot_id`** must be non-zero and must change on every boot. This is what
makes the triple safe: without it, a producer that restarted and began counting
from zero again would emit sequences it had already used, and a consumer could
not tell a fresh message from a stale one. It also gives a requester a way to
say "execute this against *that* boot", so a command created before a restart
is rejected instead of silently re-executed against different state.

**`sequence`** identifies the logical message, not the frame: every fragment of
one message carries the same `sequence`. It is 32 bits and must not wrap within
a boot. Exhausting it requires a new `boot_id`.

The sequence is shared across all channels of a source. A gap in the numbers
seen on one channel may therefore belong to another channel, and a consumer
must not treat a gap as proof of loss on the channel it happens to be watching.

### Identity is not addressing

There is no link address anywhere in the BTP header. Peers are identified by
`source_id` and `boot_id` and nothing else.

On ESP-NOW the source MAC may be used for routing and diagnostics, but it does
not replace the identity fields and it does not authenticate the frame. USB HID
is point to point and has no peer concept at all. Topology, route discovery and
network addressing are out of scope: the protocol assumes you already know
where the frame is going.

## 4. Time

`timestamp_us` is a 64-bit microsecond count taken from the producer's
monotonic clock at the moment the event occurred. It is not wall-clock time and
it is not comparable across boots.

No intermediary rewrites it. A gateway that replaced it with the arrival time
would turn every downstream time series into a measurement of the transport,
which is exactly the failure the field exists to prevent.

## 5. Delivery

**There is no protocol-level ACK on any transport.** That is a design decision,
not an omission, and it means reliability has to be read per channel rather
than assumed from the link.

Three levels exist and must not be collapsed into one:

| Level | Means | Does not mean |
| --- | --- | --- |
| Accepted locally | The sending API took the bytes | That anything left the device |
| Confirmed on the link | The link layer reported the frame delivered | That anyone interpreted it |
| Completed | A BTP message says the work is done | — |

Concretely, on ESP-NOW: a successful send call is the first level, the
transmit-complete callback is the second, and a `COMMAND_RESULT` is the third.
Treating the first as the third is the classic way to build a system that
appears to work and silently loses commands.

Per channel:

- `TELEMETRY` is at most once. A lost sample is never retransmitted. A full
  queue drops it, prefers the newest sample, and counts the loss.
- `LOG`, `STATUS` and `TERMINAL_OUT` are best-effort.
- `COMMAND_REQUEST` is confirmed by a correlated `COMMAND_RESULT`. Retrying
  means resending **the same identity triple with the same bytes**; the
  executor deduplicates and replays its stored result rather than acting twice.
  A new sequence is deliberately a different command.

## 6. Priority under congestion

Implementations use separate logical queues and apply this order, preserving
FIFO within each class:

1. Session messages (`HELLO`, close) and `COMMAND_RESULT`
2. `COMMAND_REQUEST` and control results
3. Subscription control and `STATUS` marked `DEGRADED`
4. `TERMINAL_IN` and `TERMINAL_OUT`
5. `MANIFEST_DATA`, periodic status and `LOG`
6. `TELEMETRY`

Priority does not change `sequence`, does not allow interrupting a frame
already in flight, and does not turn a best-effort transport into a reliable
one. A sender reserves capacity for at least one message of classes 1 and 2.
Under pressure it drops telemetry first, then logs and periodic status, and
counts the loss. A large manifest and terminal traffic are sliced or scheduled
so they cannot block a command.

## 7. Rejection

Three rules cut across every channel:

**A reserved field is zero.** Receiving a reserved field, flag bit, `type` or
`object_id` with an unassigned value causes rejection. An unknown value is
never ignored and never guessed at.

**There is no legacy mode.** No adapter, no flag, no fallback, no
autodetection, no alternative parser. An incompatible peer fails visibly rather
than half-working.

**A rejected frame is discarded silently.** A frame with a bad CRC, an invalid
tag or a violated invariant is dropped before routing, and no NACK is sent. The
loss is counted, and those counters are published on the `STATUS` message.

Rejecting a *payload* is a different thing from rejecting a *frame*: a
telemetry sample whose schema is unknown is discarded as a sample, but the
frame that carried it was structurally valid and is not a transport error.
