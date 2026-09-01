# Commands and discovery

BTP uses the `COMMAND` and `CONTROL` message types for request/response operations and runtime discovery.

This chapter defines:

* command requests and results;
* request correlation;
* command deduplication;
* manifest discovery;
* telemetry subscriptions;
* protocol status messages.

Session establishment and terminal traffic are defined separately in [Session and terminal](session-and-terminal.md).

All layouts below describe the **complete logical payload after reassembly**.

BTP payload structures use explicit wire fields. There is no implicit padding, alignment or native structure layout.

Every payload in this chapter has a struct ⇄ bytes implementation in the reference library's `btp::messages` (`btp/messages.hpp`): a `decode_*` / `encode_*` pair for the fixed messages, `ManifestReader` / `ManifestWriter` for `MANIFEST_DATA`. See [Using the library §12](library.md#12-the-message-layer). The layouts here remain the authority; the library is checked against them by `test-vectors/v2/messages/`.

---

## 1. Common primitives

Several messages in this chapter use the same basic representations.

### 1.1 Length-prefixed UTF-8

`utf8_u16` is encoded as:

```text
size:uint16_le
data[size]
```

`data` contains exactly `size` octets of UTF-8.

There is:

* no BOM;
* no implicit null terminator.

A zero size represents an empty string.

The receiver validates the declared size before reading the data.

---

### 1.2 Length-prefixed bytes

`bytes_u32` is encoded as:

```text
size:uint32_le
data[size]
```

The contents are arbitrary octets.

The receiver validates the declared size against the remaining logical payload and all applicable protocol limits before consuming the data.

---

### 1.3 Request references

Responses identify the request to which they belong using:

| Offset | Size | Field               |
| -----: | ---: | ------------------- |
|      0 |    4 | `request_source_id` |
|      4 |    4 | `request_boot_id`   |
|      8 |    4 | `reply_to_sequence` |

All values are `uint32_le`.

Together:

```text
(
    request_source_id,
    request_boot_id,
    reply_to_sequence
)
```

identify one request.

`reply_to_sequence` alone is not sufficient because sequence values belong to a source and boot namespace.

The response is itself a new BTP message and therefore has its own envelope `sequence`.

---

### 1.4 Result codes

Operations that produce a result use the following status values:

|  Value | Name          |
| -----: | ------------- |
| `0x00` | `SUCCESS`     |
| `0x01` | `REJECTED`    |
| `0x02` | `FAILED`      |
| `0x03` | `TIMEOUT`     |
| `0x04` | `CANCELLED`   |
| `0x05` | `UNSUPPORTED` |
| `0x06` | `BUSY`        |

Common error codes are:

|             Value | Name                  |
| ----------------: | --------------------- |
|          `0x0000` | `NONE`                |
|          `0x0001` | `MALFORMED_PAYLOAD`   |
|          `0x0002` | `UNKNOWN_OBJECT`      |
|          `0x0003` | `INVALID_ARGUMENT`    |
|          `0x0004` | `NOT_AUTHORIZED`      |
|          `0x0005` | `CAPACITY_EXHAUSTED`  |
|          `0x0006` | `EXECUTION_TIMEOUT`   |
|          `0x0007` | `INTERNAL_ERROR`      |
|          `0x0008` | `UNSUPPORTED_VERSION` |
|          `0x0009` | `STALE_TARGET_BOOT`   |
|          `0x000A` | `REQUEST_CONFLICT`    |
|          `0x000B` | `NOT_FOUND`           |
| `0x000C`–`0x7FFF` | Reserved              |
| `0x8000`–`0xFFFF` | Action-specific       |

A successful result uses:

```text
status     = SUCCESS
error_code = NONE
```

A non-success result uses an error code other than `NONE`.

---

### 1.5 `object_id` namespaces

The interpretation of `object_id` depends on the envelope `type`.

For `COMMAND`:

| `object_id` | Operation         |
| ----------: | ----------------- |
|    `0x0001` | `COMMAND_REQUEST` |
|    `0x0002` | `COMMAND_RESULT`  |

For `CONTROL`:

