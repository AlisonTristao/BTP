# Telemetry payloads

BTP telemetry is designed for channels where measurements may be transmitted continuously, sometimes at medium or high rates, and where bandwidth cannot always be treated as abundant.

A telemetry producer may expose many measurements:

```text
temperature
pressure
speed
current
voltage
vibration
...
```

A straightforward approach would be to serialize every sample as a self-describing text object:

```json
{
  "temperature": 32.4,
  "pressure": 101.2,
  "speed": 1450
}
```

This is convenient, but it also means transmitting field names, separators and textual representations again and again even when the structure of the data does not change.

For a low-rate application over Ethernet, that overhead may be irrelevant.

For an embedded device transmitting continuously over a constrained RF link, it may not be.

BTP therefore separates two things:

```text
what the data means
        |
        v
      schema


current measured values
        |
        v
    TELEMETRY
```

The schema describes the structure once.

Telemetry messages then carry primarily the values.

This chapter specifies the logical payload of messages whose envelope `type` is `TELEMETRY`. The envelope, CRC, fragmentation and transport limits are unchanged and are covered in [The datagram](frame.md).

Everything below describes the payload **after reassembly**. A consumer never decodes an isolated fragment.

The reference library draws a line through this chapter: `btp::messages` decodes the manifest `FieldRecord`s that *describe* a schema (type, unit, scale, offset, enum labels), but not a `TELEMETRY` sample against that schema -- the `PACKED_LE` / `TLV_LE` body codec, the nullable bitmap and the engineering conversion are not in the library ([Using the library §11.3](library.md#113-telemetry-schema-interpretation-is-above-the-payload-layer)).

---

## 1. Why telemetry needs a schema

Compact binary telemetry creates a fundamental problem.

If a producer transmits only:

```text
42 10 00 00 80 3f 2c 01
```

the receiver cannot determine by looking at those bytes whether they mean:

```text
uint16
float32
int16
```

or:

```text
four uint16 values
```

or something completely different.

The interpretation must come from somewhere.

One possible solution would be to define the same structure manually in both applications:

```cpp
struct Telemetry {
    float temperature;
    float pressure;
    uint16_t speed;
};
```

The producer and consumer could then agree beforehand that every message follows that structure.

BTP deliberately does not use this model.

A native structure introduces several problems:

* both applications must be compiled with prior knowledge of the layout;
* changing the layout requires coordinated changes;
* padding and alignment may differ;
* compiler and ABI behavior may differ;
* integer representation and endianness must still be defined;
* directly transmitting structure memory creates platform-dependent wire data.

A wire protocol should not depend on a compiler's in-memory representation.

BTP instead explicitly defines the serialized representation and gives that representation a versioned **schema**.

The producer may therefore describe:

```text
topic: motor_state
schema_version: 1

field 1:
    name: speed
    type: float32
    unit: rad/s

field 2:
    name: current
    type: int16
    unit: A
    scale: 0.01
```

and subsequently transmit only something conceptually equivalent to:

```text
schema_version
speed
current
```

The receiver does not need the producer's C or C++ structure.

It needs the BTP schema.

This gives BTP both properties:

```text
             self-description
                   +
             compact samples
```

without requiring the description to be repeated in every telemetry message.

---

## 2. Description once, values many times

Telemetry schemas normally change much less frequently than telemetry values.

Consider a sensor transmitting at 100 samples per second.

The following information may remain constant for hours, days or the entire firmware version:

```text
field name
field type
engineering unit
scale
offset
field order
array size
```

There is little value in transmitting that information 100 times every second.

BTP therefore keeps schema metadata outside the normal telemetry stream.

Conceptually:

```text
                     once / when changed
                            |
                            v
                    +----------------+
                    |    MANIFEST    |
                    |                |
                    | temperature    |
                    | float32        |
                    | degC           |
                    |                |
                    | pressure       |
                    | float32        |
                    | kPa            |
                    +----------------+
                            |
                            |
              +-------------+-------------+
              |             |             |
              v             v             v

         TELEMETRY      TELEMETRY      TELEMETRY
            32.4           32.5           32.6
           101.2          101.1          101.3
```

The description is transmitted when required.

The values can then be transmitted repeatedly in a compact representation.

This distinction becomes increasingly useful as:

* the number of fields grows;
* the publication rate increases;
* the transport payload becomes smaller;
* several devices share the same communication channel.

---

## 3. Discovering the schema

The consumer does not need to have every telemetry structure compiled into the application beforehand.

BTP provides runtime discovery through the manifest mechanism defined in [Commands and discovery](commands.md#3-the-manifest).

A consumer may request the description of a source using `MANIFEST_REQUEST`.

The source responds with `MANIFEST_DATA`, which describes its topics and their fields.

A typical exchange is:

```text
Consumer                                  Device
   |                                         |
   |------------ MANIFEST_REQUEST ---------->|
   |                                         |
   |<------------- MANIFEST_DATA ------------|
   |                                         |
   |          schema is now known            |
   |                                         |
   |<--------------- TELEMETRY --------------|
   |<--------------- TELEMETRY --------------|
   |<--------------- TELEMETRY --------------|
   |<--------------- TELEMETRY --------------|
```

The manifest may describe, for example:

```text
source_id:      0x11223344
topic_id:       0x0101
schema_version: 1
name:           motor_state
encoding:       PACKED_LE

field 1 -> left_speed
field 2 -> right_speed
field 3 -> left_current
field 4 -> right_current
```

Once this information is known, a telemetry sample does not need to repeat:

```text
left_speed
right_speed
left_current
right_current
rad/s
A
float32
int16
```

It only needs to identify the schema version and carry the encoded values.

### 3.1 Runtime discovery does not require dynamic memory

Discovering a schema while the program is running does **not** imply that an implementation must dynamically allocate memory.

The protocol specifies what information may be discovered.

It does not prescribe how the implementation stores that information internally.

A desktop implementation may choose dynamic containers.

An embedded implementation may instead reserve fixed-capacity tables:

```text
+------------------------------------+
| topic descriptors                  |
| maximum N topics                   |
+------------------------------------+

+------------------------------------+
| field descriptors                  |
| maximum M fields                   |
+------------------------------------+
```

The capacities can be known at compile time or configured for the application.

Therefore:

```text
runtime discovery
        !=
dynamic allocation
```

A device can discover the topics offered by another node while still keeping deterministic memory usage.

The protocol and session limits define what may be represented on the wire. An implementation intended for a constrained target may operate with smaller configured capacities and reject configurations that exceed them rather than allocating memory without bound.

This separation is intentional:

```text
BTP
 |
 +-- defines what is transmitted
 |
 +-- defines how it is interpreted
 |
 `-- does not dictate the application's memory allocator
```

---

## 4. Topic and schema identity

In a `TELEMETRY` message, the envelope's `object_id` **is the topic id**, and it must be non-zero.

A topic is identified by:

```text
(source_id, topic_id)
```

`topic_id` belongs to the source's namespace.

Two different sources may therefore use the same `topic_id` for completely unrelated data:

```text
source 0x1001
topic  0x0101 -> motor_state


source 0x2002
topic  0x0101 -> ambient_conditions
```

There is no collision because topic identity includes `source_id`.

The pair:

```text
(source_id, topic_id)
```

remains stable across boots as long as it continues to represent the same logical topic.

### 4.1 Schema identity

The schema used to decode a particular telemetry sample is identified by:

```text
(source_id, topic_id, schema_version)
```

`schema_version` is a `uint16_le` between:

```text
1 .. 65535
```

Zero is invalid.

Versions are monotonic within one topic.

A version is never reused for a different layout or interpretation.

For example:

```text
motor_state
    |
    +-- schema 1
    |      speed
    |      current
    |
    +-- schema 2
           speed
           current
           temperature
```

If schema version `1` meant one layout in the past, it must never later be reassigned to another layout.

After version `65535`, a source that requires another schema allocates a new `topic_id`.

The version does not wrap to zero or back to one.

---

## 5. Changing a schema

A new `schema_version` is required whenever the interpretation of the payload changes.

Examples include changing:

* field type;
* field order;
* element count;
* engineering unit;
* scale;
* offset;
* nullability;
* encoding;
* semantic meaning.

For example:

```text
schema 1

field 1 -> speed : uint16 : rpm
```

cannot silently become:

```text
schema 1

field 1 -> speed : float32 : rad/s
```

That would make an old consumer interpret valid bytes incorrectly.

Instead:

```text
schema 2

field 1 -> speed : float32 : rad/s
```

must be published.

A presentation-only change that does not modify identity or interpretation does not necessarily require a new schema version.

---

## 6. Payload shape

Every `TELEMETRY` logical payload starts with the schema version.

| Offset |     Size | Field            | Wire type                        |
| -----: | -------: | ---------------- | -------------------------------- |
|      0 |        2 | `schema_version` | `uint16_le`                      |
|      2 | variable | `encoded_body`   | according to the schema encoding |

Conceptually:

```text
+----------------------+-------------------------------+
| schema_version       | encoded_body                  |
| uint16_le            | variable                      |
+----------------------+-------------------------------+
0                      2
```

The schema definition itself is not repeated.

There are no field names, engineering units or descriptions unless the selected body encoding explicitly requires textual data.

For a fragmented message, this is the structure of the **logical payload**.

The schema version appears once:

```text
fragment 0
fragment 1
fragment 2
      |
      v
reassembly
      |
      v
+------------------------------------+
| schema_version | encoded_body      |
+------------------------------------+
```

It does not appear at the start of every fragment.

---

## 7. The schema model

A schema declares at minimum:

```text
source_id
topic_id
schema_version
name
encoding
```

For structured encodings it also defines an ordered list of fields.

Each field declares:

| Property            | Rule                                                             |
| ------------------- | ---------------------------------------------------------------- |
| `field_id`          | Non-zero `uint16`, unique, stable, never reused within the topic |
| `name`              | Human-readable; not an identity on the wire                      |
| `order`             | Unique index, contiguous, starting at zero                       |
| `type`              | One of the wire types defined later in this chapter              |
| `unit`              | Stable engineering unit symbol; `1` means dimensionless          |
| `scale`             | Finite multiplier; default `1`                                   |
| `offset`            | Finite offset; default `0`                                       |
| `element_count`     | `1`, a fixed count from 1 to 65535, or `variable`                |
| `max_element_count` | Required when the count is variable                              |
| `nullable`          | Boolean; default `false`                                         |

`field_id` provides stable identity.

`order` defines serialization order.

They are intentionally different concepts.

For example:

```text
field_id = 15
order    = 0

field_id = 3
order    = 1
```

is valid.

A consumer must not assume:

```text
field_id == order
```

### 7.1 Engineering conversion

A raw wire value is converted to its engineering representation using:

```text
engineering_value = raw * scale + offset
```

For example:

```text
raw    = 300
scale  = 0.01
offset = 0

engineering_value = 3.00
```

`scale` and `offset` belong to the schema.

They are not transmitted with every sample.

They are not applied to `bool`.

For enums, they do not change which label is selected. Enum labels always use the raw integer.

### 7.2 Arrays

A field may contain:

* one scalar;
* a fixed-size array;
* a variable-size array.

For a fixed array:

```text
element_count = N
```

and every sample contains exactly `N` elements when the field is present.

For a variable array, the schema provides:

```text
element_count     = variable
max_element_count = N
```

and each sample carries the actual count.

If an array field is nullable, nullability applies to the **whole field**.

Individual elements cannot independently be null.

---

## 8. Encodings

BTP defines the following telemetry body encodings:

|         Value | Name           | Use                                    |
| ------------: | -------------- | -------------------------------------- |
|        `0x00` | `INVALID`      | Reserved; never announced              |
|        `0x01` | `OPAQUE_BYTES` | Bytes whose meaning is external to BTP |
|        `0x02` | `UTF8`         | UTF-8 text with an explicit size       |
|        `0x03` | `JSON_UTF8`    | One JSON value in UTF-8                |
|        `0x04` | `CSV_UTF8`     | One CSV record in UTF-8                |
|        `0x05` | `PACKED_LE`    | Compact fields in schema order         |
|        `0x06` | `TLV_LE`       | Tag-length-value fields                |
| `0x07`-`0xFF` | —              | Reserved                               |

**`PACKED_LE` is the production encoding.**

`CSV_UTF8` is intended for tests and diagnostics and must never be used as an automatic fallback when binary encoding fails.

A producer does not change encoding without publishing a new schema version.

---

## 9. Why not just use JSON?

JSON remains useful because it is:

* human-readable;
* easy to inspect;
* widely supported;
* convenient during development.

But text representation has a cost.

A traditional self-describing JSON message might look like:

```json
{
  "temperature": 32.4,
  "pressure": 101.2,
  "speed": 1450
}
```

A large portion of the transmitted information is not the measurement itself.

It is structure:

```text
"temperature"
"pressure"
"speed"
:
,
"
{
}
```

If that same set of measurements is transmitted continuously, this structural information is repeatedly sent across the channel.

BTP avoids repeating the schema because the receiver can obtain it through the manifest.

Even BTP's `JSON_UTF8` representation, which uses schema order rather than repeating field names, still represents numbers as text and carries JSON punctuation.

For example:

```text
[32.4,101.2,1450]
```

is larger and more expensive to parse than the corresponding fixed-width binary representation.

With `PACKED_LE`, a schema containing:

```text
temperature : float32
pressure    : float32
speed       : uint16
```

needs only:

```text
4 + 4 + 2 = 10 octets
```

for the actual values.

Including the two-octet `schema_version`, the complete logical telemetry payload is:

```text
12 octets
```

before the BTP envelope.

The receiver already knows what those 10 data octets mean.

The metadata was described separately.

This is the core trade:

```text
SELF-DESCRIBING EVERY SAMPLE

name + type + value
name + type + value
name + type + value
name + type + value
        |
        v
simple, but repetitive


BTP

describe schema once
        |
        v
value + value + value
value + value + value
value + value + value
        |
        v
compact repeated telemetry
```

---

## 10. `PACKED_LE`

`PACKED_LE` is the most compact structured telemetry representation in BTP.

Fields are serialized directly in schema order.

There is:

* no padding;
* no alignment;
* no field name;
* no field identifier;
* no separator;
* no native structure representation.

The receiver already knows the order and type from the schema.

### 10.1 Nullable fields

The body starts with a presence bitmap **only when the schema contains nullable fields**.

With `K` nullable fields:

```text
bitmap_size = ceil(K / 8)
```

octets are used.

Bit zero of the first bitmap octet represents the first nullable field in schema order.

Bits advance from least significant to most significant and then continue in the next octet.

```text
1 = field present
0 = field null
```

Unused bits in the final bitmap octet must be zero.

For example, if a schema has three nullable fields:

```text
bit 0 -> nullable field A
bit 1 -> nullable field B
bit 2 -> nullable field C
```

then:

```text
00000101
```

means:

```text
A -> present
B -> null
C -> present
```

### 10.2 Field layout

After the presence bitmap, all present fields are encoded once in increasing `order`.

Non-nullable fields are always present.

A null field consumes no bytes beyond its presence bit.

A scalar occupies exactly the width of its wire type.

A fixed array contains exactly:

```text
element_count
```

consecutive elements.

No count is transmitted because the receiver already knows it from the schema.

A variable array starts with:

```text
element_count:uint16_le
```

followed by that many consecutive elements.

The count must not exceed:

```text
max_element_count
```

The decoder knows every expected bound from the schema and must consume the body exactly.

Any of the following invalidate the sample:

* truncation;
* excessive array count;
* missing required bytes;
* unexpected trailing bytes.

---

## 11. `TLV_LE`

`TLV_LE` makes each field explicit.

The body consists of zero or more entries:

```text
+----------------------+----------------------+----------------------+
| field_id:uint16_le   | value_size:uint16_le | value[value_size]    |
+----------------------+----------------------+----------------------+
0                      2                      4
```

Entries appear in increasing `field_id`.

A `field_id` never repeats in the same sample.

A nullable field is represented as null by omitting its entry.

Every non-nullable field must be present.

Variable arrays retain their:

```text
element_count:uint16_le
```

prefix inside `value`.

An unknown `field_id` must still have its `value_size` validated so that the parser can skip the bytes safely.

However, an unknown field makes the sample incompatible.

The sample is rejected and counted rather than partially consumed.

`TLV_LE` allows sparse representation and explicit field identification, but costs four additional octets per field:

```text
2 octets field_id
+
2 octets value_size
```

It does not eliminate schema versioning.

Changing the interpretation of an existing field still requires a new schema version.

---

## 12. Text and opaque encodings

### 12.1 `OPAQUE_BYTES`

`OPAQUE_BYTES` is an arbitrary sequence of octets.

Its meaning is external to BTP.

It may contain any value, including:

```text
0x00
CR
LF
```

There is no implicit terminator.

Nullability and scaling do not apply.

### 12.2 `UTF8`

`UTF8` contains well-formed UTF-8 text.

There is:

* no BOM;
* no implicit null terminator;
* no special treatment for `0x00`.

`0x00` represents Unicode U+0000 and does not terminate the value.

Invalid UTF-8 invalidates the sample.

### 12.3 `JSON_UTF8`

`JSON_UTF8` contains exactly one top-level JSON array.

There is one entry per schema field in schema order.

For example:

```json
[32.4, 101.2, 1450]
```

An array-valued field is represented by a nested JSON array.

A null field is represented as:

```json
null
```

The schema remains authoritative for field identity, type and interpretation.

JSON member names are therefore not used as the wire identity of telemetry fields.

### 12.4 `CSV_UTF8`

`CSV_UTF8` contains exactly one CSV record.

There is:

* no header;
* no line terminator.

Fields are comma-separated and may use double quotes where required.

Only scalar fields are supported.

A bare unquoted:

```text
null
```

represents a null field.

An empty field is invalid.

`CSV_UTF8` is a diagnostic representation.

It is not a fallback for a failed binary encoder.

---

## 13. Wire types

Structured telemetry uses explicit BTP wire types.

| Type                                     |          Size | Representation                              |
| ---------------------------------------- | ------------: | ------------------------------------------- |
| `uint8` / `uint16` / `uint32` / `uint64` | 1 / 2 / 4 / 8 | Unsigned, little-endian                     |
| `int8` / `int16` / `int32` / `int64`     | 1 / 2 / 4 / 8 | Two's complement, little-endian             |
| `float32` / `float64`                    |         4 / 8 | IEEE-754 bit pattern, little-endian         |
| `bool`                                   |             1 | `0x00` false, `0x01` true                   |
| `enum8` / `enum16`                       |         1 / 2 | Unsigned value; labels come from the schema |

Values are serialized field by field.

They are not created by casting a C or C++ structure to a byte pointer.

For example, a `float32` is transmitted as its IEEE-754 bit representation in little-endian order.

It is not transmitted as:

```text
"1.500000"
```

and the protocol does not depend on the compiler's structure layout.

---

## 14. Value policy

### 14.1 Non-finite floating-point values

Non-finite values are invalid.

A producer must not emit:

```text
NaN
+Infinity
-Infinity
```

A consumer rejects a sample containing one.

A missing measurement must use a nullable field rather than NaN.

These two situations intentionally remain distinct:

```text
measurement unavailable
        |
        v
       null


invalid arithmetic result
        |
        v
       NaN
        |
        v
     rejected
```

Using NaN as "measurement absent" would make those conditions indistinguishable.

### 14.2 Boolean values

A boolean accepts only:

```text
0x00 -> false
0x01 -> true
```

Any other byte is invalid.

### 14.3 Unknown enum values

An unknown enum value is recoverable.

Because the integer width is already known, an unknown label does not shift the remaining payload.

The receiver:

* keeps the raw integer;
* marks the value as an unknown enum;
* does not invent a label;
* reports the condition;
* continues processing the rest of the sample.

This is the only recoverable telemetry payload error.

### 14.4 Structural errors

Structural errors invalidate the entire logical sample.

Examples include:

* payload shorter than two octets;
* truncated field;
* invalid nullable bitmap;
* oversized variable array;
* TLV size mismatch;
* missing required field;
* trailing unconsumed bytes.

There is:

* no padding;
* no implicit terminator;
* no default value for missing bytes;
* no partial structural decode.

---

## 15. Unknown schemas

A valid BTP frame may carry a telemetry schema that the consumer does not currently know.

This is not a frame error.

For example:

```text
valid envelope
valid CRC
source_id       = 0x11223344
topic_id        = 0x0101
schema_version  = 7
```

but the consumer may only know:

```text
schema_version = 6
```

The consumer must not guess that version 7 is "probably similar".

It must not:

* decode using version 6;
* infer field positions;
* search fields by name;
* assume appended fields;
* partially publish the sample.

Instead:

```text
TELEMETRY schema 7
        |
        v
schema unknown
        |
        +----> reject sample
        |
        `----> refresh manifest
```

The consumer should request a catalog refresh and obtain the current schema through `MANIFEST_REQUEST`.

At minimum, the rejection should record:

```text
source_id
topic_id
received schema_version
reason
```

and increment the appropriate rejection counter.

The BTP frame itself remains structurally valid.

The telemetry sample could simply not be interpreted.

---

## 16. Schema caching

A consumer does not need to request a manifest before every sample.

Once a schema is known, it may be cached.

A typical lifecycle is:

```text
1. connect

2. obtain manifest

3. cache:
       source
       topics
       schema versions
       field descriptions

4. receive telemetry

5. decode using cached schema

6. configuration changes

7. unknown/new schema observed

8. refresh manifest
```

The manifest includes a `config_revision` so that consumers can detect whether cached descriptors remain current.

This keeps normal telemetry traffic compact while still allowing devices to evolve their exposed configuration.

---

## 17. Publishing and fragmentation

A frequently transmitted sample should ideally fit inside one frame of the most restrictive transport on its path.

For ESP-NOW, the BTP payload available to a frame is:

```text
210 octets
```

The first two telemetry octets contain:

```text
schema_version
```

leaving up to:

```text
208 octets
```

for the encoded body of a non-fragmented sample.

This is one reason compact telemetry representation matters.

For example, if the same RF channel is carrying:

```text
telemetry
commands
command responses
terminal input
terminal output
control traffic
```

unnecessary telemetry overhead consumes capacity that could otherwise be used by the other message classes.

Larger telemetry samples use the normal fragmentation mechanism described in [Getting it across the link](fragmentation-and-transports.md).

A consumer must not decode or publish an isolated fragment.

Processing is:

```text
fragment 0
fragment 1
fragment 2
    |
    v
complete reassembly
    |
    v
telemetry validation
    |
    v
schema decode
    |
    v
publish sample
```

An incomplete, expired or inconsistent fragmented message is discarded as a whole.

---

## 18. Client binding

After telemetry has been decoded, applications need a stable way to refer to individual measurements.

A scalar datum is identified by:

```text
(source_id, topic_id, field_id)
```

An array element additionally includes its index:

```text
(source_id, topic_id, field_id, element_index)
```

The field name is not the identity.

For example:

```text
field_id = 7
name     = "temperature"
```

may later receive a presentation-label correction without changing the identity of field 7.

Client applications should therefore bind to identifiers rather than human-readable labels.

### 18.1 Variable arrays

If a variable array does not contain a previously bound element index in the current sample, the binding temporarily has no value.

The client must not:

* reuse the previous sample;
* fabricate zero;
* extend the array artificially.

For example:

```text
previous sample:

[10, 20, 30, 40]


current sample:

[11, 21]
```

a client bound to element 3 does not receive:

```text
40
```

again.

It receives no current value.

### 18.2 Schema changes

`schema_version` determines how a sample is decoded.

It does not replace the stable identity represented by `field_id`.

If a new schema version removes a field or changes it incompatibly, the client binding becomes unavailable until reconfigured.

It never migrates automatically by:

* field name;
* field position;
* similarity.

### 18.3 Presentation is not telemetry metadata

A client binding contains no:

* chart id;
* panel id;
* color;
* axis;
* screen position;
* visualization type.

Several visualizations may consume the same field.

Things such as:

```text
label
color
axis limits
layout
aggregation
chart type
```

belong to client configuration.

They are not part of the telemetry wire format.

---

## 19. Worked example

Consider the following source and topic:

```text
source_id:      0x11223344
topic_id:       0x0101
schema_version: 1
name:           motor_state
encoding:       PACKED_LE
```

The manifest describes:

| order | field_id | name            | type      | unit    | scale | element_count | nullable |
| ----: | -------: | --------------- | --------- | ------- | ----: | ------------: | -------- |
|     0 |        1 | `left_speed`    | `float32` | `rad/s` |     1 |             1 | false    |
|     1 |        2 | `right_speed`   | `float32` | `rad/s` |     1 |             1 | false    |
|     2 |        3 | `left_current`  | `int16`   | `A`     |  0.01 |             1 | false    |
|     3 |        4 | `right_current` | `int16`   | `A`     |  0.01 |             1 | false    |

Assume the current measurements are:

```text
left_speed   =  1.5 rad/s
right_speed  = -2.25 rad/s

left_current raw  = 300
right_current raw = -40
```

The engineering currents are therefore:

```text
300 * 0.01  =  3.00 A

-40 * 0.01  = -0.40 A
```

The complete telemetry payload is:

```text
01 00                      schema_version = 1

00 00 c0 3f                left_speed  = 1.5f
00 00 10 c0                right_speed = -2.25f

2c 01                      left_current
                            raw 300 -> 3.00 A

d8 ff                      right_current
                            raw -40 -> -0.40 A
```

Continuous form:

```text
01 00 00 00 c0 3f 00 00 10 c0 2c 01 d8 ff
```

There are no nullable fields, so no presence bitmap is required.

The body contains:

```text
4 octets left_speed
4 octets right_speed
2 octets left_current
2 octets right_current
--------------------------
12 octets
```

plus:

```text
2 octets schema_version
```

giving a complete logical telemetry payload of:

```text
14 octets
```

The sample contains:

```text
no field names
no unit strings
no separators
no terminator
no padding
```

Yet the receiver can interpret every byte because the manifest already provided the schema.

That is the intended telemetry model.

---

## 20. Nullable example

Consider a second topic:

```text
schema_version: 3
encoding:       PACKED_LE
```

with:

```text
order 0 -> speed        float32   non-nullable
order 1 -> temperature  float32   non-nullable
order 2 -> pressure     float32   nullable
```

Only one field is nullable.

Therefore the body begins with one bitmap octet.

Bit zero represents `pressure`.

If pressure is available:

```text
00000001
```

followed by:

```text
speed
temperature
pressure
```

If pressure is unavailable:

```text
00000000
```

followed only by:

```text
speed
temperature
```

No placeholder bytes for pressure are transmitted.

The schema tells the decoder where the nullable field would have appeared and what type it would have contained.

---

## 21. End-to-end example

The complete idea can be summarized through an unknown device joining a system.

Initially, the consumer knows only that a BTP source exists:

```text
Consumer                               Device
   |                                      |
   |<--------------- HELLO ---------------|
   |                                      |
```

It does not have to contain a compiled C structure for every topic that device may expose.

The consumer requests the manifest:

```text
   |                                      |
   |---------- MANIFEST_REQUEST --------->|
   |                                      |
   |<----------- MANIFEST_DATA -----------|
   |                                      |
```

The manifest describes:

```text
topic 0x0101
name: motor_state
schema: 1
encoding: PACKED_LE

field 1: left_speed
field 2: right_speed
field 3: left_current
field 4: right_current
```

The consumer stores that descriptor according to its own implementation strategy.

That may be:

```text
dynamic objects
```

on a desktop application, or:

```text
fixed-capacity static tables
```

on an embedded target.

Telemetry can then flow efficiently:

```text
   |                                      |
   |<----------- TELEMETRY -------------- |
   |<----------- TELEMETRY -------------- |
   |<----------- TELEMETRY -------------- |
   |<----------- TELEMETRY -------------- |
   |<----------- TELEMETRY -------------- |
```

Each sample carries:

```text
schema_version + values
```

rather than:

```text
name + type + unit + value
name + type + unit + value
name + type + unit + value
...
```

If the producer changes the layout, it publishes a new schema version.

When the consumer observes an unknown version:

```text
   |<------ TELEMETRY schema = 2 ---------|
   |                                      |
   |          unknown schema              |
   |                                      |
   |---------- MANIFEST_REQUEST --------->|
   |                                      |
   |<----------- MANIFEST_DATA -----------|
   |                                      |
   |          schema 2 known              |
```

The consumer never guesses the new layout.

---

## 22. Validation order

After validating the BTP envelope and completing any required reassembly, a telemetry consumer processes the logical sample in the following order:

1. require at least two payload octets;

2. read `schema_version`;

3. reject version zero;

4. resolve exactly:

   ```text
   (source_id, topic_id, schema_version)
   ```

5. confirm that the schema encoding is supported;

6. validate lengths before reading any variable content;

7. validate presence information;

8. validate array counts;

9. validate field widths and types;

10. ensure parsing never reads beyond the logical payload;

11. apply boolean, floating-point and enum policies;

12. require the logical body to be consumed exactly;

13. apply scale and offset;

14. associate engineering units;

15. only then publish the decoded sample to the application.

A failure at any structural step discards the whole logical sample.

The only recoverable payload condition defined in this chapter is an unknown enum label whose raw numeric value remains structurally valid.

---

## 23. Summary

BTP telemetry separates **description** from **data**.

A producer does not need to transmit a self-describing JSON object with every measurement, and a consumer does not need to share the producer's native C or C++ structure.

Instead:

```text
MANIFEST
    |
    | describes
    v
SCHEMA
    |
    | interprets
    v
TELEMETRY
```

The manifest communicates:

* topic identity;
* schema version;
* encoding;
* fields;
* field types;
* units;
* scale;
* offset;
* array information;
* nullability.

The telemetry stream then carries primarily:

```text
schema_version + current values
```

This keeps repeated telemetry compact while still allowing devices and consumers to discover each other's data models at runtime.

The design also does not require dynamic memory.

Runtime discovery describes **when information becomes known**, not **where it must be stored**.

An implementation may use dynamic data structures or fixed-capacity static storage depending on the target platform.

For high-rate or bandwidth-constrained applications, `PACKED_LE` provides the most compact structured representation by encoding fields directly in schema order without names, padding, separators or alignment.

The result is a telemetry model intended to provide both:

```text
flexibility
+
deterministic binary representation
+
compact transmission
+
runtime discoverability
```

without coupling the BTP wire format to any particular programming language, compiler, processor or in-memory structure.
