# Session and terminal

A BTP session is the agreement between two peers about which envelope version
they speak and what limits they will respect. This chapter covers how one
starts, how it ends, and the opaque terminal channel that lives inside it.

## 1. `HELLO`

`HELLO` is the first BTP message of a session, and **no other message may
precede a successful `HELLO_RESULT`**.

| Offset | Size | Field | Wire type |
| ---: | ---: | --- | --- |
| 0 | 1 | `role` | `uint8` |
| 1 | 1 | `version_count` | `uint8` |
| 2 | 2 | `flags` | `uint16_le`, zero |
| 4 | 4 | `max_logical_payload` | `uint32_le` |
| 8 | 2 | `max_inflight_reassemblies` | `uint16_le` |
| 10 | 2 | `max_subscriptions` | `uint16_le` |
| 12 | 4 | `max_dedup_entries` | `uint32_le` |
| 16 | 4 | `session_timeout_ms` | `uint32_le` |
| 20 | 16 | `peer_uuid` | 16 octets |
| 36 | 4 | `config_revision` | `uint32_le` |
| 40 | V | `versions` | `version_count` increasing `uint8` values |

`peer_uuid` is an opaque, stable 16-octet identity compared byte for byte. It
must not be all zero, and no endianness conversion is applied to it.

`config_revision = 0` means the peer publishes no manifest. When it has a
catalog the revision is monotonic and starts at 1.

Capabilities and the timeout must all be non-zero. On a path that includes an
ESP-NOW hop, `max_logical_payload` is at most 53550.

`versions` enumerates the envelope versions the sender can use, in increasing
order with no duplicates and no zero. Today those are `0x01` and `0x02`.

### Roles

| Value | Role |
| ---: | --- |
| `0x01` | producer |
| `0x02` | gateway |
| `0x03` | consumer |
| `0x04` | consumer, announcing itself as a diagnostic tool |

Zero and `0x05`-`0xFF` are reserved.

What travels is the number, and the number is what the conformance vectors pin
down. `0x03` and `0x04` are both consumers and the protocol defines no
behavioral difference between them; the distinction exists so a producer can
tell an ordinary client from a diagnostic one if it wants to.

## 2. `HELLO_RESULT`

