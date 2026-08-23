# Authenticated encryption

BTP version 2 supports authenticated encryption of the application payload.

Authenticated encryption provides two properties:

* **confidentiality** — the payload cannot be read without the cryptographic key;
* **authentication and integrity** — modification of the protected message is detected during authentication.

BTP uses AEAD: **Authenticated Encryption with Associated Data**.

The payload is encrypted, while selected header fields remain visible but are included in the authentication process.

Encryption is optional and configured outside the protocol.

---

## 1. Security model

A BTP communication channel may operate over a medium that cannot be considered trusted.

This is particularly relevant for wireless communication, where another device within radio range may be able to capture or inject frames.

Without cryptographic protection, an attacker may attempt to:

* read telemetry;
* observe command parameters;
* read terminal traffic;
* modify transmitted payloads;
* create forged messages;
* inject frames into the communication channel.

CRC-32 does not protect against these attacks.

A CRC detects accidental corruption but can be recomputed by anyone capable of modifying the frame.

Authenticated encryption provides cryptographic protection for BTP messages when this threat model applies.

---

## 2. Wire representation

Encryption does not change the BTP header layout.

An encrypted logical message uses:

```text
version   = 0x02
ENCRYPTED = 1
```

The logical payload becomes:

```text
ciphertext || authentication_tag
```

The authentication tag is always:

```text
16 octets
```

Therefore, for a plaintext of size `P`:

```text
ciphertext_size = P
tag_size        = 16

logical_payload_size = P + 16
```

The `payload_size` represented in the canonical logical header includes both the ciphertext and the authentication tag.

For example:

```text
plaintext         = 100 octets
ciphertext        = 100 octets
authentication    =  16 octets
--------------------------------
encrypted payload = 116 octets
```

The encrypted logical payload may then be fragmented according to the selected transport profile.

---

## 3. Supported algorithms

BTP version 2 defines two AEAD algorithms.

| `CIPHER_ID` | Algorithm         | Key size  | Nonce size | Tag size  |
| ----------- | ----------------- | --------- | ---------- | --------- |
| `0`         | AES-128-GCM       | 16 octets | 12 octets  | 16 octets |
| `1`         | ChaCha20-Poly1305 | 32 octets | 12 octets  | 16 octets |
| `2`         | Reserved          | —         | —          | —         |
| `3`         | Reserved          | —         | —          | —         |

Supporting two algorithms allows an implementation to select the cipher that better matches the capabilities of the target platform.

The cryptographic algorithm is part of the BTP wire contract, but the way the algorithm is executed internally is not.

An implementation may therefore use:

* a generic software implementation;
* an optimized cryptographic library;
* processor-specific instructions;
* dedicated cryptographic hardware;
* or a combination of hardware and software.

This allows cryptographic processing to be optimized for embedded platforms without creating a platform-specific version of BTP.

For example, ESP32-class devices may provide hardware acceleration for AES through the cryptographic facilities exposed by ESP-IDF. On compatible devices, parts of AES-GCM processing may also be accelerated by hardware, while other operations may remain in software.

A BTP implementation running on such a device can therefore use the hardware-supported AES path instead of performing every cryptographic operation directly on the main processor.

On another target, ChaCha20-Poly1305 may provide a more appropriate software implementation when efficient AES acceleration is not available.

The choice can therefore consider factors such as:

* processor architecture;
* available cryptographic peripherals;
* CPU utilization;
* execution time;
* memory usage;
* power consumption.

BTP does not require a specific cryptographic backend.

For example:

```text
Desktop
    |
    v
optimized software library
    |
    v
AES-128-GCM


ESP32
    |
    v
ESP-IDF / hardware AES
    |
    v
AES-128-GCM


Microcontroller without AES acceleration
    |
    v
optimized software implementation
    |
    v
ChaCha20-Poly1305
```

These implementations can communicate with each other because the internal implementation does not affect the BTP wire representation.

