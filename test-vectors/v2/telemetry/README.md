# Telemetry-sample conformance vectors

These pin the byte layout of a `TELEMETRY` logical payload -- `schema_version`
plus a `PACKED_LE` or `TLV_LE` body -- decoded against a schema, as defined in
[Telemetry payloads](../../../docs/telemetry.md) sections 6, 10, 11 and 13.
They are what `btp::telemetry` is checked against.

## Frame vectors vs telemetry vectors

The vectors two levels up (`../valid/`, `../invalid/`) are **frame** vectors:
a whole BTP datagram, payload opaque. A vector here is the **logical payload
only** (after reassembly), and unlike a message vector it also needs the
**schema** to mean anything -- so each `.json` carries one.

## Layout

```text
telemetry/
  manifest.json                every vector id
  valid/*.{json,bin}           schema + sample -> its exact octets
  invalid/*.{json,bin}         a mutation of a valid base + expected_error
```

Each `.json` has:

* `encoding` -- `PACKED_LE` or `TLV_LE`;
* `schema_version` -- the `uint16_le` prefix;
* `schema` -- the ordered field list (`field_id`, `order`, `type` name,
  `scale`, `offset`, `nullable`, `variable_count` / `max_element_count`);
* `sample` -- one entry per field: `{"raw": [...]}` with the wire values, or
  `{"null": true}`.

`tools/test_telemetry.py` encodes `schema` + `sample` to the `.bin`, decodes
the `.bin` back and checks the round-trip. An invalid vector names a `base`, a
list of byte `mutations`, and the `expected_error` (a `btp::MessageError`
name) the decoder must return.

## Verifying

```bash
python tools/test_telemetry.py --root test-vectors/v2/telemetry --check
```

An independent reimplementation of the `PACKED_LE` / `TLV_LE` rules, the same
principle as the other vector tools: a bug in the C++ and a bug in the
reference decoder cannot hide each other. Run without `--check` to regenerate
the `.bin` files.
