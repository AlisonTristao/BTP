#ifndef BTP_AEAD_HPP
#define BTP_AEAD_HPP

#include "btp/codec.hpp"

#include <cstddef>
#include <cstdint>

namespace btp {

// Two interchangeable backends implement everything below, chosen at compile
// time by which crypto headers the target actually has (see the top of
// src/aead.cpp): mbedtls's classic <mbedtls/gcm.h> and <mbedtls/chachapoly.h>
// where they are public -- the Arduino ESP32 SDK, and the mbedtls this
// project's CMake build fetches -- and the PSA Crypto API where they are not,
// which is mbedtls 4.x / TF-PSA-Crypto as shipped by ESP-IDF 6.x.
//
// The choice is invisible from here. Both compute the same AES-128-GCM and
// ChaCha20-Poly1305 over the same nonce and AAD, so a peer built against one
// interoperates with a peer built against the other, and neither asks the
// caller to initialize anything -- the v2 AEAD conformance vectors are run
// against both. If a target has neither backend, this translation unit is
// empty and a call here fails at link time rather than silently doing nothing.

static const std::size_t kAesGcmKeySize = 16U;            // 128 bits, AES-128-GCM
static const std::size_t kChaCha20Poly1305KeySize = 32U;  // 256 bits, RFC 8439

// A view, not an owner -- mirrors ByteView (codec.hpp) -- because the two
// supported ciphers use different, non-interchangeable key sizes (docs/encryption.md section 3: 16 octets for AES-128-GCM, 32 for ChaCha20-Poly1305), so a
// single fixed-size key type could not serve both.
struct AeadKey {
    const std::uint8_t* data;
    std::size_t size;
};

// Deliberately separate from btp::Error (codec.hpp): codec.hpp has zero
// dependencies by design (see ADR 0012) and must never gain AEAD-specific
// vocabulary, so AEAD failures need their own enum rather than extending it.
enum class AeadError : std::uint8_t {
    Ok,
    InvalidArgument,
    InvalidCipherId,
    TagMismatch
};

// Seals one logical message with AES-128-GCM (docs/encryption.md section 3).
// out_ciphertext_and_tag must have room for payload_size + 16 octets.
AeadError aead_seal_aes_gcm(const AeadKey& key, const Header& header,
                            std::uint16_t payload_size,
                            const std::uint8_t* plaintext,
                            std::uint8_t* out_ciphertext_and_tag) noexcept;

// Opens one logical message sealed by aead_seal_aes_gcm(). ciphertext_size
// already includes the trailing 16-octet tag (docs/encryption.md section 2);
// out_plaintext must have room for ciphertext_size - 16 octets.
AeadError aead_open_aes_gcm(const AeadKey& key, const Header& header,
                            std::uint16_t ciphertext_size,
                            const std::uint8_t* ciphertext_and_tag,
                            std::uint8_t* out_plaintext) noexcept;

// Seals one logical message with ChaCha20-Poly1305 (docs/encryption.md section 3).
// out_ciphertext_and_tag must have room for payload_size + 16 octets.
AeadError aead_seal_chacha20poly1305(const AeadKey& key, const Header& header,
                                     std::uint16_t payload_size,
                                     const std::uint8_t* plaintext,
                                     std::uint8_t* out_ciphertext_and_tag) noexcept;

// Opens one logical message sealed by aead_seal_chacha20poly1305().
// ciphertext_size already includes the trailing 16-octet tag (docs/encryption.md section 2); out_plaintext must have room for ciphertext_size - 16 octets.
AeadError aead_open_chacha20poly1305(const AeadKey& key, const Header& header,
                                     std::uint16_t ciphertext_size,
                                     const std::uint8_t* ciphertext_and_tag,
                                     std::uint8_t* out_plaintext) noexcept;

// Dispatches to aead_seal_aes_gcm() or aead_seal_chacha20poly1305() based on
// the CIPHER_ID sub-field of header.flags (docs/encryption.md section 3).
AeadError aead_seal(const AeadKey& key, const Header& header,
                    std::uint16_t payload_size, const std::uint8_t* plaintext,
                    std::uint8_t* out_ciphertext_and_tag) noexcept;

// Dispatches to aead_open_aes_gcm() or aead_open_chacha20poly1305() based on
// the CIPHER_ID sub-field of header.flags. ciphertext_size already includes
// the trailing 16-octet tag (docs/encryption.md section 2).
AeadError aead_open(const AeadKey& key, const Header& header,
                    std::uint16_t ciphertext_size,
                    const std::uint8_t* ciphertext_and_tag,
                    std::uint8_t* out_plaintext) noexcept;

// ---------------------------------------------------------------------------
// AeadCipher -- one imported key, reused across many seal() / open() calls
// ---------------------------------------------------------------------------
//
// Every aead_seal_*() / aead_open_*() call above re-derives the cipher from
// raw key bytes from scratch: the classic backend re-runs the AES key
// schedule and rebuilds GCM's GHASH multiplication table (or re-runs
// ChaCha20-Poly1305's key setup); the PSA backend re-imports the key into the
// keystore. That is the right cost model for a key used once or rarely --
// a HELLO handshake reply, say -- where importing it is not worth
// remembering. It is wasted work for a node that seals every outgoing
// TELEMETRY frame, or opens every inbound one, under the SAME key for the
// life of a session: the key never changes between calls, only the header
// (nonce) and the payload do.
//
// AeadCipher is that key import, done once by reset(), reused by every
// later seal() / open() against it -- the classic backend's key schedule /
// GHASH table or the PSA key handle stays live in this object until the next
// reset() or the destructor. Same guarantees as the rest of the library: no
// heap (the backend context lives in storage_ below, sized generously and
// checked by a static_assert in aead.cpp against the real backend struct
// sizes, so a future backend upgrade that outgrows it fails the build rather
// than corrupting memory), noexcept, no global state. The stateless
// aead_seal_*() / aead_open_*() functions above are unaffected and remain
// the right choice for a key that is not worth caching.
//
// Unlike PsaKey (aead.cpp, internal), a key imported here is usable for BOTH
// directions -- reset() grants encrypt AND decrypt -- because the whole point
// of this class is a node that seals its own traffic and opens its peer's
// under the one symmetric key, not two separately-scoped one-way imports.
//
//   btp::AeadCipher channel_b;
//   if (!channel_b.reset(btp::CipherId::AesGcm, key)) { ... }
//   ...
//   // every frame on this key, from here on:
//   channel_b.seal(header, payload_size, plaintext, out);
//   channel_b.open(header, ciphertext_size, ciphertext_and_tag, out_plaintext);
//
// NOT internally synchronised -- seal() / open() mutate backend working
// state the same way a raw mbedtls_gcm_context / mbedtls_chachapoly_context
// already does, so one AeadCipher belongs to one caller (or its own critical
// section), not several tasks sharing it unlocked.
static const std::size_t kAeadCipherStorageSize = 512U;

class AeadCipher {
public:
    AeadCipher() noexcept;
    ~AeadCipher() noexcept;
    AeadCipher(const AeadCipher&) = delete;
    AeadCipher& operator=(const AeadCipher&) = delete;

