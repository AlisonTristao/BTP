# Wire v2 conformance vectors (AEAD)

Wire v2 adds the `ENCRYPTED` flag, the version-2 octet and the `CIPHER_ID`
sub-field. These vectors pin down how that extension appears on the wire.

The file conventions are the same as
[wire v1](../v1/README.md): a `.json` describing each `.bin`, a
`manifest.json`, and `valid/` beside `invalid/` where every invalid case names
the exact error it must provoke.

## What these vectors do and do not prove

`btp::codec` understands framing only — magic, version, size, envelope CRC,
flags, fragmentation. **It does not verify the AEAD tag.** That is a separate
call the integrator makes against a key the codec never sees.

So a vector here proves the *framing* of an encrypted frame: that the version
octet followed from the flag, that `payload_size` accounts for the 16-octet
tag, that the envelope CRC covers header plus ciphertext plus tag.

A "ciphertext with a corrupted tag" case is deliberately absent. Such a frame
is structurally valid and `decode()` accepts it, correctly — detecting it is
the cipher's job. Those cases live in `tests/test_aead_conformance.cpp`, which
links the AEAD target and can actually check a tag.

## The valid vectors

| Vector | What it covers |
| --- | --- |
| `hello` | A plain `CONTROL` frame, unchanged from v1 |
| `aead_telemetry_gcm` | AES-128-GCM, one frame |
| `aead_telemetry_chacha20poly1305` | ChaCha20-Poly1305, one frame |
| `aead_fragmented_gcm_0` / `_1` | Two ESP-NOW fragments of one message sealed whole |

The `aead` block in each JSON publishes the key, nonce, AAD and plaintext, so
any implementation in any language can reproduce the exact ciphertext and tag.
Those keys are fixed, public, sequential test values documented purely for
reproduction — a real key never travels and is never written down in the clear.

The fragmented pair is the important one. The message is sealed **once**, whole,
and only then cut, so the tag sits at the end of fragment 1 and neither
fragment can be opened alone. Its AAD is the canonicalized *logical* header —
`FRAGMENTED` cleared, index 0, count 1, `payload_size` set to the full
ciphertext-and-tag size — which deliberately differs from either fragment's own
on-wire header.

That is what lets a gateway reassemble an encrypted message from one transport
and re-fragment it onto another without holding the key, and it is what
`test_aes_gcm_fragmented_vector_reassembles_and_decrypts()` checks end to end:
Python sealed those bytes, and C++ reassembles them out of order and opens them
under mbedtls.

## The invalid vectors

| Vector | Expected error |
| --- | --- |
| `encrypted_version_mismatch` | `ENCRYPTED` set with a version other than 2 |
| `crc_mismatch_encrypted` | Envelope CRC wrong over the ciphertext |
| `reserved_flag` | A reserved flag bit set |
| `cipher_id_reserved` | `CIPHER_ID` of 2 or 3 |
| `cipher_id_requires_encrypted` | Non-zero `CIPHER_ID` without `ENCRYPTED` |

## The payload-layer vectors

`messages/` and `telemetry/` are separate sets one layer up from the frame:
the `.bin` is a logical payload with no frame around it.

* `messages/` -- `COMMAND` / `CONTROL` payloads, checked against `btp::messages`
  by `tools/test_messages.py`. See [messages/README.md](messages/README.md).
* `telemetry/` -- a `TELEMETRY` sample body (each vector carries the schema it
  is decoded against), checked against `btp::telemetry` by
  `tools/test_telemetry.py`. See [telemetry/README.md](telemetry/README.md).

## Verifying

```bash
python tools/test_vectors_v2.py --root test-vectors/v2 --check
python tools/test_messages.py --root test-vectors/v2/messages --check
python tools/test_telemetry.py --root test-vectors/v2/telemetry --check
```

`tools/test_vectors_v2.py` is a sibling of the v1 tool, not a wrapper around
it: its reference decoder independently reimplements the v2 rules, so a bug in
one reference decoder cannot silently hide in the other. `tools/test_messages.py`
and `tools/test_telemetry.py` do the same one layer up, for the payloads.
