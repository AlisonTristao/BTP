# Commands and discovery

This chapter covers the `COMMAND` channel and the parts of `CONTROL` that
describe what a source offers: the manifest, subscriptions and status. Session
setup and the terminal are in
[Session and terminal](session-and-terminal.md).

All layouts describe the logical payload after reassembly. There is no
alignment, no padding, no implicit terminator and no struct memory
representation anywhere.

## 1. Common primitives

**`utf8_u16`** is `size:uint16_le` followed by exactly `size` octets of UTF-8,
with no BOM and no terminator. `size = 0` is legal and means empty text.

**`bytes_u32`** is `size:uint32_le` followed by exactly `size` arbitrary
octets.

Before adding or allocating anything, a decoder validates each length against
the bytes actually remaining and against the limits in section 6.

**A request reference** is how every response points back at its request:

| Offset | Size | Field | Wire type |
| ---: | ---: | --- | --- |
| 0 | 4 | `request_source_id` | `uint32_le` |
| 4 | 4 | `request_boot_id` | `uint32_le` |
| 8 | 4 | `reply_to_sequence` | `uint32_le` |

`reply_to_sequence` alone is not globally unique, so correlation always uses
all three — which is exactly the requesting message's envelope identity. A
response copies the three values verbatim.

### Result and error codes

| `status` | Name | Meaning |
| ---: | --- | --- |
| `0x00` | `SUCCESS` | Completed |
| `0x01` | `REJECTED` | Valid request, refused |
| `0x02` | `FAILED` | Execution started and failed |
| `0x03` | `TIMEOUT` | Execution limit expired |
| `0x04` | `CANCELLED` | Execution was cancelled |
| `0x05` | `UNSUPPORTED` | Feature or version not supported |
| `0x06` | `BUSY` | Capacity temporarily unavailable |

| `error_code` | Name |
| ---: | --- |
| `0x0000` | `NONE` |
| `0x0001` | `MALFORMED_PAYLOAD` |
| `0x0002` | `UNKNOWN_OBJECT` |
| `0x0003` | `INVALID_ARGUMENT` |
| `0x0004` | `NOT_AUTHORIZED` |
| `0x0005` | `CAPACITY_EXHAUSTED` |
| `0x0006` | `EXECUTION_TIMEOUT` |
| `0x0007` | `INTERNAL_ERROR` |
| `0x0008` | `UNSUPPORTED_VERSION` |
| `0x0009` | `STALE_TARGET_BOOT` |
| `0x000A` | `REQUEST_CONFLICT` |
| `0x000B` | `NOT_FOUND` |
| `0x000C`-`0x7FFF` | Reserved to BTP |
| `0x8000`-`0xFFFF` | Action-specific, described in the manifest |

`SUCCESS` always pairs with `NONE`; any other status always pairs with an error
other than `NONE`. Text messages are human diagnostics only — client logic
depends on the codes.

### `object_id` namespaces

| Channel | `object_id` | Name |
| --- | ---: | --- |
| `COMMAND` | `0x0001` | `COMMAND_REQUEST` |
| `COMMAND` | `0x0002` | `COMMAND_RESULT` |
| `CONTROL` | `0x0001` | `HELLO` |
| `CONTROL` | `0x0002` | `HELLO_RESULT` |
| `CONTROL` | `0x0003` | `MANIFEST_REQUEST` |
| `CONTROL` | `0x0004` | `MANIFEST_DATA` |
| `CONTROL` | `0x0005` | `SUBSCRIBE` |
| `CONTROL` | `0x0006` | `SUBSCRIBE_RESULT` |
| `CONTROL` | `0x0007` | `UNSUBSCRIBE` |
| `CONTROL` | `0x0008` | `UNSUBSCRIBE_RESULT` |
| `CONTROL` | `0x0009` | `STATUS` |
| `CONTROL` | `0x000A` | `SESSION_CLOSE` |
| `CONTROL` | `0x000B` | `SESSION_CLOSE_RESULT` |
| `TERMINAL` | `0x0001` | `TERMINAL_IN` |
| `TERMINAL` | `0x0002` | `TERMINAL_OUT` |