| `object_id` | Operation              |
| ----------: | ---------------------- |
|    `0x0001` | `HELLO`                |
|    `0x0002` | `HELLO_RESULT`         |
|    `0x0003` | `MANIFEST_REQUEST`     |
|    `0x0004` | `MANIFEST_DATA`        |
|    `0x0005` | `SUBSCRIBE`            |
|    `0x0006` | `SUBSCRIBE_RESULT`     |
|    `0x0007` | `UNSUBSCRIBE`          |
|    `0x0008` | `UNSUBSCRIBE_RESULT`   |
|    `0x0009` | `STATUS`               |
|    `0x000A` | `SESSION_CLOSE`        |
|    `0x000B` | `SESSION_CLOSE_RESULT` |

Unassigned values are reserved.

---

## 2. Commands

A command requests execution of an action exposed by a target source.

The available actions and their parameter/result schemas are described by the source manifest.

The basic exchange is:

```text
Requester                              Target
   |                                     |
   |--------- COMMAND_REQUEST ---------->|
   |                                     |
   |<--------- COMMAND_RESULT -----------|
```

---

### 2.1 `COMMAND_REQUEST`

The logical payload is:

| Offset |     Size | Field              | Wire type   |
| -----: | -------: | ------------------ | ----------- |
|      0 |        4 | `target_source_id` | `uint32_le` |
|      4 |        4 | `target_boot_id`   | `uint32_le` |
|      8 |        2 | `action_id`        | `uint16_le` |
|     10 |        2 | `action_version`   | `uint16_le` |
|     12 |        2 | `flags`            | `uint16_le` |
|     14 |        2 | `reserved`         | `uint16_le` |
|     16 |        4 | `parameter_size`   | `uint32_le` |
|     20 | variable | `parameters`       | octets      |

The following fields must be non-zero:

```text
target_source_id
target_boot_id
action_id
action_version
```

`flags` and `reserved` are zero in version 2.

`parameters` contains exactly `parameter_size` octets.

The parameter representation is determined by the action descriptor associated with:

```text
(action_id, action_version)
```

in the target manifest.

---

### 2.2 Target boot validation

A command is addressed to:

```text
(target_source_id, target_boot_id)
```

The target compares `target_boot_id` with its current `boot_id`.

If they differ, the command is not executed and the result is:

```text
REJECTED
STALE_TARGET_BOOT
```

This prevents a request created for one source boot from being applied to another.

---

### 2.3 Action version

The target resolves the command using:

```text
(action_id, action_version)
```

The requested version must correspond to a currently supported action descriptor.

The receiver does not substitute another action version automatically.

An unsupported version is rejected.

---

### 2.4 `COMMAND_RESULT`

The logical payload is:

|   Offset |     Size | Field             | Wire type   |
| -------: | -------: | ----------------- | ----------- |
|        0 |       12 | request reference | section 1.3 |
|       12 |        2 | `action_id`       | `uint16_le` |
|       14 |        2 | `action_version`  | `uint16_le` |
|       16 |        1 | `status`          | `uint8`     |
|       17 |        1 | `reserved`        | `uint8`     |
|       18 |        2 | `error_code`      | `uint16_le` |
|       20 | variable | `message`         | `utf8_u16`  |
| variable | variable | `result`          | `bytes_u32` |

When the request was decoded successfully:

```text
action_id
action_version
```

identify the requested action.

If the request could not be parsed far enough to determine them, they may be zero.

The result body uses the result encoding defined by the corresponding action descriptor.

For a non-success status, the result body is empty unless explicitly defined otherwise by the protocol version.

---

### 2.5 Command deduplication

