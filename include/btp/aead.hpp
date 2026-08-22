#ifndef BTP_AEAD_HPP
#define BTP_AEAD_HPP

#include "btp/codec.hpp"

#include <cstddef>
#include <cstdint>

namespace btp {

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

}  // namespace btp

#endif  // BTP_AEAD_HPP