As long as both sides use the same:

* cipher;
* key;
* nonce;
* associated data;
* plaintext representation;

they produce a compatible protected BTP message.

Hardware acceleration is therefore an implementation optimization, not a protocol extension.

`CIPHER_ID` is stored in bits 2 and 3 of the BTP `flags` field.

It identifies the algorithm used to protect the payload.

It does not negotiate the algorithm.

The sender and receiver must already be configured with:

* the same encryption state;
* the same cipher;
* the same cryptographic key.

A key must have exactly the size required by the selected cipher.

---

## 4. Key configuration

BTP does not define cryptographic key provisioning.

Keys are expected to be established through an external mechanism, such as:

* firmware configuration;
* secure device provisioning;
* protected configuration storage;
* another trusted management system.

Transporting cryptographic keys, passwords, or other provisioning secrets inside BTP application payloads is **not recommended**.

BTP payloads are application-defined, and the protocol does not inspect or prohibit their contents. An application can therefore place credentials or cryptographic material inside a telemetry, command, terminal, or control payload.

The protocol does not prevent this behavior.

However, applications should avoid using normal BTP message exchange as a key-provisioning mechanism unless an appropriate external security model has been defined.

In particular, a key used to protect a BTP connection cannot be securely provisioned by sending that same key through an unprotected BTP message.

```text
Unprotected BTP channel
        |
        | encryption key
        v
     Receiver

        NOT RECOMMENDED
```

If cryptographic material must be transferred through BTP, its confidentiality and authenticity must already be protected by an independent trusted mechanism.

BTP itself does not define:

* key exchange;
* password exchange;
* key derivation;
* credential provisioning;
* key rotation;
* key revocation.

These functions belong to the surrounding security architecture.

---

## 5. Nonce derivation

Both supported AEAD algorithms require a unique nonce for each message encrypted with the same key.

BTP derives the nonce directly from the logical-message identity:

```text
nonce =
    LE32(source_id)
    || LE32(boot_id)
    || LE32(sequence)
```

The resulting nonce contains 12 octets:

```text
+-------------+-------------+-------------+
| source_id   | boot_id     | sequence    |
| 4 octets    | 4 octets    | 4 octets    |
+-------------+-------------+-------------+
               12 octets
```

The nonce is not transmitted as a separate field because all values required to reconstruct it are already present in the BTP header.

### 5.1 Nonce uniqueness requirement

The tuple:

```text
(source_id, boot_id, sequence)
```

**must never repeat while the same encryption key is in use.**

This is a cryptographic requirement.

For producers sharing the same key, `source_id` values must be unique within that key domain.

For one producer:

* `boot_id` must not be reused under the same key;
* `sequence` must not repeat within one `boot_id`;
* each new logical message must receive a new sequence value.

Therefore:

```text
same key
+
same source_id
+
same boot_id
+
same sequence
```

must never be used to encrypt two different logical messages.

Nonce reuse can compromise the security guarantees of AEAD algorithms, particularly AES-GCM.

### 5.2 Retransmission

Retransmitting the same already-protected logical message does not create a new message identity.

The original encrypted payload and authentication tag can be transmitted again.

An implementation must not use the same:

```text
(source_id, boot_id, sequence)
```

to encrypt different plaintext.

If the application creates a new logical message, it must assign a new sequence number before encryption.

---

## 6. Associated data

AEAD can authenticate data without encrypting it.

BTP uses this mechanism to protect the logical-message header.

The associated data, or AAD, is a canonical 36-octet BTP header.

Conceptually:

```text
AEAD(
    key,
    nonce,
    AAD = canonical logical header,
    plaintext
)
```

produces:

```text
ciphertext || tag
```

The header remains visible on the wire, but modifications to authenticated header fields cause tag verification to fail.

### 6.1 Canonical logical header

Fragmentation modifies some header fields between frames.