`btp::messages` encodes and decodes `COMMAND_REQUEST` / `COMMAND_RESULT` and the request reference that correlates them, but the deduplication cache below is the integration's state, not the library's ([Using the library §11.4](library.md#114-command-execution-and-deduplication-are-above-the-payload-layer)).

Command requests are deduplicated using the request identity:

```text
(
    request_source_id,
    request_boot_id,
    request_sequence
)
```

The executor retains the logical request associated with this key.

For a previously unseen key, the request is processed normally.

For an existing key:

```text
same key + same logical request
```

is treated as a retransmission of the original command.

The command is not executed again.

If execution has already completed, the previously generated `COMMAND_RESULT` is retransmitted.

If:

```text
same key + different logical request
```

is received, the new request is rejected with:

```text
REJECTED
REQUEST_CONFLICT
```

A request sequence therefore cannot identify two different commands within the same requester boot.

---

### 2.6 Deduplication capacity

The deduplication cache is bounded.

If no additional entry can be retained, a new command request may be rejected with:

```text
BUSY
CAPACITY_EXHAUSTED
```

Existing entries are not discarded in a way that would permit a previously processed command to be executed again under the same request identity.

The deduplication state is scoped to the executor boot.

---

## 3. The manifest

The manifest describes the BTP objects exposed by a source.

It provides the information required to interpret telemetry topics and command actions.

A consumer obtains it using:

```text
MANIFEST_REQUEST
        |
        v
MANIFEST_DATA
```

The manifest is versioned independently through:

```text
config_revision
```

A source increments this revision when the described configuration changes.

---

### 3.1 `MANIFEST_REQUEST`

The payload is:

| Offset | Size | Field                   |
| -----: | ---: | ----------------------- |
|      0 |    4 | `target_source_id`      |
|      4 |    4 | `target_boot_id`        |
|      8 |    4 | `known_config_revision` |

All fields are `uint32_le`.

---

#### Targeted request

When:

```text
target_source_id != 0
```

the request targets one source.

`target_boot_id` may be zero to accept the current boot.

If it is non-zero and differs from the source's current boot, the request fails with:

```text
STALE_TARGET_BOOT
```

A `known_config_revision` of zero requests complete manifest data.

A non-zero revision tells the responder which manifest revision the requester already has.

---

#### Full catalog request

When:

```text
target_source_id = 0
```

the requester asks for all sources currently known by the responder.

In this mode:

```text
target_boot_id        = 0
known_config_revision = 0
```

The responder creates a catalog snapshot and sends one `MANIFEST_DATA` logical message for each source in the snapshot.

Entries are ordered by `source_id`.

---

### 3.2 `MANIFEST_DATA`

The payload begins with:

|   Offset |     Size | Field                     |
| -------: | -------: | ------------------------- |
|        0 |       12 | request reference         |
|       12 |        1 | `status`                  |
|       13 |        1 | `flags`                   |
|       14 |        2 | `error_code`              |
|       16 |        2 | `manifest_format_version` |
|       18 |        2 | `reserved`                |
|       20 |        4 | `config_revision`         |
|       24 |       16 | `source_uuid`             |
|       40 |        4 | `described_source_id`     |
|       44 |        4 | `described_boot_id`       |
|       48 |        1 | `source_role`             |
|       49 |        1 | `source_flags`            |
|       50 |        2 | `catalog_index`           |
|       52 |        2 | `catalog_count`           |
|       54 |        2 | `topic_count`             |
|       56 |        2 | `action_count`            |
|       58 | variable | `source_name`             |
| variable | variable | `source_info`             |
| variable | variable | topic records             |
| variable | variable | action records            |

`manifest_format_version` is `1` or `2`.

Format `2` is identical to format `1` up to and including `source_name`, then
inserts the `source_info` block defined in [Source info](#312-source-info)
before the topic records. Format `1` has no `source_info` block.

A responder sends the highest format it implements. There is no format
negotiation in `MANIFEST_REQUEST`, so a requester that only implements format
`1` rejects a format `2` response, and a deployment moves both ends together.
A requester that implements format `2` also accepts format `1` and treats its
`source_info` as empty.

`source_name` is encoded as `utf8_u16`.

Manifest flags currently define:

```text
bit 0 -> NOT_MODIFIED
bit 1 -> CATALOG_COMPLETE
```

Source flags currently define:

```text
bit 0 -> ONLINE
```

All unassigned flag bits are zero.

When `status` is not `SUCCESS` (a `REJECTED` response to a targeted request for
an unknown source, a `STALE_TARGET_BOOT`, and so on) the response describes no
source. `source_role`, `source_uuid`, `described_source_id`, `described_boot_id`,
`config_revision`, `topic_count` and `action_count` are then "don't care" and
are conventionally zero; `source_name` may carry a short human diagnostic. A
reader validates `source_role` only on a `SUCCESS` descriptor.

---

### 3.3 `NOT_MODIFIED`

When the requester provides the current `config_revision`, the responder may return:

```text
status = SUCCESS
NOT_MODIFIED = 1
```

In this case:

```text
topic_count  = 0
action_count = 0
```

and no topic or action records follow.

In format `2`, the `source_info` block still follows `source_name` exactly as
in a full response. `source_info` is informational rather than a descriptor,
and is not covered by `config_revision` (see [Source info](#312-source-info)),
so a consumer receives the current `source_info` even when the catalog itself
was not modified.

The returned identity and revision still identify the manifest to which the response refers.

---

### 3.4 Catalog enumeration

For a targeted request:

```text
catalog_index = 0
catalog_count = 1
```

For a full catalog request:

```text
catalog_count
```

is the total number of `MANIFEST_DATA` responses in the snapshot.

`catalog_index` ranges from:

```text
0
```

to:

```text
catalog_count - 1
```

The final entry sets:

```text
CATALOG_COMPLETE
```

A catalog snapshot is considered complete only when all expected entries are received consistently.

---

### 3.5 Manifest records

Topic, field and action descriptors are encoded as length-delimited records.

Each record begins with:

```text
record_size:uint32_le
```

followed by exactly `record_size` octets.

The decoder validates the record size before parsing its contents and requires exact consumption of the record.

In `btp::messages` this run is walked, not decoded whole: `ManifestReader::next_topic` / `next_action` hand back the raw record bytes, and `FieldRecordReader` / `EnumEntryReader` / `ActionErrorReader` walk them without allocation ([Using the library §12.3](library.md#123-reading-and-writing-manifest_data)). A relay that forwards a manifest without inspecting it skips the walk entirely: `ManifestReader::raw_records` returns the whole topic- and action-record runs as spans and `ManifestWriter::put_raw_records` splices them back.

---

### 3.6 Topic records

A topic record contains:

```text
topic_id:uint16_le
schema_version:uint16_le
encoding:uint8
flags:uint8
field_count:uint16_le
max_rate_millihz:uint32_le

name:utf8_u16
description:utf8_u16

field records...
```

`topic_id` and `schema_version` must be non-zero.

Topic flags currently define:

```text
bit 0 -> SUBSCRIBABLE
```

`max_rate_millihz` specifies the maximum periodic publication rate announced for the topic.

The telemetry payload interpretation is defined in [Telemetry payloads](telemetry.md).

---

### 3.7 Field records

A field record contains:

```text
field_id:uint16_le
order:uint16_le
type:uint8
flags:uint8

element_count:uint16_le
max_element_count:uint16_le

scale:float64_le
offset:float64_le

enum_count:uint16_le

name:utf8_u16
unit:utf8_u16
description:utf8_u16

enum entries...
```

Field flags currently define:

```text
bit 0 -> NULLABLE
bit 1 -> VARIABLE_COUNT
```

`scale` and `offset` must be finite IEEE-754 `float64` values.

The field types and serialization rules are defined in [Telemetry payloads](telemetry.md).

---

### 3.8 Enum entries

An enum descriptor entry contains:

```text
value:uint16_le
label:utf8_u16
```

`value` is the wire value.

`label` is descriptive metadata.

---

### 3.9 Action records

An action record contains:

```text
action_id:uint16_le
action_version:uint16_le
flags:uint16_le

parameter_encoding:uint8
result_encoding:uint8

parameter_field_count:uint16_le
result_field_count:uint16_le

execution_timeout_ms:uint32_le

name:utf8_u16
description:utf8_u16
confirmation_text:utf8_u16

parameter field records...

result field records...

error_count:uint16_le

action-specific error entries...
```

`action_id` and `action_version` must be non-zero.

Action flags currently define:

```text
bit 0 -> IDEMPOTENT
bit 1 -> DANGEROUS
```

The protocol-defined action encodings are:

|  Value | Encoding    |
| -----: | ----------- |
| `0x00` | `EMPTY`     |
| `0x05` | `PACKED_LE` |
| `0x06` | `TLV_LE`    |

`EMPTY` requires zero fields.

`PACKED_LE` and `TLV_LE` use the same serialization rules defined for telemetry fields.

Action parameter and result bodies do **not** contain a `schema_version` prefix because the action version is already carried by the command request/result.

---

### 3.10 Action-specific errors

Action-specific error descriptors use values from:

```text
0x8000 .. 0xFFFF
```

Each entry contains:

```text
error_code:uint16_le
label:utf8_u16
```

Values below `0x8000` are reserved for common BTP errors.

---

### 3.11 Stable identifiers and revisions

A manifest does not reuse an identifier or version for a different interpretation.

A change that modifies the interpretation of a topic or action requires the corresponding version to change.

A change to manifest content also requires a new:

```text
config_revision
```

A consumer receiving an unknown schema or action version must obtain the corresponding current descriptor before interpreting that payload.

---

### 3.12 Source info

Format `2` of `MANIFEST_DATA` carries a `source_info` block immediately after
`source_name` and before the topic records:

| Offset | Size | Field        | Wire type                 |
| -----: | ---: | ------------ | ------------------------- |
|      0 |    2 | `info_count` | `uint16_le`               |
|      2 |    V | `entries`    | `info_count` × info entry |

Each info entry is three consecutive `utf8_u16` strings:

```text
key:utf8_u16
label:utf8_u16
value:utf8_u16
```

`key` is a stable machine identifier for the datum, for example `fw_version`.

`label` is a human-readable name for display, for example `Firmware`. It may
be empty, in which case a consumer displays `key`.

`value` is the datum itself, always textual. A numeric quantity is carried as
its text form, for example `"2"` or `"16777216"`.

BTP does not define a registry of `key` values and assigns no meaning to any
of them. A consumer that does not recognise a `key` still displays the entry
from `label` and `value`. The entries are unordered, a `key` should not repeat
within one block, and every entry is optional: `info_count = 0` is valid and
means the source published no info.

---

#### Purpose and scope

`source_info` describes attributes of the source that help a human operator or
a diagnostic tool identify and inspect it: a configured name or description,
the firmware version and build identity, the chip, the running firmware
partition, where an update is served.

It is not part of the object catalog. It does not affect how telemetry topics
or command actions are interpreted, and a change limited to `source_info` does
not require a new `config_revision`. A consumer therefore receives the current
`source_info` on every full and `NOT_MODIFIED` response, regardless of the
revision it has cached.

A source keeps `source_info` small. A value that changes continuously — uptime,
free memory, a counter — belongs in [Status](#5-status), not here. A secret,
such as an update password, is never placed in `source_info`.

---

#### Conventional keys

These `key` spellings let two sources describe the same thing the same way.
They are conventions, not a registry: nothing validates them, no consumer
behaviour depends on them, and a source is free to use others.

| `key`           | Meaning                                             |
| --------------- | -------------------------------------------------- |
| `name`          | Configured human name for this source              |
| `description`   | Free-text description                              |
| `model`         | Hardware model                                     |
| `fw_version`    | Firmware version string                            |
| `fw_build`      | Firmware build timestamp                           |
| `chip`          | Chip model                                         |
| `chip_revision` | Silicon revision                                   |
| `ota_partition` | Running firmware partition                         |
| `ota_endpoint`  | Where a firmware update is served — never a secret |

---

## 4. Subscriptions

`SUBSCRIBE` requests periodic publication of one telemetry topic.

The topic must be announced as subscribable in the source manifest.

---

### 4.1 `SUBSCRIBE`

The payload is:

| Offset | Size | Field                    |
| -----: | ---: | ------------------------ |
|      0 |    4 | `target_source_id`       |
|      4 |    4 | `target_boot_id`         |
|      8 |    2 | `topic_id`               |
|     10 |    2 | `flags`                  |
|     12 |    4 | `requested_rate_millihz` |
|     16 |    4 | `requested_lease_ms`     |

The following fields must be non-zero:

```text
target_source_id
target_boot_id
topic_id
requested_rate_millihz
requested_lease_ms
```

`flags` is zero in version 2.

---

### 4.2 `SUBSCRIBE_RESULT`

The result contains:

```text
request reference

status:uint8
reserved:uint8
error_code:uint16_le

subscription_id:uint32_le
effective_rate_millihz:uint32_le
granted_lease_ms:uint32_le
```

On success:

```text
subscription_id != 0
effective_rate_millihz != 0
granted_lease_ms != 0
```

On failure these values are zero.

The effective publication rate does not exceed:

```text
requested_rate_millihz
```

or the topic's:

```text
max_rate_millihz
```

---

### 4.3 Subscription identity

An accepted subscription is identified by:

```text
subscription_id
```

A subscription remains valid only for the granted lease period.

Renewal uses another `SUBSCRIBE` request.

A retransmission of the same request identity does not create a second subscription.

---

### 4.4 `UNSUBSCRIBE`

`UNSUBSCRIBE` contains:

```text
target_source_id:uint32_le
target_boot_id:uint32_le
subscription_id:uint32_le
```

All values must be non-zero.

The corresponding result uses the common request-reference and status model.

Removing an already absent subscription is treated as successful.

---

## 5. Status

`STATUS` is a spontaneous `CONTROL` message containing protocol counters.

It does not have a corresponding result message.

The counters are scoped to the source envelope:

```text
(source_id, boot_id)
```

---

### 5.1 Status version 1

The logical payload is:

| Offset | Size | Field                  |
| -----: | ---: | ---------------------- |
|      0 |    2 | `status_version`       |
|      2 |    2 | `flags`                |
|      4 |    8 | `uptime_us`            |
|     12 |    8 | `frames_rx`            |
|     20 |    8 | `frames_tx`            |
|     28 |    8 | `frames_dropped`       |
|     36 |    8 | `crc_errors`           |
|     44 |    8 | `decode_errors`        |
|     52 |    8 | `reassembly_completed` |
|     60 |    8 | `reassembly_timeouts`  |
|     68 |    8 | `reassembly_rejected`  |
|     76 |    8 | `command_duplicates`   |
|     84 |    8 | `telemetry_dropped`    |

The complete version-1 payload is:

```text
92 octets
```

`status_version` is:

```text
1
```

Status flags currently define:

```text
bit 0 -> DEGRADED
```

All counters are `uint64_le`.

Counters are monotonic within one source boot and saturate at `UINT64_MAX`.

---

### 5.2 Status version 2

Status version 2 preserves the complete 92-octet version-1 prefix and appends per-topic information.

At offset 92:

```text
topic_status_count:uint16_le
```

followed by:

```text
topic_status_count
```

records of 28 octets each.

Each topic status record contains:

| Offset | Size | Field                    |
| -----: | ---: | ------------------------ |
|      0 |    4 | `source_id`              |
|      4 |    2 | `topic_id`               |
|      6 |    2 | `subscriber_count`       |
|      8 |    4 | `effective_rate_millihz` |
|     12 |    8 | `bytes_total`            |
|     20 |    8 | `samples_dropped_total`  |

Within one status message, the pair:

```text
(source_id, topic_id)
```

must not repeat.

---

## 6. Limits

The following limits apply to the structures defined in this chapter:

| Limit                                   |        Value |
| --------------------------------------- | -----------: |
| Logical manifest                        | 49152 octets |
| Individual `utf8_u16` text              |  1024 octets |
| Name or unit                            |   128 octets |
| Result message                          |   512 octets |
| Action parameters or result             | 32768 octets |
| Fields per topic or action side         |          256 |
| Sources per catalog                     |         1024 |
| Topics per manifest                     |         1024 |
| Actions per manifest                    |         1024 |
| Enum or action-error entries per record |          256 |
| Source info entries                     |           32 |
| Source info `key`                       |    64 octets |
| Source info `label` or `value`          |   256 octets |

The effective limit is always the smallest applicable limit imposed by:

```text
protocol
session
descriptor
transport path
```

A logical message larger than the payload available in one physical BTP frame uses the normal fragmentation mechanism described in [Getting it across the link](fragmentation-and-transports.md).

There is no second fragmentation mechanism inside commands or manifests.

---

## 7. Validation

A receiver validates the complete logical payload before applying the operation represented by it.

The general order is:

1. validate the BTP envelope;
2. complete reassembly when fragmented;
3. resolve `type` and `object_id`;
4. validate the fixed-size portion of the payload;
5. validate reserved fields and flags;
6. validate every declared length before consuming variable data;
7. validate identifiers, versions and counts;
8. validate nested records within their declared bounds;
9. validate all applicable protocol limits;
10. require exact consumption of the logical payload;
11. only then process the operation.

A malformed logical payload is rejected as a whole.

The decoder does not infer missing fields, ignore unexpected trailing bytes or continue parsing beyond a declared boundary.

---

## 8. Summary

BTP defines command and discovery operations through `COMMAND` and `CONTROL` messages.

Commands use:

```text
COMMAND_REQUEST
        |
        v
COMMAND_RESULT
```

and include:

* explicit target source and boot identity;
* versioned action identifiers;
* request correlation;
* bounded deduplication.

Runtime discovery uses:

```text
MANIFEST_REQUEST
        |
        v
MANIFEST_DATA
```

to describe:

* sources;
* telemetry topics;
* schemas;
* fields;
* actions;
* parameters;
* results;
* action-specific errors.

Subscriptions control periodic telemetry publication, while status messages expose protocol counters.

All of these mechanisms use the same BTP logical-message and fragmentation rules defined by the rest of the protocol.
