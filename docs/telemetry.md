# Telemetry payloads

This chapter specifies the logical payload of messages whose envelope `type` is
`TELEMETRY`. The envelope, CRC, fragmentation and transport limits are
unchanged and are covered in [The datagram](frame.md).

Everything below describes the payload **after** reassembly. A consumer never
decodes an isolated fragment.

## 1. Topic and schema identity

In a `TELEMETRY` message, the envelope's `object_id` *is* the topic id, and it
must be non-zero. A topic is identified by a pair:

```text
(source_id, topic_id)
```

`topic_id` is local to a source's namespace. Two different sources may use the
same number for unrelated topics. The pair stays stable across boots as long as
it means the same thing.

The schema used to decode a sample is identified by a triple:

```text
(source_id, topic_id, schema_version)
```

`schema_version` is a `uint16_le` between 1 and 65535; zero is invalid. It is
monotonic within a topic and a version is **never** reused for a different
layout or a different interpretation. After 65535 a source that needs another
schema allocates a new `topic_id` — the version does not wrap.

## 2. Payload shape

Every `TELEMETRY` payload has the same two-part shape, whatever the encoding:

| Offset | Size | Field | Wire type |
| ---: | ---: | --- | --- |
| 0 | 2 | `schema_version` | `uint16_le` |
| 2 | variable | `encoded_body` | per the encoding the schema declares |

