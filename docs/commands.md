# Commands and discovery

This chapter defines:

* command execution and results;
* manifest discovery;
* telemetry subscriptions;
* protocol status reporting.

Session establishment and terminal traffic are defined separately in [Session and terminal](session-and-terminal.md).

All layouts in this chapter describe the complete logical payload after reassembly.

There is no:

* alignment padding;
* implicit terminator;
* native structure representation.

All multi-octet integers use little-endian encoding.

---

## 1. Common data types

### 1.1 `utf8_u16`

`utf8_u16` represents a length-prefixed UTF-8 string.

```text
+----------------------+----------------------+
| size                 | UTF-8 data           |
| uint16_le            | size octets          |
+----------------------+----------------------+
```

`size` defines the number of UTF-8 octets that follow.

```text
size = 0
```

represents an empty string.

The string contains:

* no byte-order mark requirement;
* no implicit null terminator.

The decoder must validate `size` before reading the string.

---

### 1.2 `bytes_u32`

`bytes_u32` represents an arbitrary byte sequence.

```text
+----------------------+----------------------+
| size                 | data                 |
| uint32_le            | size octets          |
+----------------------+----------------------+
```

Any octet value is valid inside `data`.

The decoder must validate `size` against:

* the remaining logical payload;
* the protocol limits;
* the negotiated session limits, when applicable.

Length validation occurs before allocation, copying, or parsing of the contained data.

---

## 2. Request references

Responses identify the request that produced them using a request reference.

The layout is:

| Offset | Size | Field               | Wire type   |
| -----: | ---: | ------------------- | ----------- |
|      0 |    4 | `request_source_id` | `uint32_le` |
|      4 |    4 | `request_boot_id`   | `uint32_le` |
|      8 |    4 | `reply_to_sequence` | `uint32_le` |

These values correspond to the envelope identity of the original request:

```text
request_source_id = request.source_id
request_boot_id   = request.boot_id
reply_to_sequence = request.sequence
```

A sequence number alone is not sufficient to identify a request.

Request correlation therefore uses:

```text
(
    request_source_id,
    request_boot_id,
    reply_to_sequence
)
```

The response copies these values unchanged.

---

## 3. Result status

Operations that return an explicit result use the following status codes.

|  Value | Name          | Meaning                                       |
| -----: | ------------- | --------------------------------------------- |
| `0x00` | `SUCCESS`     | Operation completed successfully              |
| `0x01` | `REJECTED`    | Request was valid but refused                 |
| `0x02` | `FAILED`      | Execution started but failed                  |
| `0x03` | `TIMEOUT`     | Execution exceeded its permitted time         |
| `0x04` | `CANCELLED`   | Execution was cancelled                       |
| `0x05` | `UNSUPPORTED` | Requested feature or version is not supported |
| `0x06` | `BUSY`        | Required capacity is temporarily unavailable  |

Error codes provide additional information.

|             Value | Name                   |
| ----------------: | ---------------------- |
|          `0x0000` | `NONE`                 |
|          `0x0001` | `MALFORMED_PAYLOAD`    |
|          `0x0002` | `UNKNOWN_OBJECT`       |
|          `0x0003` | `INVALID_ARGUMENT`     |
|          `0x0004` | `NOT_AUTHORIZED`       |
|          `0x0005` | `CAPACITY_EXHAUSTED`   |
|          `0x0006` | `EXECUTION_TIMEOUT`    |
|          `0x0007` | `INTERNAL_ERROR`       |
|          `0x0008` | `UNSUPPORTED_VERSION`  |
|          `0x0009` | `STALE_TARGET_BOOT`    |
|          `0x000A` | `REQUEST_CONFLICT`     |
|          `0x000B` | `NOT_FOUND`            |
| `0x000C`–`0x7FFF` | Reserved to BTP        |
| `0x8000`–`0xFFFF` | Action-specific errors |

The following relationship is required:

```text
SUCCESS     -> error_code = NONE
non-SUCCESS -> error_code != NONE
```

Human-readable error messages may accompany these values.

Applications must use `status` and `error_code` for program logic. Diagnostic text is not a machine-readable error identifier.

---

## 4. Object identifiers

The message envelope uses `type` to select a logical channel and `object_id` to select an operation within that channel.

This chapter defines the following objects.

### `COMMAND`