Any other value is reserved and is rejected, never reinterpreted.

## 2. Commands

### 2.1 `COMMAND_REQUEST`

The envelope identifies the requester. The payload is:

| Offset | Size | Field | Wire type |
| ---: | ---: | --- | --- |
| 0 | 4 | `target_source_id` | `uint32_le` |
| 4 | 4 | `target_boot_id` | `uint32_le` |
| 8 | 2 | `action_id` | `uint16_le` |
| 10 | 2 | `action_version` | `uint16_le` |
| 12 | 2 | `flags` | `uint16_le`, zero |
| 14 | 2 | `reserved` | `uint16_le`, zero |
| 16 | 4 | `parameter_size` | `uint32_le` |
| 20 | variable | `parameters` | exactly `parameter_size` octets |

`target_source_id`, `target_boot_id`, `action_id` and `action_version` are all
non-zero; `parameter_size` may be zero.

`target_boot_id` is the interesting field. It pins the intent to the boot it
was created against: an executor that has since restarted refuses the command
with `REJECTED/STALE_TARGET_BOOT` rather than applying it to different state.
A command written for one run of a device does not silently execute against the
next one.

`action_id`, `action_version` and the encoding of `parameters` come from the
target's manifest. Unknown, truncated or over-long parameters yield
`REJECTED/INVALID_ARGUMENT`.

### 2.2 `COMMAND_RESULT`

The envelope identifies the executor and uses its own `sequence`.

| Offset | Size | Field | Wire type |
| ---: | ---: | --- | --- |
| 0 | 12 | request reference | section 1 |
| 12 | 2 | `action_id` | `uint16_le` |
| 14 | 2 | `action_version` | `uint16_le` |
| 16 | 1 | `status` | result code |
| 17 | 1 | `reserved` | `uint8`, zero |
| 18 | 2 | `error_code` | `uint16_le` |
| 20 | 2 + M | `message` | `utf8_u16` |
| 22 + M | 4 + R | `result` | `bytes_u32` |

`action_id` and `action_version` echo the request when it could be parsed; on
`MALFORMED_PAYLOAD` both may be zero. `result` uses the encoding the manifest
announced for that action, and is empty when there is no output schema or when
the status is not `SUCCESS`.

**Exactly one final result** is produced per accepted or rejected request.
Intermediate progress belongs on a telemetry or status topic, not here.

### 2.3 Deduplication

The dedup key is the request message's identity:

```text
(request_source_id, request_boot_id, request_sequence)
```

and the executor also compares **every byte of the logical payload**. The rules:

- First arrival reserves the key *before* starting any effect.
- The same key with the same bytes never starts a second execution. If the
  original is still running, the repeat stays attached to it; if it finished,
  the executor retransmits the identical `COMMAND_RESULT`, same sequence, same
  bytes.
- The same key with **different** bytes is a conflict: not executed, answered
  `REJECTED/REQUEST_CONFLICT`.
- Entries for a live `request_boot_id` are never evicted to make room. When
  `max_dedup_entries` is exhausted, new keys get `BUSY/CAPACITY_EXHAUSTED` and
  old keys stay protected.

An accepted entry stays cached until the executor's boot ends. Session close,
transport disconnection, timeout, a new requester boot or a retransmission do
not authorize early removal. The announced limit is what makes this storage
finite: when it runs out the executor refuses new keys rather than weakening
the guarantee.

An executor restart may clear the cache without risking a repeated effect,
because every earlier request carries the old `target_boot_id` and is now
rejected on that basis.

The manifest may mark a naturally idempotent action `IDEMPOTENT`, but
deduplication is mandatory for all actions regardless. A requester retries by
resending **the same sequence and the same bytes** — using a new sequence
deliberately creates a different command.

## 3. The manifest