The schema definition itself — names, types, units, metadata — is **not**
repeated in every sample. That is the whole point: the version selects a schema
announced separately, through the [manifest](commands.md#3-the-manifest).

In a fragmented message this structure belongs to the *logical* payload. The
version appears once, at the start of the reassembled result, not once per
fragment.

## 3. The schema model

A schema declares at minimum its `(source_id, topic_id, schema_version)`, a
stable human-readable name, the body `encoding`, and — for structured
encodings — an ordered list of fields.

Each structured field declares:

| Property | Rule |
| --- | --- |
| `field_id` | Non-zero `uint16`, unique, stable, never reused within the topic |
| `name` | Human-readable; **not** an identity on the wire |
| `order` | Unique index, contiguous, starting at zero |
| `type` | One of the types in section 5 |
| `unit` | Stable engineering unit symbol; `1` means dimensionless |
| `scale` | Finite multiplier; default `1` |
| `offset` | Finite offset; default `0` |
| `element_count` | `1`, a fixed count from 1 to 65535, or `variable` |
| `max_element_count` | Required when the count is variable |
| `nullable` | Boolean; default `false` |

The conversion from a raw wire value to an engineering value is:

```text
engineering_value = raw * scale + offset
```

`scale` and `offset` are schema metadata, not sample bytes. They are not
applied to `bool`, and for enums they never change which label is selected —
that always uses the raw integer.

In an array, `nullable` refers to the whole field, not to individual elements;
elements inside an array cannot be individually null.

Changing a type, order, count, unit, scale, offset, nullability, encoding or
meaning requires a new `schema_version`. Changing only a presentation label that
alters neither identity nor interpretation does not.

## 4. Encodings

| Value | Name | Use |
| ---: | --- | --- |
| `0x00` | `INVALID` | Reserved; never announced |
| `0x01` | `OPAQUE_BYTES` | Bytes whose meaning is external to BTP |
| `0x02` | `UTF8` | UTF-8 text with an explicit size |
| `0x03` | `JSON_UTF8` | One JSON value in UTF-8 |
| `0x04` | `CSV_UTF8` | One CSV record in UTF-8 |
| `0x05` | `PACKED_LE` | Compact fields in schema order |
| `0x06` | `TLV_LE` | Tag-length-value fields |
| `0x07`-`0xFF` | — | Reserved |

**`PACKED_LE` is the production encoding.** `CSV_UTF8` is for test and
diagnostics and must never be used as a fallback when a binary encoding fails.
A producer does not change encoding without publishing a new schema version.

### 4.1 `PACKED_LE`

The body starts with a presence bitmap **only if the schema has nullable
fields**. With `K` nullable fields the bitmap is `ceil(K / 8)` octets. Bit zero
of the first octet is the first nullable field in schema order; bits advance
from least to most significant, then to the next octet. `1` means present, `0`
means null. Unused bits in the last octet are zero.

After the bitmap, present fields appear exactly once each in increasing
`order`, with no padding and no alignment. Non-nullable fields always appear. A
null field costs nothing beyond its presence bit.

- A scalar occupies exactly its type's width.
- A fixed array holds exactly `element_count` consecutive elements and carries
  no count.
- A variable array starts with `element_count:uint16_le`, then that many
  consecutive elements, and the count must not exceed `max_element_count`.

The decoder knows the bounds from the schema and must consume the body exactly.
Truncation, an excessive count, or leftover bytes invalidate the sample.

### 4.2 `TLV_LE`

The body is zero or more entries:

```text
+----------------------+----------------------+----------------------+
| field_id:uint16_le   | value_size:uint16_le | value[value_size]    |
+----------------------+----------------------+----------------------+
0                      2                      4
```

Entries are in increasing `field_id` and an id never repeats. A null field is
represented by the absence of its entry; every non-nullable field must be
present. Variable arrays keep their `element_count:uint16_le` prefix inside the
value.

An unknown `field_id` must have its `value_size` validated so the parser can
skip it safely — but it makes the sample incompatible, and the sample is
rejected and counted rather than partially used.

`TLV_LE` allows sparse extension at a cost of four octets per field. It does
not replace an explicit version change when the meaning of an existing field
changes.

### 4.3 The text encodings

`OPAQUE_BYTES` is a byte sequence delimited only by its size. It may contain
any octet, `0x00` and CR and LF included. No nullability and no scaling.

`UTF8` is well-formed UTF-8, no BOM, no implicit terminator. `0x00` is the
character U+0000 and does not end the sample. Invalid UTF-8 invalidates it.

`JSON_UTF8` is exactly one top-level JSON array, one entry per field in schema
order, with nested arrays for array fields. A null field is JSON `null`.

`CSV_UTF8` is exactly one record, no header and no line terminator, fields
comma-separated with optional double quotes. Scalars only. A bare unquoted
`null` means a null field; an empty field is invalid.

## 5. Wire types

| Type | Size | Representation |
| --- | ---: | --- |
| `uint8` / `uint16` / `uint32` / `uint64` | 1 / 2 / 4 / 8 | Unsigned, little-endian |
| `int8` / `int16` / `int32` / `int64` | 1 / 2 / 4 / 8 | Two's complement, little-endian |
| `float32` / `float64` | 4 / 8 | IEEE-754 bit pattern, little-endian |
| `bool` | 1 | `0x00` false, `0x01` true; anything else invalid |
| `enum8` / `enum16` | 1 / 2 | Unsigned value, labels in the schema |

Floats are serialized as their IEEE-754 bit pattern in little-endian order —
not as text, and not as a memory cast.

## 6. Value policy

**Non-finite values are invalid.** NaN, positive or negative infinity, and any
non-finite result must not be emitted, and a sample containing one is rejected
with the reason reported. A missing measurement uses a nullable field, not NaN.
This is a deliberate choice: NaN-as-absent conflates "no reading" with
"arithmetic went wrong", and only one of those is a sensor state.

**An unknown enum value is recoverable.** It does not shift any offset, so the
sample stays structurally sound. The receiver keeps the raw integer, marks the
field as an unknown enum, assigns no label, and reports it. The rest of the
sample stays valid. This is the *only* recoverable payload error.

**Structural problems reject the whole sample.** A payload under two octets, a
truncated field, an oversized array count, an invalid bitmap, a TLV size
mismatch or unconsumed trailing bytes all reject the entire logical sample.
There is no padding, no implicit terminator, no default value for missing
bytes, and no partial decode.

**An unknown schema rejects the sample, not the frame.** If the version is
zero, unknown, or inconsistent with the announced schema, the consumer rejects
the sample without trying another version or guessing, delivers nothing
partial, logs at least the source, topic, received version and reason,
increments a rejection counter, and should request a catalog refresh. The frame
that carried it was structurally valid and is not a transport error.

## 7. Publishing and fragmentation

A frequent sample should fit in a single frame of the most restrictive
transport on its path. On ESP-NOW that means at most 210 octets of BTP payload,
two of which are `schema_version`, leaving 208 for the encoded body.

Larger samples use the ordinary fragmentation from
[Getting it across the link](fragmentation-and-transports.md). A consumer must
not decode, publish or plot an isolated fragment: a sample is publishable only
after complete reassembly and full validation of the logical payload. An
incomplete, expired or inconsistent message is discarded whole.

## 8. Client binding

A client identifies a scalar datum by:

```text
(source_id, topic_id, field_id)
```

and an array element by adding the index:

```text
(source_id, topic_id, field_id, element_index)
```

If an `element_index` does not exist in the current sample of a variable array,
that binding is temporarily without a value. The client must not reuse the
previous element and must not fabricate a zero.

`schema_version` selects how the current sample is decoded but does not replace
the field's stable identity. If a new version removes or incompatibly changes a
`field_id`, the binding becomes unavailable until it is reconfigured — it never
migrates by name or by position.

A binding contains no chart id, panel, color, axis or screen position. Several
charts may consume the same field. Labels, colors, axis limits, layout and
aggregation are client configuration and are not part of the telemetry payload
or the data schema.

## 9. Worked example

```text
source_id:      0x11223344
topic_id:       0x0101
schema_version: 1
name:           motor_state
encoding:       PACKED_LE
```

| order | field_id | name | type | unit | scale | element_count | nullable |
| ---: | ---: | --- | --- | --- | ---: | ---: | --- |
| 0 | 1 | `left_speed` | `float32` | `rad/s` | 1 | 1 | false |
| 1 | 2 | `right_speed` | `float32` | `rad/s` | 1 | 1 | false |
| 2 | 3 | `left_current` | `int16` | `A` | 0.01 | 1 | false |
| 3 | 4 | `right_current` | `int16` | `A` | 0.01 | 1 | false |

For speeds `1.5` and `-2.25 rad/s` and raw currents `300` and `-40`:

```text
01 00                      schema_version = 1
00 00 c0 3f                left_speed  = 1.5f
00 00 10 c0                right_speed = -2.25f
2c 01                      left_current  raw 300  -> 3.00 A
d8 ff                      right_current raw -40  -> -0.40 A
```

Continuous form:

```text
01 00 00 00 c0 3f 00 00 10 c0 2c 01 d8 ff
```

No nullable fields, so no bitmap. Twelve octets of values plus two of version
is fourteen. No names, no separators, no terminator, no padding. The `0x00`
octets inside the floats are ordinary data.

A second example makes the bitmap concrete: a topic with three fields where
only the last is nullable starts its body with one bitmap octet, whose bit 0 is
that field's presence and whose remaining seven bits are zero.

## 10. Validation order

After validating the envelope and completing any reassembly, a consumer:

1. requires at least two octets and reads `schema_version`;
2. resolves exactly `(source_id, topic_id, schema_version)`;
3. confirms it supports the announced encoding;
4. validates structure, lengths, counts, presence and types without reading
   outside the buffer;
5. applies the bool, float and enum policies;
6. only then converts scale and unit and publishes the sample.

Failure at any step discards the entire logical sample, except for the unknown
enum case in section 6.