| `object_id` | Name              |
| ----------: | ----------------- |
|    `0x0001` | `COMMAND_REQUEST` |
|    `0x0002` | `COMMAND_RESULT`  |

### `CONTROL`

| `object_id` | Name                 |
| ----------: | -------------------- |
|    `0x0003` | `MANIFEST_REQUEST`   |
|    `0x0004` | `MANIFEST_DATA`      |
|    `0x0005` | `SUBSCRIBE`          |
|    `0x0006` | `SUBSCRIBE_RESULT`   |
|    `0x0007` | `UNSUBSCRIBE`        |
|    `0x0008` | `UNSUBSCRIBE_RESULT` |
|    `0x0009` | `STATUS`             |

Other `CONTROL` identifiers are defined in [Session and terminal](session-and-terminal.md).

An unknown `object_id` is rejected.

The receiver does not infer another message type from the payload contents.

---

# 5. Commands

Commands use a request/result model.

```text
Requester                              Executor
    |                                     |
    |--------- COMMAND_REQUEST ---------->|
    |                                     |
    |          execute action             |
    |                                     |
    |<--------- COMMAND_RESULT -----------|
```

The request identifies:

* the target source;
* the target boot;
* the action;
* the action version;
* the encoded parameters.

The result identifies the original request and reports its final outcome.

---

## 5.1 `COMMAND_REQUEST`

The payload is:

| Offset |     Size | Field              | Wire type               |
| -----: | -------: | ------------------ | ----------------------- |
|      0 |        4 | `target_source_id` | `uint32_le`             |
|      4 |        4 | `target_boot_id`   | `uint32_le`             |
|      8 |        2 | `action_id`        | `uint16_le`             |
|     10 |        2 | `action_version`   | `uint16_le`             |
|     12 |        2 | `flags`            | `uint16_le`             |
|     14 |        2 | `reserved`         | `uint16_le`             |
|     16 |        4 | `parameter_size`   | `uint32_le`             |
|     20 | variable | `parameters`       | `parameter_size` octets |

The following fields must be non-zero:

```text
target_source_id
target_boot_id
action_id
action_version
```

The current command format requires:

```text
flags    = 0
reserved = 0
```

`parameter_size` may be zero.

The payload must contain exactly:

```text
20 + parameter_size
```

octets.

Trailing bytes are invalid.

---

## 5.2 Target boot

`target_boot_id` identifies the execution of the target device against which the command was created.

Before executing the command, the target verifies:

```text
target_source_id == local source_id
target_boot_id   == local boot_id
```

If the source is correct but the target has restarted since the command was created, the command is rejected with:

```text
REJECTED / STALE_TARGET_BOOT
```

This prevents an old command from being executed against a different runtime state after a restart.

For example:

```text
Server creates command
target_boot_id = 100
        |
        v
Device restarts
boot_id = 101
        |
        v
Old command arrives
        |
        v
REJECTED / STALE_TARGET_BOOT
```

---

## 5.3 Action definition

`action_id` identifies an action exposed by the target.

`action_version` identifies the exact definition of that action.

The target's manifest defines:

* action name;
* action version;
* parameter encoding;
* parameter fields;
* result encoding;
* result fields;
* execution timeout;
* action-specific error codes;
* action flags.

The command parameters must match the action descriptor identified by:

```text
(target_source_id, action_id, action_version)
```

Unknown versions or invalid parameter payloads are rejected.

The receiver must not attempt to decode parameters using another action version.

---

## 5.4 `COMMAND_RESULT`

The result envelope identifies the executor and uses a new sequence number generated by that executor.

The payload contains:

|   Offset |    Size | Field             | Wire type   |
| -------: | ------: | ----------------- | ----------- |
|        0 |      12 | request reference | Section 2   |
|       12 |       2 | `action_id`       | `uint16_le` |
|       14 |       2 | `action_version`  | `uint16_le` |
|       16 |       1 | `status`          | `uint8`     |
|       17 |       1 | `reserved`        | `uint8`     |
|       18 |       2 | `error_code`      | `uint16_le` |
|       20 | `2 + M` | `message`         | `utf8_u16`  |
| `22 + M` | `4 + R` | `result`          | `bytes_u32` |

`reserved` must be zero.

When the original request was parsed successfully:

```text
action_id
action_version
```