Those transport-dependent values cannot be used directly as AAD because the same logical message may be fragmented differently on another transport.

Before generating the AAD, the header is normalized to represent the complete logical message:

```text
version         = 2
FRAGMENTED      = 0
fragment_index  = 0
fragment_count  = 1
payload_size    = complete ciphertext + tag size
```

Other logical-message fields are preserved.

The resulting header is serialized using the standard 36-octet BTP header representation.

```text
                  physical fragment header
                           |
                           v
                  canonicalization
                           |
                           v
+----------------------------------------------------+
|          canonical logical BTP header              |
|                   36 octets                        |
+----------------------------------------------------+
                           |
                           v
                          AAD
```

### 6.2 Authenticated header information

The canonical AAD binds the encrypted payload to logical-message properties including:

* `version`;
* `type`;
* applicable `flags`;
* `source_id`;
* `boot_id`;
* `sequence`;
* `timestamp_us`;
* `object_id`;
* logical payload size.

Changing one of these authenticated properties causes authentication to fail.

The transport-specific fragmentation representation is normalized and is therefore not authenticated as a particular set of physical fragments.

---

## 7. Encryption and fragmentation

Encryption is performed on the **complete logical payload before fragmentation**.

The sender processing order is:

```text
plaintext
    |
    v
AEAD encryption
    |
    v
ciphertext || tag
    |
    v
fragmentation
    |
    v
BTP frames
```

In steps:

1. create the logical-message header;
2. set `ENCRYPTED`;
3. select `CIPHER_ID`;
4. derive the nonce;
5. generate the canonical logical header;
6. use the canonical header as AAD;
7. encrypt the complete plaintext;
8. append the 16-octet authentication tag;
9. fragment the resulting encrypted payload if required;
10. encode each fragment as a BTP frame.

Each resulting frame receives its own CRC-32.

The authentication tag belongs to the complete logical message, not to an individual fragment.

---

## 8. Decryption and reassembly

The receiver performs the inverse operation.

```text
BTP frames
    |
    v
frame validation
    |
    v
reassembly
    |
    v
ciphertext || tag
    |
    v
AEAD authentication
    |
    v
plaintext
```

The receiver must:

1. decode each BTP frame;
2. validate its frame CRC and header;
3. collect all fragments belonging to the logical message;
4. reconstruct the complete encrypted payload;
5. reconstruct the canonical logical header;
6. derive the nonce from `source_id`, `boot_id`, and `sequence`;
7. select the configured key and `CIPHER_ID`;
8. verify the authentication tag;
9. decrypt the payload;
10. deliver the plaintext to the application only after successful authentication.

A message that fails AEAD authentication must not be delivered as valid application data.

---

## 9. Gateway forwarding

The canonical AAD allows an encrypted logical message to cross transports with different frame-size limits.

Consider a message received as:

```text
Fragment 0
Fragment 1
Fragment 2
Fragment 3
```

A gateway may reassemble the encrypted logical payload:

```text
ciphertext || tag
```

and then transmit it through a transport with a larger payload capacity as:

```text
Fragment 0
Fragment 1
```

The gateway does not need to decrypt or re-encrypt the message.

```text
Transport A

[frag 0]
[frag 1]
[frag 2]
[frag 3]
    |
    v
+-----------+
|  Gateway  |
+-----------+
    |
    | reassembly
    |
    | ciphertext || tag
    |
    | re-fragmentation
    v

Transport B

[frag 0]
[frag 1]
```

The following logical properties remain unchanged:

```text
source_id
boot_id
sequence
timestamp_us
type
object_id
ciphertext
authentication tag
```

Only the fragmentation representation changes.

Because fragmentation fields are normalized before AAD generation, the authentication tag remains valid after re-fragmentation.

The gateway therefore does not require access to the cryptographic key.

---

## 10. CRC and AEAD

CRC-32 and AEAD have different purposes.