| Offset | Size | Field | Wire type |
| ---: | ---: | --- | --- |
| 0 | 12 | request reference | [commands](commands.md#1-common-primitives) |
| 12 | 1 | `status` | `SUCCESS` or `UNSUPPORTED` |
| 13 | 1 | `selected_version` | `uint8`; zero on failure |
| 14 | 2 | `error_code` | `uint16_le` |
| 16 | 4 | `max_logical_payload` | effective value |
| 20 | 2 | `max_inflight_reassemblies` | effective value |
| 22 | 2 | `max_subscriptions` | effective value |
| 24 | 4 | `max_dedup_entries` | effective value |
| 28 | 4 | `session_timeout_ms` | effective value |
| 32 | 16 | `peer_uuid` | the responder's UUID |
| 48 | 4 | `config_revision` | the responder's revision |

The responder picks the highest version it can use **on every link of the
session** — a gateway bridging two hops cannot select a version one of them
cannot carry.

Every effective limit is the minimum of the announced and the local
capability. That rule is what keeps a small embedded peer safe: a desktop
announcing a large `max_logical_payload` does not get to impose it.

On success all limits are non-zero. With no version in common, the responder
answers `UNSUPPORTED/UNSUPPORTED_VERSION` with a zero version and zero limits,
and closes the session after transmitting the response.

## 3. Entering protocol mode on serial

Serial is the only transport with a human console mode. The port starts in that
mode, and the only line it recognizes as a control line is:

```text
BTP/1 ENTER NNNNNNNNNNNNNNNN\r\n
```

where `NNNNNNNNNNNNNNNN` is 16 hexadecimal digits chosen by the client, in
either case. The port owner answers with the same nonce in lowercase:

```text
BTP/1 READY nnnnnnnnnnnnnnnn\r\n
```

Binary framing begins only after the `\n` of `READY`, and the COBS decoder
state is cleared at that instant. The client then sends `HELLO` within 2000 ms
and nothing before it.

An invalid or incomplete line stays ordinary console input. **Binary framing is
never autodetected** — there is no heuristic that sniffs incoming bytes for
something frame-shaped, because a heuristic that can be triggered by ordinary
console text is a heuristic that will be.

The other two profiles have no console. ESP-NOW and USB HID are always in
protocol mode and must be ready for `HELLO` as soon as the link is open.

## 4. Leaving protocol mode

Protocol mode ends through a BTP exchange, never by scanning the encoded bytes
for an escape sequence.

`SESSION_CLOSE` carries `reason:uint8`, three reserved zero octets, and
`drain_timeout_ms:uint32_le`:

| `reason` | Name |
| ---: | --- |
| `0x00` | `NORMAL` |
| `0x01` | `VERSION_MISMATCH` |
| `0x02` | `CLIENT_SHUTDOWN` |
| `0x03` | `PROTOCOL_ERROR` |

`SESSION_CLOSE_RESULT` carries the request reference, `status:uint8`,
`reserved:uint8` and `error_code:uint16_le`.

On a valid close the port owner stops accepting new work, waits at most
`min(drain_timeout_ms, 2000)` ms to transmit the result, discards incomplete
reassemblies, and only then returns to the console — emitting exactly:

```text
BTP/1 CONSOLE\r\n
```

## 5. The watchdog

If no valid BTP frame arrives for `session_timeout_ms`, or if `HELLO` does not
arrive within its 2000 ms window, the peer performs the same cleanup and
returns to the console.

**Invalid traffic does not renew the watchdog.** A stream of corrupt bytes is
not evidence that a session is alive; if anything it is evidence of the
opposite, and a watchdog fed by garbage would keep a dead session open forever.

Losing the transport does not authorize repeating commands and does not clear
the deduplication cache. Those entries survive until the executor's boot ends —
see [Commands](commands.md#23-deduplication).

## 6. Timeouts

Every reassembly and every operation completes within the announced limits.

An action's execution timeout comes from the manifest. When it expires, the
executor answers `TIMEOUT/EXECUTION_TIMEOUT` and prevents the action from
producing further effects. If an implementation cannot guarantee that, the
action must not be announced as a synchronous BTP command in the first place.

## 7. The terminal

The entire logical payload of `TERMINAL_IN` and `TERMINAL_OUT` is opaque bytes,
delimited only by `payload_size` or by the reassembled size.

`0x00`, CR, LF, invalid UTF-8 and sequences that happen to look like BTP frames
are all valid data. There is no prefix, no terminator and no requirement that
the content be text.

`TERMINAL_IN` flows from the session controller to the source's terminal input;
`TERMINAL_OUT` flows the other way. Fragments are delivered only after
reassembly.

Between complete messages from one origin, the consumer uses `sequence` to
order them and to help detect loss — but the sequence is shared by all channels
of a source, so a gap may belong to another channel entirely. The terminal must
never fabricate or pad missing bytes to fill a gap.

Terminal and telemetry stay semantically separate. A terminal must not parse,
display or wait for a `TELEMETRY` frame, and terminal bytes must never be
published as telemetry or as a log.

## 8. Priority

Under congestion, implementations use separate logical queues in this order,
FIFO within each class:

1. Session messages and `COMMAND_RESULT`
2. `COMMAND_REQUEST` and control results
3. Subscription control and `STATUS` marked `DEGRADED`
4. `TERMINAL_IN` and `TERMINAL_OUT`
5. `MANIFEST_DATA`, periodic status and `LOG`
6. `TELEMETRY`

Priority never changes `sequence`, never interrupts a frame already in flight,
and never turns a best-effort transport into a reliable one. A sender reserves
capacity for at least one message of classes 1 and 2, drops telemetry first
under pressure, counts the loss, and slices or schedules a large manifest and
terminal traffic so they cannot block a command.
