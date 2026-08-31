# Message-layer conformance vectors

These pin the byte layout of every `COMMAND` and `CONTROL` payload defined in
[Commands and discovery](../../../docs/commands.md) and
[Session and terminal](../../../docs/session-and-terminal.md) -- HELLO,
MANIFEST_DATA, STATUS, SUBSCRIBE, COMMAND_REQUEST and the rest. They are what
`btp::messages` is checked against.

## Frame vectors vs message vectors

The vectors one level up (`../valid/`, `../invalid/`) are **frame** vectors: a
whole BTP datagram -- 36-octet header, payload, CRC -- and the payload is
opaque to them. `btp::codec` and `tools/test_vectors_v2.py` own those.

A vector here is a **message** vector: the `.bin` is the **logical payload
only**, after reassembly, with no frame around it. It is the input to
`btp::decode_hello` / `btp::ManifestReader` / etc., not to `btp::decode`.
`manifest_data/valid/manifest_data_source_info_only.bin` is byte-for-byte the
payload of the frame vector `../valid/manifest_data.bin` -- the same octets,
seen from the layer above.

## Layout

```text
messages/
  manifest.json                 every vector id, valid and invalid
  <object>/valid/*.{json,bin}    one payload_model -> its exact octets
  <object>/invalid/*.{json,bin}  a mutation of a valid base + expected_error
```

`<object>` is `hello`, `hello_result`, `command_request`, `command_result`,
`manifest_request`, `manifest_data`, `subscribe`, `subscribe_result`,
`unsubscribe`, `unsubscribe_result`, `session_close`, `session_close_result`,
or `status`.

A valid vector's `payload_model` is the contract: the decoded structure,
field by field. The tool encodes it to the `.bin` and decodes the `.bin` back
to check the round-trip. An invalid vector names a `base`, a list of byte
`mutations`, and the `expected_error` (a `btp::MessageError` name) the decoder
must return.

## Verifying

```bash
python tools/test_messages.py --root test-vectors/v2/messages --check
```

`tools/test_messages.py` is an independent reimplementation of the layout
rules, the same principle as the frame tools: a bug in the C++ and a bug in
the reference decoder cannot hide each other. Run without `--check` to
regenerate the `.bin` files from the `payload_model`s.