| Mechanism       | Protects against                                    | Cryptographic |
| --------------- | --------------------------------------------------- | ------------- |
| CRC-32          | Accidental frame corruption                         | No            |
| AEAD tag        | Unauthorized modification of protected message data | Yes           |
| AEAD encryption | Passive reading of protected payload                | Yes           |

CRC validation occurs at the frame level.

```text
BTP frame
    |
    v
CRC validation
```

AEAD authentication occurs at the logical-message level after reassembly.

```text
logical encrypted message
    |
    v
AEAD authentication
```

A frame can therefore have:

```text
valid CRC
```

while the complete message later produces:

```text
TagMismatch
```

A valid CRC does not mean that the sender was authorized and does not mean that the encrypted payload has been authenticated.

---

## 11. Authentication failure

AEAD decryption succeeds only when the authentication tag is valid for:

* the configured key;
* the selected cipher;
* the derived nonce;
* the canonical associated data;
* the received ciphertext.

Otherwise, authentication fails.

Possible causes include:

* modified ciphertext;
* modified authenticated header fields;
* incorrect key;
* incorrect cipher configuration;
* corrupted encrypted payload;
* incompatible logical-message metadata.

The reference implementation reports authentication failure as:

```text
TagMismatch
```

The application must discard the message.

It must not:

* process the unauthenticated plaintext;
* retry using plaintext automatically;
* bypass authentication;
* reinterpret the message as an unencrypted frame.

---

## 12. Replay protection

Authenticated encryption does not provide replay protection by itself.

An attacker may capture a complete valid encrypted message and transmit the same message again.

Because the message itself has not been modified:

```text
ciphertext  -> unchanged
tag         -> unchanged
header      -> unchanged
```

the authentication tag can remain valid.

BTP version 2 does not maintain a receive-side replay window or record which sequence numbers have already been accepted.

Systems that require replay protection must implement an additional policy based on message identity, sequence tracking, session state, or another mechanism appropriate to the application.

This is particularly important for control messages whose repeated execution may change system state.

Command-level duplicate handling is defined separately from cryptographic replay protection.

---

## 13. Metadata visibility

BTP encrypts the application payload.

The frame header remains visible.

An observer may therefore still obtain information such as:

* `source_id`;
* `boot_id`;
* `sequence`;
* `timestamp_us`;
* `type`;
* `object_id`;
* message size;
* transmission frequency.

For example, an observer may not know the value of a telemetry measurement but may still determine:

* which producer is transmitting;
* how often it transmits;
* when commands occur;
* the approximate amount of data exchanged.

BTP therefore provides payload confidentiality, not traffic-flow confidentiality.

---

## 14. Key scope and identity

Authentication proves possession of the configured cryptographic key.

If several devices share the same key, AEAD alone cannot cryptographically distinguish which key holder generated a valid message.

For example:

```text
Device A ─┐
Device B ─┼── shared key K
Device C ─┘
```

a valid authentication tag proves that the message was generated using `K`.

It does not independently prove whether the sender was Device A, B, or C.

`source_id` is authenticated as part of the logical header, but a device possessing the same shared key can generate a valid message using another `source_id`.

Systems requiring cryptographic per-device identity should use independent keys or an external identity mechanism.

---

## 15. Key lifetime

BTP does not define:

* key exchange;
* key rotation;
* key expiration;
* key revocation;
* forward secrecy.

These functions belong to the surrounding security architecture.

If a long-lived shared key is compromised, messages protected using that key may also be compromised.

Applications must define an appropriate key-management policy for their threat model and deployment environment.

---

## 16. Availability

Encryption does not protect communication availability.

An attacker with access to the physical communication medium may still attempt to:

* jam an RF channel;
* generate interference;
* flood a receiver with invalid frames;
* consume link capacity.

These are availability attacks and are outside the protection provided by BTP authenticated encryption.

BTP can detect invalid authenticated messages, but it cannot guarantee that valid messages reach their destination.

