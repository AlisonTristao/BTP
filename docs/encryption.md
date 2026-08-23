# Encryption

Wire version `0x02` adds authenticated encryption of the payload. This chapter
explains what it protects, how it is wired into the envelope, and — just as
importantly — what it deliberately does not cover.

## 1. Why a CRC is not enough

The envelope's CRC-32 detects accidental corruption: a flipped bit on a radio
hop, a truncated read. It is not a security mechanism and never was.

Anyone who can modify a frame can recompute its CRC in a few lines of code. A
valid CRC proves that the bytes arrived as they were sent by *whoever sent
them*. It says nothing about who that was.

AEAD — authenticated encryption with associated data — closes both gaps at
once. The payload becomes unreadable to anyone without the key, and a 16-octet
authentication tag makes any modification detectable, including modification of
the header fields that are not themselves encrypted.

## 2. What changes on the wire

Exactly three things:

1. The `ENCRYPTED` flag (`0x0002`) is set.
2. The version octet becomes `0x02`, because the encoder derives it from that
   flag.
3. The payload becomes `ciphertext || tag`, so `payload_size` grows by 16.

Both ciphers are stream constructions, so the ciphertext is the same length as
the plaintext. The whole overhead is the 16-octet tag.

Nothing else moves. The header keeps the same 36-octet layout at the same
offsets, and the envelope CRC is computed over the encrypted payload exactly as
it would be over a plaintext one.

## 3. The two ciphers

| `CIPHER_ID` | Cipher | Key size | Tag |
| ---: | --- | ---: | ---: |
| `0` | AES-128-GCM | 16 octets | 16 octets |
| `1` | ChaCha20-Poly1305 | 32 octets | 16 octets |

AES-128-GCM is the default and is the right choice where hardware AES exists.
ChaCha20-Poly1305 is there for targets without it, where a software AES would
be slow or would leak timing.

The key sizes are **not interchangeable**, which is why the library takes a key
as a pointer and a length rather than a fixed-size type, and rejects a length
that does not match the selected cipher exactly.

`CIPHER_ID` lives in bits 2-3 of `flags` as a two-bit enum. It **identifies**
which cipher produced this payload. It does not negotiate: see section 7.

## 4. The nonce is derived, not transmitted

```text
nonce = source_id (4, LE) || boot_id (4, LE) || sequence (4, LE)
```

Twelve octets, which is what both ciphers want, built entirely from header
fields the frame already carries. No counter, no random value, no extra wire
field.

This works because nonce uniqueness falls out of rules the protocol already
enforces for other reasons:

- `source_id` is unique within a routing domain, so two producers cannot
  collide.
- `sequence` never repeats within a boot.
- `boot_id` changes on every boot, so a restart cannot replay a range of
  sequence numbers under the same key.

That last point is what makes the construction safe rather than merely
convenient. Without `boot_id`, a producer that rebooted and restarted its
sequence at zero would reuse nonces — and nonce reuse in GCM is catastrophic,
not merely untidy.

`boot_id` is not a secret and is not a key derivation input. It travels in the
clear like every other header field.

## 5. The AAD is the canonicalized logical header

This is the most interesting decision in the design, and the one that makes
encryption compatible with a multi-transport path.

The associated data is the 36-octet header — so the identity, the timestamp and
the object id are authenticated even though they are not encrypted — but it is
the header of the **logical message**, canonicalized:

```text
FRAGMENTED      cleared
fragment_index  0
fragment_count  1
payload_size    the full ciphertext + tag size of the whole message
version         2
```

The tag is therefore computed **once per logical message, before fragmenting**,
and every field that fragmentation touches is excluded from it.

The consequence is worth stating plainly: a gateway can reassemble an encrypted
message that arrived over one transport and re-fragment it onto another with a
different size ceiling, **without holding the key**, and the tag still
verifies. The per-fragment sizes changed, the indices changed, the per-frame
CRCs changed — none of it entered the tag.

Without this canonicalization, encryption and crossing transports would be
mutually exclusive, and the protocol would have to pick one.

This is also why reassembly restores the logical header on completion: it hands
back exactly the form the tag was computed over.

The conformance vectors `aead_fragmented_gcm_0` and `aead_fragmented_gcm_1`
exist to pin this down. They are the two ESP-NOW fragments of one 220-octet
message sealed whole, and the test suite reassembles them out of order and
opens the result.