`MANIFEST_DATA` describes exactly one source. The envelope identifies who
answered; `described_source_id` and `described_boot_id` identify the source
being catalogued. A gateway may answer from its cache but preserves those ids
and the producer's UUID.

### 3.1 `MANIFEST_REQUEST`

| Offset | Size | Field | Wire type |
| ---: | ---: | --- | --- |
| 0 | 4 | `target_source_id` | `uint32_le`; zero requests the whole catalog |
| 4 | 4 | `target_boot_id` | `uint32_le`; zero accepts the current boot |
| 8 | 4 | `known_config_revision` | `uint32_le`; zero forces full content |

With a non-zero target: a missing source yields `NOT_FOUND`, a mismatched
`target_boot_id` yields `STALE_TARGET_BOOT`, and a `known_config_revision`
equal to the current one allows a `NOT_MODIFIED` answer with no descriptors.

With `target_source_id = 0` the other two fields must be zero. The responder
snapshots the sources it knows, including itself, sorts them by `source_id`,
and emits one complete response per source — so `catalog_count` is always at
least one. Entries that change during enumeration appear only in a later
request; the snapshot never changes mid-answer.

### 3.2 `MANIFEST_DATA`

| Offset | Size | Field | Wire type |
| ---: | ---: | --- | --- |
| 0 | 12 | request reference | section 1 |
| 12 | 1 | `status` | `SUCCESS`, `REJECTED` or `UNSUPPORTED` |
| 13 | 1 | `flags` | bit 0 `NOT_MODIFIED`, bit 1 `CATALOG_COMPLETE` |
| 14 | 2 | `error_code` | `uint16_le` |
| 16 | 2 | `manifest_format_version` | `uint16_le`, value 1 |
| 18 | 2 | `reserved` | `uint16_le`, zero |
| 20 | 4 | `config_revision` | `uint32_le` |
| 24 | 16 | `source_uuid` | 16 octets |
| 40 | 4 | `described_source_id` | `uint32_le` |
| 44 | 4 | `described_boot_id` | `uint32_le` |
| 48 | 1 | `source_role` | `uint8`, same codes as `HELLO` |
| 49 | 1 | `source_flags` | bit 0 `ONLINE`; rest zero |
| 50 | 2 | `catalog_index` | `uint16_le`, from zero |
| 52 | 2 | `catalog_count` | `uint16_le` |
| 54 | 2 | `topic_count` | `uint16_le` |
| 56 | 2 | `action_count` | `uint16_le` |
| 58 | 2 + N | `source_name` | `utf8_u16` |
| variable | variable | topics | `topic_count` records |
| variable | variable | actions | `action_count` records |

For a targeted request, `catalog_index = 0`, `catalog_count = 1` and
`CATALOG_COMPLETE` is set. In an enumeration every response repeats the same
`catalog_count`, uses contiguous indices, and only the last sets
`CATALOG_COMPLETE`. A missing or duplicated response makes the snapshot
incomplete: the client does not publish the new catalog and may retry.

With `NOT_MODIFIED`, `status` is `SUCCESS`, identity and name are still
present, both counts are zero and there are no records. `ONLINE` means the
responder currently has a session with that source; cached sources may be
announced offline.

### 3.3 Records

Every record begins with `record_size:uint32_le`, counting the bytes after the
size itself. The decoder bounds its reads to the record, consumes exactly
`record_size`, and rejects the whole manifest on any inconsistency. Unknown
trailing content is **not** ignored.

A **topic record** carries `topic_id` and `schema_version` (both non-zero
`uint16_le`), `encoding:uint8`, `flags:uint8` (bit 0 `SUBSCRIBABLE`),
`field_count:uint16_le`, `max_rate_millihz:uint32_le` (zero means not
periodic), `name` and `description` as `utf8_u16`, then `field_count` field
records.