---

## 17. Transport restrictions

A transport profile may impose additional restrictions on encrypted messages.

The current USB HID profile does not support `ENCRYPTED` frames because the 16-octet authentication tag consumes a significant portion of its available frame payload.

This restriction belongs to the transport profile and does not change the general BTP encryption model.

Transport-specific encryption support is defined in [Fragmentation and transport profiles](fragmentation-and-transports.md).

---

## 18. Reference implementation

Cryptographic processing is separated from the core BTP frame codec.

The frame codec is responsible for:

* encoding and decoding the BTP envelope;
* validating frame structure;
* validating CRC-32;
* checking encryption-related header consistency.

It does not authenticate or decrypt application payloads.

Cryptographic operations are provided by the optional `btp::aead` component.

The reference implementation supports:

```text
AES-128-GCM
ChaCha20-Poly1305
```

through interchangeable cryptographic backends.

The backend does not affect the BTP wire representation.

A message encrypted by one conforming backend can be decrypted by another when both use the same:

* cipher;
* key;
* nonce;
* AAD;
* ciphertext representation.

The public AEAD operations report:

| Result            | Meaning                                                           |
| ----------------- | ----------------------------------------------------------------- |
| `Ok`              | Operation completed successfully                                  |
| `InvalidArgument` | Invalid key, buffer, size, header, or cryptographic configuration |
| `InvalidCipherId` | Unsupported or reserved `CIPHER_ID`                               |
| `TagMismatch`     | Authentication failed                                             |

Separating cryptographic processing from the core codec allows applications that do not require encryption to use the BTP framing layer without including a cryptographic dependency.

It also allows the cryptographic backend to be replaced or optimized for a particular platform without changing the rest of the protocol implementation.

For example, an embedded target may use a hardware-accelerated AES backend while another implementation uses a software backend.

Both remain interoperable because the backend is not represented on the wire.

---

## 19. Security limitations

BTP authenticated encryption provides:

* payload confidentiality;
* payload integrity;
* authentication of the canonical logical-message header;
* cryptographic detection of modified protected messages.

It does not provide:

* replay protection;
* metadata confidentiality;
* RF jamming protection;
* traffic-flow confidentiality;
* key provisioning;
* automatic key rotation;
* forward secrecy;
* certificate management;
* per-device cryptographic identity when a key is shared.

BTP also does not restrict which information an application places inside a payload.

Applications remain responsible for deciding whether credentials, passwords, cryptographic keys, or other sensitive information should be transmitted.

These limitations must be considered when defining the security architecture of a BTP deployment.

---

## 20. Summary

BTP version 2 protects application payloads using authenticated encryption.

The protected logical message is constructed as:

```text
plaintext
    |
    | AEAD
    | key
    | nonce = source_id || boot_id || sequence
    | AAD   = canonical logical header
    v
ciphertext || 16-octet tag
```

The encrypted payload can then be fragmented and transported using any profile that supports encryption.

The receiver performs frame validation and reassembly before authenticating and decrypting the complete logical message.

This design provides message-level cryptographic protection while allowing gateways to re-fragment encrypted traffic without access to the encryption key.

The protocol also allows the cryptographic implementation to be optimized independently for each target platform.

A resource-constrained embedded device may use hardware acceleration or a cipher better suited to software execution, while another implementation may use a completely different cryptographic backend.

These implementation choices do not change the BTP wire format and do not affect interoperability.

Its security depends on three external requirements:

1. cryptographic keys must be provisioned securely;
2. `(source_id, boot_id, sequence)` must never repeat under the same key for different encrypted messages;
3. applications requiring replay protection or stronger identity guarantees must provide those mechanisms outside the BTP AEAD layer.

BTP does not prevent an application from transmitting passwords, keys, or other sensitive values inside its payloads. Such use is application-defined and should only be adopted when an appropriate security mechanism already protects those values.