copy the corresponding request values.

If the request payload itself was malformed, these fields may be zero.

The `result` data uses the encoding defined by the action descriptor.

`result` is empty when:

* the action defines no output;
* `status` is not `SUCCESS`.

Exactly one final `COMMAND_RESULT` is associated with each accepted or rejected request.

Intermediate progress is not transmitted through additional command results.

Progress information belongs to telemetry or status messages.

---

## 5.5 Command retry

A requester retries a command by retransmitting the same logical request.

A retry preserves:

```text
source_id
boot_id
sequence
logical payload
```

Therefore, the request identity remains:

```text
(
    request_source_id,
    request_boot_id,
    request_sequence
)
```

Using a new sequence number creates a new logical command.

It is not a retry.

---

## 5.6 Deduplication

Command execution uses request deduplication to prevent the same logical command from producing the same side effect more than once.

The deduplication key is:

```text
(
    request_source_id,
    request_boot_id,
    request_sequence
)
```

The executor also compares the complete logical request payload.

### First request

When a new request identity is received, the executor reserves its deduplication entry before starting the action.

```text
new identity
    |
    v
reserve dedup entry
    |
    v
execute action
```

The entry must exist before any externally visible effect occurs.

### Identical retry

If the same identity is received with the same payload:

```text
same identity
same payload
```

the action is not executed again.

If execution is still in progress, the duplicate remains associated with the original execution.

If execution has completed, the executor retransmits the previously generated `COMMAND_RESULT`.

The retransmitted result is byte-identical to the original result and uses the same result sequence.

### Conflicting request

If the same request identity is received with different payload bytes:

```text
same identity
different payload
```

the request is not executed.

The executor returns:

```text
REJECTED / REQUEST_CONFLICT
```

### Deduplication capacity

Deduplication storage is bounded.

Entries protecting accepted requests are not removed to create space for new commands during the executor's current boot.

If the configured deduplication capacity is exhausted, new requests are rejected with:

```text
BUSY / CAPACITY_EXHAUSTED
```

Existing entries remain protected.

This preserves the duplicate-execution guarantee even under resource exhaustion.

### Deduplication lifetime

An accepted deduplication entry remains valid until the executor restarts.

The following events do not authorize removal:

* transport disconnection;
* session closure;
* command retransmission;
* requester restart;
* communication timeout.

When the executor restarts, the cache may be cleared.

Previously created commands contain the old `target_boot_id` and are therefore rejected as stale if received after that restart.

### Idempotent actions

The manifest may mark an action as:

```text
IDEMPOTENT
```

This indicates that repeated execution is naturally safe according to the action semantics.

Deduplication is still required.

The flag does not disable command deduplication.

---

# 6. Manifest discovery

The manifest describes the data and actions exposed by a BTP source.

It allows a consumer to discover at runtime:

* telemetry topics;
* telemetry schemas;
* field definitions;
* supported actions;
* action parameters;
* action results;
* application-specific errors;
* subscription capability.

This allows binary telemetry and commands to remain compact without requiring all structures to be hard-coded in the consumer.

```text
Consumer                             Source
   |                                   |
   |-------- MANIFEST_REQUEST -------->|
   |                                   |
   |<--------- MANIFEST_DATA ----------|
   |                                   |
   |     topics and actions known       |
```

A manifest describes exactly one source.

A gateway may return manifest information for another source from its catalog.

---

## 6.1 `MANIFEST_REQUEST`

The payload is:

| Offset | Size | Field                   | Wire type   |
| -----: | ---: | ----------------------- | ----------- |
|      0 |    4 | `target_source_id`      | `uint32_le` |
|      4 |    4 | `target_boot_id`        | `uint32_le` |
|      8 |    4 | `known_config_revision` | `uint32_le` |

The request has two modes:

* targeted source request;
* complete catalog request.

---

## 6.2 Targeted manifest request

When:

```text
target_source_id != 0
```

the request asks for one source.

`target_boot_id` may be:

```text
0
```

to accept the source's current boot, or a specific non-zero boot identifier.

If the requested source does not exist:

```text
REJECTED / NOT_FOUND
```

is returned.

If a non-zero `target_boot_id` does not match the current boot:

```text
REJECTED / STALE_TARGET_BOOT
```

is returned.

---

## 6.3 Configuration revision