A **field record** carries `field_id` and `order` (`uint16_le`), `type:uint8`,
`flags:uint8` (bit 0 `NULLABLE`, bit 1 `VARIABLE_COUNT`), `element_count` and
`max_element_count` (`uint16_le`), `scale` and `offset` as finite little-endian
IEEE-754 `float64`, `enum_count:uint16_le`, `name`, `unit` and `description` as
`utf8_u16`, then `enum_count` entries of `value:uint16_le` plus a
`label:utf8_u16`.

Type codes follow [Telemetry payloads](telemetry.md#5-wire-types) in order:
`0x01=uint8`, `0x02=uint16`, `0x03=uint32`, `0x04=uint64`, `0x05=int8`,
`0x06=int16`, `0x07=int32`, `0x08=int64`, `0x09=float32`, `0x0A=float64`,
`0x0B=bool`, `0x0C=enum8`, `0x0D=enum16`. Zero and higher values are reserved,
except for two descriptor-only codes: `0x0E=opaque_bytes` and `0x0F=utf8`,
which represent the single optional logical field of a whole-body
`OPAQUE_BYTES` or `UTF8` topic. Those two are not valid in a structured topic
or in an action schema.

An **action record** carries `action_id` and `action_version` (non-zero),
`flags:uint16_le` (bit 0 `IDEMPOTENT`, bit 1 `DANGEROUS`),
`parameter_encoding` and `result_encoding` (`uint8`), the two field counts
(`uint16_le`), a non-zero `execution_timeout_ms:uint32_le`, `name`,
`description` and `confirmation_text` as `utf8_u16`, then the parameter and
result field records in increasing `order`, then `error_count:uint16_le` and
that many `error_code:uint16_le` plus `label:utf8_u16` entries.

Action encodings are `0x00=EMPTY`, `0x05=PACKED_LE` and `0x06=TLV_LE`. `EMPTY`
requires zero fields; the other two use the telemetry field and encoding rules
exactly, without a `schema_version` prefix. Action-specific errors live in
`0x8000`-`0xFFFF`.

`DANGEROUS` requires explicit user confirmation, and `confirmation_text` comes
from the source and must be non-empty when that flag is set. It does not
authorize a client to invent local semantics.

Ids and versions are stable. Any change to the interpretation of a topic,
field, parameter, result or action increments the respective version **and**
`config_revision`, and a revision is never reused for different content. A
changed `config_revision` invalidates cached descriptors: the client pauses
decoding of unknown schemas and requests a fresh manifest rather than guessing
from the previous revision.

## 4. Subscriptions

`SUBSCRIBE` asks for periodic publication of a topic:

| Offset | Size | Field | Wire type |
| ---: | ---: | --- | --- |
| 0 | 4 | `target_source_id` | `uint32_le` |
| 4 | 4 | `target_boot_id` | `uint32_le`, non-zero |
| 8 | 2 | `topic_id` | `uint16_le`, non-zero |
| 10 | 2 | `flags` | zero |
| 12 | 4 | `requested_rate_millihz` | `uint32_le`, non-zero |
| 16 | 4 | `requested_lease_ms` | `uint32_le`, non-zero |

`SUBSCRIBE_RESULT` carries the request reference, `status:uint8`,
`reserved:uint8`, `error_code:uint16_le`, `subscription_id:uint32_le`,
`effective_rate_millihz:uint32_le` and `granted_lease_ms:uint32_le`. On success
the last three are non-zero; on error they are zero.

The effective rate never exceeds the requested rate or the manifest's maximum.
Publication may jitter and may lose samples — telemetry stays best-effort and
gets no per-sample ACK, and a subscription does not change that.

Repeating the identical request returns the same subscription rather than
creating another. A new sequence atomically creates or replaces the
subscription for that session and topic. A subscription expires after its lease
unless renewed by another `SUBSCRIBE`.

`UNSUBSCRIBE` carries `target_source_id`, `target_boot_id` and
`subscription_id`. Removing an already-absent subscription returns
`SUCCESS/NONE`, which makes retries idempotent.

## 5. Status

`STATUS` is spontaneous and gets no response. The envelope's `source_id` and
`boot_id` scope the counters.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 2 | `status_version` (`uint16_le`, 1 or 2) |
| 2 | 2 | `flags`, bit 0 `DEGRADED` |
| 4 | 8 | `uptime_us` |
| 12 | 8 | `frames_rx` |
| 20 | 8 | `frames_tx` |
| 28 | 8 | `frames_dropped` |
| 36 | 8 | `crc_errors` |
| 44 | 8 | `decode_errors` |
| 52 | 8 | `reassembly_completed` |
| 60 | 8 | `reassembly_timeouts` |
| 68 | 8 | `reassembly_rejected` |
| 76 | 8 | `command_duplicates` |
| 84 | 8 | `telemetry_dropped` |

All counters are `uint64_le`. The payload is exactly 92 octets when
`status_version = 1`.

Counters are monotonic, saturate at `UINT64_MAX`, and reset only with a new
`boot_id` — which is the point: a counter that resets on reconnection cannot
tell you anything about a flaky link. `frames_dropped` covers valid frames
dropped by queues or capacity; `crc_errors` counts CRC rejections;
`decode_errors` counts malformed envelopes or payloads. One event may
contribute to both a specific counter and `frames_dropped`, but never twice to
the same counter.

### 5.1 Per-topic extension

A sender that tracks per-topic consumption may publish `status_version = 2`.
The 92 octets above stay at the same offsets, followed by:

| Offset | Size | Field |
| ---: | ---: | --- |
| 92 | 2 | `topic_status_count` (`uint16_le`) |
| 94 | 28 x T | `topic_status` records |

Each 28-octet record has no `record_size` of its own — the count already
delimits the list:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | `source_id` (`uint32_le`, non-zero) |
| 4 | 2 | `topic_id` (`uint16_le`, non-zero) |
| 6 | 2 | `subscriber_count` (`uint16_le`) |
| 8 | 4 | `effective_rate_millihz`; zero means not currently published |
| 12 | 8 | `bytes_total` (`uint64_le`) |
| 20 | 8 | `samples_dropped_total` (`uint64_le`) |

`source_id` is present because `topic_id` alone is not globally unique: a
gateway reporting more than one source disambiguates by the pair, never by
`topic_id` alone. A source describing only itself uses its own `source_id` in
every record.

A `status_version = 1` message must not include these fields, and a receiver
stops reading at 92 octets when it sees version 1. A receiver uninterested in
per-topic metrics may ignore the extension entirely — which is what keeps a
version-1-only reader working. `topic_status_count` may be zero. Record order
is not significant, but the `(source_id, topic_id)` pair is unique within one
message.

## 6. Limits

| Limit | Value |
| --- | ---: |
| Logical payload on a path including ESP-NOW | 53550 octets |
| Logical manifest | 49152 octets |
| Individual `utf8_u16` text | 1024 octets |
| Name or unit | 128 octets |
| Result message | 512 octets |
| Action parameters or result | 32768 octets |
| Fields per topic or per action side | 256 |
| Sources per catalog | 1024 |
| Topics per manifest | 1024 |
| Actions per manifest | 1024 |
| Enum or error entries per record | 256 |

The effective limit is always the smallest of this table, the negotiated
session limits, the manifest and the transport. A producer never announces a
descriptor whose maximum size would violate the negotiated path.

A manifest or command result that exceeds one frame uses the ordinary
fragmentation from
[Getting it across the link](fragmentation-and-transports.md). There is no
alternative internal fragmentation.

## 7. Validation order

After validating the envelope and completing reassembly, a receiver:

1. resolves `type` and `object_id` with no fallback;
2. requires the minimum fixed size and validates reserved fields;
3. validates every length before reading or allocating;
4. validates ids, versions, codes, flags, counts and limits;
5. requires the logical payload to be consumed exactly;
6. only then changes session state, executes an action, publishes a catalog or
   delivers bytes to the terminal.

An error in a fragmented message is reported only after the message has been
safely identified, and never delivers partial content.