## 6. Encryption is layered outside the codec

`btp::codec` never encrypts, decrypts or verifies a tag. It has no crypto
dependency at all, and that is deliberate: an integrator who does not need
encryption links a codec that pulls in nothing.

What `decode()` checks on an encrypted frame is only *framing* consistency —
that the version matches the flag, that the size adds up, that the envelope CRC
is right over the ciphertext. **A frame that passes `decode()` has not been
authenticated.** Verifying the tag is a separate call the caller makes, against
a key the library never sees.

The order for a receiver is therefore: decode the frame, reassemble if
fragmented, then open the reassembled message. The order for a sender is:
seal the logical payload, then fragment, then encode each fragment.

That sender order is why the library exposes a way to serialize a header
without encoding a frame — the AAD has to exist before the payload is
encrypted, but `encode()` expects a payload that is already encrypted.

### Two backends, one contract

`btp::aead` reaches its ciphers through one of two backends, picked at compile
time by which headers the target actually has:

| Backend | Selected when | Seen on |
|---|---|---|
| classic | `<mbedtls/gcm.h>` and `<mbedtls/chachapoly.h>` are reachable | mbedtls 2.x/3.x — the Arduino ESP32 SDK, and the mbedtls this project's CMake build fetches |
| PSA | they are not, but `<psa/crypto.h>` is | mbedtls 4.x / TF-PSA-Crypto — ESP-IDF 6.x, which moved those two headers under `mbedtls/private/` |

Classic is preferred where both would work, so a target that builds today keeps
generating exactly the code it generated before PSA existed.

The distinction does not reach the wire or the caller. Both compute the same
AES-128-GCM and ChaCha20-Poly1305 over the same nonce and AAD, so a peer built
against one interoperates with a peer built against the other, and the v2 AEAD
conformance vectors are run against both. Neither asks the caller to initialize
anything: PSA's process-wide `psa_crypto_init()` is made lazily by the backend
itself, because requiring it of the caller fails in the worst possible shape —
every operation, from the first, with an error that reads like a bad argument
rather than a missing setup step.

On a target with neither backend the translation unit is empty, so a call fails
at link time. That is deliberate: a stub returning an error would let a build
that believes it is encrypting ship without doing so.

## 7. What v2 does not protect

Everything in this section is an accepted limitation, not a defect.

**No anti-replay.** A captured valid frame can be reinjected and will verify,
because nothing in the protocol tracks which sequence numbers have been seen.
If your threat model includes an attacker who can record and resend, you need a
layer above BTP.

**No metadata confidentiality.** Only the payload is encrypted. `source_id`,
`boot_id`, `sequence`, `timestamp_us`, `type` and `object_id` travel in the
clear even with `ENCRYPTED` set. An observer cannot read your samples but can
see who is talking, how often, and on which channel. They are *authenticated*,
so they cannot be modified undetected — but they are not hidden.

**No key rotation and no forward secrecy.** There is one static key. Anyone who
obtains it can read every message ever captured under it, past and future.

**No per-peer identity.** Authentication is by possession of the shared key.
Two peers holding the same key are indistinguishable to the cipher, so the tag
proves the message came from *someone in the group*, not from a specific
sender.

**No key provisioning.** Distributing keys is out of scope, and a key must
never appear in any field of any message. Before deploying you need your own
mechanism, and the protocol will not help you build it.

## 8. Encryption is a static decision

There is no negotiation of any kind. Not in `HELLO`, not anywhere.

Whether a channel is encrypted, and with which cipher, is configuration decided
out of band before the first frame. `CIPHER_ID` tells a receiver how to open
what it just got; it does not ask.

This means there is no wire case for "one side encrypts and the other does
not". A mismatch is a deployment error and it fails visibly: the receiver
rejects frames it cannot make sense of, rather than falling back to cleartext.
There is no fallback within a channel, by design — a downgrade path is an
attack surface, and this protocol does not have one.

## 9. Not available on USB HID

An encoder is refused if it sets `ENCRYPTED` on a frame bound for the USB HID
profile, and a decoder is refused if it is handed one. The reason is the
arithmetic: 16 octets of tag over a 22-octet payload ceiling is 73% overhead,
against roughly 7.6% on ESP-NOW and 0.4% on serial.

The rule used to live only in prose, which meant an encoder could produce
frames every conforming decoder was supposed to refuse while no decoder
actually refused them. It is now enforced in both directions.