`known_config_revision` allows the consumer to avoid downloading an unchanged manifest.

```text
known_config_revision = 0
```

forces a complete manifest response.

If the value matches the current source configuration revision, the source may return a successful `MANIFEST_DATA` with:

```text
NOT_MODIFIED = 1
```

and omit the topic and action descriptors.

This supports the following flow:

```text
first connection
      |
      v
request full manifest
      |
      v
cache config_revision = 17
      |
      v
later connection
      |
      v
MANIFEST_REQUEST
known_config_revision = 17
      |
      +---- unchanged ---> NOT_MODIFIED
      |
      +---- changed -----> new manifest
```

---

## 6.4 Catalog request

A complete catalog is requested with:

```text
target_source_id      = 0
target_boot_id        = 0
known_config_revision = 0
```

The responder creates a snapshot of the sources it currently knows.

The snapshot includes the responder itself.

Sources are ordered by:

```text
source_id
```

One complete `MANIFEST_DATA` message is generated for each source.

The number of responses is given by:

```text
catalog_count
```

and is at least one.

The snapshot does not change while the response set is being generated.

Sources added or removed during enumeration are reflected only in a later manifest request.

---

# 7. `MANIFEST_DATA`

The payload begins with:

|   Offset |     Size | Field                     | Wire type              |
| -------: | -------: | ------------------------- | ---------------------- |
|        0 |       12 | request reference         | Section 2              |
|       12 |        1 | `status`                  | `uint8`                |
|       13 |        1 | `flags`                   | `uint8`                |
|       14 |        2 | `error_code`              | `uint16_le`            |
|       16 |        2 | `manifest_format_version` | `uint16_le`            |
|       18 |        2 | `reserved`                | `uint16_le`            |
|       20 |        4 | `config_revision`         | `uint32_le`            |
|       24 |       16 | `source_uuid`             | 16 octets              |
|       40 |        4 | `described_source_id`     | `uint32_le`            |
|       44 |        4 | `described_boot_id`       | `uint32_le`            |
|       48 |        1 | `source_role`             | `uint8`                |
|       49 |        1 | `source_flags`            | `uint8`                |
|       50 |        2 | `catalog_index`           | `uint16_le`            |
|       52 |        2 | `catalog_count`           | `uint16_le`            |
|       54 |        2 | `topic_count`             | `uint16_le`            |
|       56 |        2 | `action_count`            | `uint16_le`            |
|       58 | variable | `source_name`             | `utf8_u16`             |
| variable | variable | topics                    | `topic_count` records  |
| variable | variable | actions                   | `action_count` records |

The current manifest format is:

```text
manifest_format_version = 1
```

`reserved` must be zero.

The defined manifest flags are:

| Bit | Name               |
| --: | ------------------ |
|   0 | `NOT_MODIFIED`     |
|   1 | `CATALOG_COMPLETE` |

All other bits are reserved and must be zero.

The defined source flag is:

| Bit | Name     |
| --: | -------- |
|   0 | `ONLINE` |

`ONLINE` indicates that the responder currently has an active session with the described source.

A cached source may therefore appear in a catalog with `ONLINE` clear.

---

## 7.1 Source identity

The BTP envelope identifies the source that transmitted the `MANIFEST_DATA`.

The manifest itself identifies the source being described using:

```text
described_source_id
described_boot_id
source_uuid
```

These may differ from the envelope source when a gateway responds using cached information.

The gateway preserves the identity of the source being described.

---

## 7.2 Targeted response

For a targeted request:

```text
catalog_index = 0
catalog_count = 1
CATALOG_COMPLETE = 1
```

The response describes exactly one source.

---

## 7.3 Catalog response

For a catalog request, each response contains:

```text
catalog_index = 0 .. catalog_count - 1
```

Every response uses the same:

```text
catalog_count
```

Only the final entry sets:

```text
CATALOG_COMPLETE = 1
```

The consumer publishes the new catalog only after receiving a complete and consistent snapshot.

A missing or duplicated catalog entry makes the snapshot incomplete.

The consumer may discard the incomplete snapshot and issue another manifest request.

---

## 7.4 `NOT_MODIFIED`

When `NOT_MODIFIED` is set:

```text
status = SUCCESS
```

Source identity and source name remain present.

The descriptor counts are:

```text
topic_count  = 0
action_count = 0
```