    // Imports `key` for `cipher` -- kAesGcmKeySize octets for CipherId::AesGcm,
    // kChaCha20Poly1305KeySize for CipherId::ChaCha20Poly1305 -- discarding
    // whatever this object held before (including on failure: a partial
    // reset() never leaves the old key live). Every later seal() / open()
    // reuses this import until the next reset(). False on a wrong key size or
    // a backend failure -- valid() is then false and seal() / open() fail
    // closed with AeadError::InvalidArgument.
    bool reset(CipherId cipher, const AeadKey& key) noexcept;
    bool valid() const noexcept { return valid_; }
    CipherId cipher() const noexcept { return cipher_; }

    // Same wire contract as aead_seal_aes_gcm() / aead_seal_chacha20poly1305()
    // -- out_ciphertext_and_tag needs room for payload_size + 16 octets --
    // against whichever cipher reset() bound. header's CIPHER_ID sub-field
    // must name that same cipher (AeadError::InvalidCipherId otherwise); the
    // header still supplies this message's own nonce and AAD (source_id /
    // boot_id / sequence, the canonical logical header), reset() only ever
    // fixes the key. AeadError::InvalidArgument without a key imported yet.
    AeadError seal(const Header& header, std::uint16_t payload_size,
                   const std::uint8_t* plaintext,
                   std::uint8_t* out_ciphertext_and_tag) noexcept;

    // Same wire contract as aead_open_aes_gcm() / aead_open_chacha20poly1305()
    // -- ciphertext_size already includes the trailing 16-octet tag,
    // out_plaintext needs room for ciphertext_size - 16 octets.
    AeadError open(const Header& header, std::uint16_t ciphertext_size,
                  const std::uint8_t* ciphertext_and_tag,
                  std::uint8_t* out_plaintext) noexcept;

private:
    alignas(8) unsigned char storage_[kAeadCipherStorageSize];
    CipherId cipher_;
    bool valid_;
};

}  // namespace btp

#endif  // BTP_AEAD_HPP