No topic or action records follow.

---

# 8. Manifest records

Topic, field, and action descriptors are encoded as length-delimited records.

Every record begins with:

```text
record_size : uint32_le
```

`record_size` counts the number of octets following the size field.

```text
+----------------------+-----------------------------+
| record_size          | record body                 |
| uint32_le            | record_size octets          |
+----------------------+-----------------------------+
```

A decoder must:

1. validate `record_size` against the remaining payload;
2. restrict parsing to that record;
3. consume exactly `record_size` octets.

Trailing unrecognized bytes are not ignored.

A malformed record invalidates the complete manifest.

---

# 9. Topic records

A topic record describes one telemetry topic.

It contains, in order:

| Field              | Wire type                   |
| ------------------ | --------------------------- |
| `record_size`      | `uint32_le`                 |
| `topic_id`         | `uint16_le`                 |
| `schema_version`   | `uint16_le`                 |
| `encoding`         | `uint8`                     |
| `flags`            | `uint8`                     |
| `field_count`      | `uint16_le`                 |
| `max_rate_millihz` | `uint32_le`                 |
| `name`             | `utf8_u16`                  |
| `description`      | `utf8_u16`                  |
| fields             | `field_count` field records |

The following values must be non-zero:

```text
topic_id
schema_version
```

The defined topic flag is:

| Bit | Name           |
| --: | -------------- |
|   0 | `SUBSCRIBABLE` |

All remaining flag bits are reserved.

`max_rate_millihz` defines the maximum periodic publication rate.

```text
max_rate_millihz = 0
```

means that the topic is not periodically published.

For example:

```text
1000 millihz = 1 Hz
10000 millihz = 10 Hz
```

Topic encodings are defined by the telemetry specification.

---

# 10. Field records

A field record describes one field of a structured telemetry topic or action schema.

It contains:

| Field               | Wire type                 |
| ------------------- | ------------------------- |
| `record_size`       | `uint32_le`               |
| `field_id`          | `uint16_le`               |
| `order`             | `uint16_le`               |
| `type`              | `uint8`                   |
| `flags`             | `uint8`                   |
| `element_count`     | `uint16_le`               |
| `max_element_count` | `uint16_le`               |
| `scale`             | IEEE-754 `float64_le`     |
| `offset`            | IEEE-754 `float64_le`     |
| `enum_count`        | `uint16_le`               |
| `name`              | `utf8_u16`                |
| `unit`              | `utf8_u16`                |
| `description`       | `utf8_u16`                |
| enums               | `enum_count` enum records |

`scale` and `offset` must be finite.

The defined field flags are:

| Bit | Name             |
| --: | ---------------- |
|   0 | `NULLABLE`       |
|   1 | `VARIABLE_COUNT` |

All other bits are reserved.

The field type codes are:

|  Value | Type           |
| -----: | -------------- |
| `0x01` | `uint8`        |
| `0x02` | `uint16`       |
| `0x03` | `uint32`       |
| `0x04` | `uint64`       |
| `0x05` | `int8`         |
| `0x06` | `int16`        |
| `0x07` | `int32`        |
| `0x08` | `int64`        |
| `0x09` | `float32`      |
| `0x0A` | `float64`      |
| `0x0B` | `bool`         |
| `0x0C` | `enum8`        |
| `0x0D` | `enum16`       |
| `0x0E` | `opaque_bytes` |
| `0x0F` | `utf8`         |

`0x00` and higher unassigned values are reserved.

`opaque_bytes` and `utf8` are descriptor-only field types used for the optional logical field of whole-body `OPAQUE_BYTES` or `UTF8` telemetry topics.

They are not valid fields inside:

* structured telemetry;
* action parameters;
* action results.

The complete field and array rules are defined in [Telemetry payloads](telemetry.md).

---

## 10.1 Enum records

Enumeration entries contain:

| Field   | Wire type   |
| ------- | ----------- |
| `value` | `uint16_le` |
| `label` | `utf8_u16`  |

The number of entries is defined by:

```text
enum_count
```

Labels are descriptive metadata.

The numeric value remains the wire identity of the enum entry.

---

# 11. Action records

An action record describes one command exposed by a source.

It contains:

| Field                   | Wire type            |
| ----------------------- | -------------------- |
| `record_size`           | `uint32_le`          |
| `action_id`             | `uint16_le`          |
| `action_version`        | `uint16_le`          |
| `flags`                 | `uint16_le`          |
| `parameter_encoding`    | `uint8`              |
| `result_encoding`       | `uint8`              |
| `parameter_field_count` | `uint16_le`          |
| `result_field_count`    | `uint16_le`          |
| `execution_timeout_ms`  | `uint32_le`          |
| `name`                  | `utf8_u16`           |
| `description`           | `utf8_u16`           |
| `confirmation_text`     | `utf8_u16`           |
| parameter fields        | field records        |
| result fields           | field records        |
| `error_count`           | `uint16_le`          |
| errors                  | action-error records |

The following values must be non-zero:

```text
action_id
action_version
execution_timeout_ms
```

Parameter and result field records are ordered by increasing `order`.

---

## 11.1 Action flags

The defined action flags are:

| Bit | Name         |
| --: | ------------ |
|   0 | `IDEMPOTENT` |
|   1 | `DANGEROUS`  |

All other bits are reserved.

### `IDEMPOTENT`

The action can naturally be executed more than once without producing additional side effects.

This does not disable BTP command deduplication.

### `DANGEROUS`

The action requires explicit user confirmation before execution.

When `DANGEROUS` is set:

```text
confirmation_text
```

must not be empty.

The confirmation text is provided by the source.

A client must not infer or invent different semantics for the action.

---

## 11.2 Action encodings

Action parameters and results support:

|  Value | Encoding    |
| -----: | ----------- |
| `0x00` | `EMPTY`     |
| `0x05` | `PACKED_LE` |
| `0x06` | `TLV_LE`    |

`EMPTY` requires:

```text
field_count = 0
```

`PACKED_LE` and `TLV_LE` use the same field representation defined for telemetry.

Unlike telemetry payloads, action parameters and results do not contain a `schema_version` prefix.

Their structure is selected directly by:

```text
action_id
action_version
```

---

## 11.3 Action-specific errors

Action records may define application-specific error codes.

Each entry contains:

| Field        | Wire type   |
| ------------ | ----------- |
| `error_code` | `uint16_le` |
| `label`      | `utf8_u16`  |

Action-specific errors use:

```text
0x8000 .. 0xFFFF
```

The range below `0x8000` is reserved for protocol-defined errors.

---

# 12. Manifest versioning

Identifiers and versions are stable after publication.

A change that modifies how a telemetry topic is interpreted requires:

* a new `schema_version`;
* a new `config_revision`.

A change that modifies an action's:

* parameters;
* result structure;
* execution semantics;
* action-specific errors;

requires:

* a new `action_version`;
* a new `config_revision`.

A `config_revision` value must not be reused for different manifest contents.

A consumer may cache descriptors associated with a known revision.

If a new revision is detected, the consumer updates its manifest information before interpreting previously unknown schema or action versions.

It must not guess compatibility from an older descriptor.

---

# 13. Subscriptions

Subscriptions request periodic publication of telemetry topics.

```text
Consumer                               Source
   |                                     |
   |----------- SUBSCRIBE -------------->|
   |                                     |
   |<------ SUBSCRIBE_RESULT ------------|
   |                                     |
   |<---------- TELEMETRY ---------------|
   |<---------- TELEMETRY ---------------|
   |<---------- TELEMETRY ---------------|
```

A subscription controls publication rate and lifetime.

It does not change telemetry delivery semantics.

Telemetry remains best-effort.

---

## 13.1 `SUBSCRIBE`

The payload is:

| Offset | Size | Field                    | Wire type   |
| -----: | ---: | ------------------------ | ----------- |
|      0 |    4 | `target_source_id`       | `uint32_le` |
|      4 |    4 | `target_boot_id`         | `uint32_le` |
|      8 |    2 | `topic_id`               | `uint16_le` |
|     10 |    2 | `flags`                  | `uint16_le` |
|     12 |    4 | `requested_rate_millihz` | `uint32_le` |
|     16 |    4 | `requested_lease_ms`     | `uint32_le` |

The following values must be non-zero:

```text
target_source_id
target_boot_id
topic_id
requested_rate_millihz
requested_lease_ms
```

The current format requires:

```text
flags = 0
```

The topic must be marked:

```text
SUBSCRIBABLE
```

in the manifest.

---

## 13.2 `SUBSCRIBE_RESULT`

The result contains:

| Offset | Size | Field                    | Wire type   |
| -----: | ---: | ------------------------ | ----------- |
|      0 |   12 | request reference        | Section 2   |
|     12 |    1 | `status`                 | `uint8`     |
|     13 |    1 | `reserved`               | `uint8`     |
|     14 |    2 | `error_code`             | `uint16_le` |
|     16 |    4 | `subscription_id`        | `uint32_le` |
|     20 |    4 | `effective_rate_millihz` | `uint32_le` |
|     24 |    4 | `granted_lease_ms`       | `uint32_le` |

`reserved` must be zero.

On success:

```text
subscription_id        != 0
effective_rate_millihz != 0
granted_lease_ms       != 0
```

On failure, all three values are zero.

The effective publication rate must not exceed:

```text
requested_rate_millihz
```

or the topic's:

```text
max_rate_millihz
```

Therefore:

```text
effective_rate <= requested_rate
effective_rate <= manifest maximum
```

The source may grant a lower rate.

---

## 13.3 Delivery behavior

A subscription defines the requested publication schedule.

It does not guarantee exact sample timing.

Publication may contain:

* scheduling jitter;
* transport delay;
* lost telemetry messages.

There is no per-sample acknowledgement.

A subscription therefore does not convert telemetry into a reliable stream.

---

## 13.4 Subscription replacement

Retransmitting the same logical `SUBSCRIBE` request returns the same subscription rather than creating another one.

A new request identity for the same session and topic creates or replaces the corresponding subscription atomically.

This allows subscription parameters to be updated without accumulating duplicate subscriptions.

---

## 13.5 Lease expiration

Every subscription has a finite lease.

The granted lifetime is returned as:

```text
granted_lease_ms
```

The subscription expires when its lease ends.

The consumer renews the subscription by issuing another `SUBSCRIBE`.

---

# 14. `UNSUBSCRIBE`

`UNSUBSCRIBE` removes an existing subscription.

Its payload contains:

| Offset | Size | Field              | Wire type   |
| -----: | ---: | ------------------ | ----------- |
|      0 |    4 | `target_source_id` | `uint32_le` |
|      4 |    4 | `target_boot_id`   | `uint32_le` |
|      8 |    4 | `subscription_id`  | `uint32_le` |

Removing an existing subscription returns success.

Removing a subscription that is already absent also returns:

```text
SUCCESS / NONE
```

This makes `UNSUBSCRIBE` safe to retry.

---

# 15. Status

`STATUS` reports protocol and communication counters.

It is sent spontaneously and does not receive a response.

The message envelope identifies the reporting source.

Counters are scoped by:

```text
(source_id, boot_id)
```

They reset only when the source starts a new boot.

---

## 15.1 Status version 1

The fixed payload is:

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

`status_version` is:

```text
1
```

All counters are `uint64_le`.

The complete version-1 payload is:

```text
92 octets
```

The defined status flag is:

| Bit | Name       |
| --: | ---------- |
|   0 | `DEGRADED` |

All other bits are reserved.

---

## 15.2 Counter behavior

Counters are monotonic within one boot.

They saturate at:

```text
UINT64_MAX
```

and do not wrap.

They reset only when `boot_id` changes.

A transport reconnection or session restart does not reset the counters.

This allows communication problems to remain observable across transient reconnects.

### `frames_dropped`

Counts valid frames discarded because of queue or resource-capacity limits.

### `crc_errors`

Counts frames rejected because their CRC was invalid.

### `decode_errors`

Counts malformed envelopes or logical payloads.

### `reassembly_completed`

Counts fragmented logical messages successfully reconstructed.

### `reassembly_timeouts`

Counts incomplete reassemblies removed because their timeout expired.

### `reassembly_rejected`

Counts fragmented messages rejected because of invalid or conflicting fragment state.

### `command_duplicates`

Counts duplicate command requests detected by command deduplication.

### `telemetry_dropped`

Counts telemetry messages discarded by the implementation.

One event may contribute to different counters when those counters represent different aspects of the same event.

The same event must not increment one counter more than once.

---

# 16. Status version 2

`status_version = 2` extends the version-1 payload with optional per-topic statistics.

The first 92 octets are identical to version 1.

The extension is:

| Offset |     Size | Field                |
| -----: | -------: | -------------------- |
|     92 |        2 | `topic_status_count` |
|     94 | `28 × T` | topic status records |

where:

```text
T = topic_status_count
```

The complete size is therefore:

```text
94 + 28 * topic_status_count
```

---

## 16.1 Topic status record

Each topic status record has a fixed size of 28 octets.

| Offset | Size | Field                    | Wire type   |
| -----: | ---: | ------------------------ | ----------- |
|      0 |    4 | `source_id`              | `uint32_le` |
|      4 |    2 | `topic_id`               | `uint16_le` |
|      6 |    2 | `subscriber_count`       | `uint16_le` |
|      8 |    4 | `effective_rate_millihz` | `uint32_le` |
|     12 |    8 | `bytes_total`            | `uint64_le` |
|     20 |    8 | `samples_dropped_total`  | `uint64_le` |

`source_id` and `topic_id` must be non-zero.

```text
effective_rate_millihz = 0
```

means the topic is not currently being published periodically.

`source_id` is included because `topic_id` is only unique within one source.

A gateway may therefore report:

```text
(source A, topic 1)
(source B, topic 1)
```

as two independent topic status records.

The pair:

```text
(source_id, topic_id)
```

must be unique within one `STATUS` message.

Record order has no semantic meaning.

A receiver that supports status version 2 may ignore the per-topic extension if those metrics are not required.

---

# 17. Limits

The protocol defines the following limits.

| Limit                                       |       Maximum |
| ------------------------------------------- | ------------: |
| Logical payload on a path including ESP-NOW | 53,550 octets |
| Logical manifest                            | 49,152 octets |
| Individual `utf8_u16` string                |  1,024 octets |
| Name or unit                                |    128 octets |
| Result diagnostic message                   |    512 octets |
| Action parameters                           | 32,768 octets |
| Action result                               | 32,768 octets |
| Fields per topic                            |           256 |
| Fields per action side                      |           256 |
| Sources per catalog                         |         1,024 |
| Topics per manifest                         |         1,024 |
| Actions per manifest                        |         1,024 |
| Enum entries per record                     |           256 |
| Action-specific errors per record           |           256 |

The effective limit is the minimum imposed by:

```text
protocol
transport path
session negotiation
manifest descriptor
implementation capacity
```

A producer must not advertise a data structure whose declared maximum size exceeds the limits available on the negotiated communication path.

Large manifests and command results use the normal BTP fragmentation mechanism.

There is no separate fragmentation format inside these messages.

---

# 18. Validation order

After the BTP envelope has been validated and any required fragments have been reassembled, the receiver processes messages from this chapter in the following order:

1. resolve `type` and `object_id`;
2. validate the minimum fixed payload size;
3. validate reserved fields and flags;
4. validate every length before reading the referenced data;
5. validate identifiers and versions;
6. validate status and error-code combinations;
7. validate record counts and protocol limits;
8. validate nested records within their declared boundaries;
9. require exact consumption of the logical payload;
10. only then perform application-visible state changes.

State changes include:

* executing an action;
* replacing a subscription;
* removing a subscription;
* publishing a manifest catalog.

Malformed or incomplete logical messages do not produce partial application state.

For fragmented messages, application processing starts only after successful reassembly.

---

# 19. Summary

The `COMMAND` channel provides request/result command execution with explicit target identification and mandatory deduplication.

```text
COMMAND_REQUEST
      |
      v
target + action + parameters
      |
      v
deduplicated execution
      |
      v
COMMAND_RESULT
```

The manifest provides runtime discovery of the structures exposed by each source.

```text
MANIFEST_REQUEST
      |
      v
MANIFEST_DATA
      |
      +---- telemetry topics and schemas
      |
      +---- actions and parameter schemas
```

Subscriptions control periodic telemetry publication without changing telemetry's best-effort delivery semantics.

```text
SUBSCRIBE
    |
    v
subscription + lease + effective rate
    |
    v
TELEMETRY
```

`STATUS` provides boot-scoped counters for communication, reassembly, command, and telemetry behavior.

Together, these mechanisms allow a consumer to discover a device at runtime, decode its telemetry, execute its supported actions, configure telemetry publication, and monitor protocol health without requiring device-specific wire structures to be compiled into the consumer.
