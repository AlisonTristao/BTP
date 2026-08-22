#include "btp/aead.hpp"

// btp::aead is optional. Under CMake it is a separate target behind
// BTP_ENABLE_AEAD, so this file simply is not compiled when the option is off.
// PlatformIO has no such switch -- library.json's srcDir pulls in every .cpp
// unconditionally -- so on a platform whose SDK ships no mbedtls this would be
// a hard build error for a feature the user may not even want. Compile the
// implementation only when the backend headers are actually reachable, and let
// CMake, which knows it asked for them, fail loudly if they are not.
#if defined(__has_include)
#  if __has_include(<mbedtls/gcm.h>) && __has_include(<mbedtls/chachapoly.h>)
#    define BTP_AEAD_HAVE_MBEDTLS 1
#  endif
#endif

#if defined(BTP_AEAD_REQUIRE_MBEDTLS) && !defined(BTP_AEAD_HAVE_MBEDTLS)
#  error "btp::aead was requested but <mbedtls/gcm.h> was not found"
#endif

#if defined(BTP_AEAD_HAVE_MBEDTLS)

#include <mbedtls/chachapoly.h>
#include <mbedtls/gcm.h>

namespace btp {

namespace {

const std::size_t kTagSize = 16U;
const std::size_t kNonceSize = 12U;
const std::size_t kAadSize = 36U;

// The AAD is the header of the LOGICAL message, never of one fragment
// (docs/encryption.md section 5). The tag is computed once, before
// fragmenting, so the fragmentation fields cannot be part of what it
// authenticates: they differ per fragment, and the sealing side has no single
// value to put there.
// Canonicalizing them here -- FRAGMENTED cleared, index 0, count 1 -- makes
// both sides agree by construction instead of by convention, so aead_open()
// accepts the header decoded from any fragment of the message.
//
// It also keeps the tag independent of how the message got fragmented, which
// matters whenever a gateway relays between transports with different payload
// ceilings (kEspNowMaxPayloadSize vs kSerialMaxPayloadSize): it may
// re-fragment without holding the key.
//
// For an unfragmented message this is a no-op -- FRAGMENTED is already clear
// and the fields are already 0/1 -- so every existing conformance vector
// keeps producing byte-identical AAD.
Header aad_header(const Header& header) noexcept {
    Header canonical = header;
    canonical.flags = static_cast<std::uint16_t>(
        canonical.flags & static_cast<std::uint16_t>(~kFlagFragmented));
    canonical.fragment_index = 0U;
    canonical.fragment_count = 1U;
    return canonical;
}

// All four entry points validate the same arguments and build the same two
// buffers. Doing it once keeps the AAD construction -- the subtle part, and
// the one both peers must agree on byte for byte -- from being spelled out
// four times and drifting in one of them.
//
// wire_payload_size is what the AAD records: the size of the whole logical
// payload as it appears on the wire, ciphertext plus tag. A sealer therefore
// passes plaintext_size + kTagSize, while an opener passes ciphertext_size
// unchanged because it already includes the tag.
//
// Pointer rules follow the rest of the library: an input may be null only when
// its size is zero, and an output buffer is always required.
AeadError prepare(const AeadKey& key,
                  std::size_t expected_key_size,
                  const Header& header,
                  std::uint16_t wire_payload_size,
                  const std::uint8_t* input,
                  std::size_t input_size,
                  const std::uint8_t* output,
                  std::uint8_t out_aad[kAadSize],
                  std::uint8_t out_nonce[kNonceSize]) noexcept {
    if (key.data == nullptr || key.size != expected_key_size) {
        return AeadError::InvalidArgument;
    }
    if (input == nullptr && input_size != 0U) {
        return AeadError::InvalidArgument;
    }
    if (output == nullptr) {
        return AeadError::InvalidArgument;
    }
    if (encode_header(aad_header(header), wire_payload_size, out_aad) !=
        Error::Ok) {
        return AeadError::InvalidArgument;
    }
    aead_nonce(header, out_nonce);
    return AeadError::Ok;
}

// Guards the plaintext_size + kTagSize sum a sealer has to compute: the size
// is already a uint16, so a caller near the top of the range would wrap it
// silently and authenticate a size no receiver can reproduce.
bool seal_size_fits(std::uint16_t payload_size) noexcept {
    return payload_size <= static_cast<std::uint16_t>(0xFFFFU - kTagSize);
}

}  // namespace

AeadError aead_seal_aes_gcm(const AeadKey& key, const Header& header,
                            std::uint16_t payload_size,
                            const std::uint8_t* plaintext,
                            std::uint8_t* out_ciphertext_and_tag) noexcept {
    if (!seal_size_fits(payload_size)) {
        return AeadError::InvalidArgument;
    }

    std::uint8_t aad[kAadSize];
    std::uint8_t nonce[kNonceSize];
    const AeadError prepared = prepare(
        key, kAesGcmKeySize, header,
        static_cast<std::uint16_t>(payload_size + kTagSize), plaintext,
        payload_size, out_ciphertext_and_tag, aad, nonce);
    if (prepared != AeadError::Ok) {
        return prepared;
    }

    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);

    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key.data,
                                static_cast<unsigned int>(key.size * 8U));
    if (rc == 0) {
        rc = mbedtls_gcm_crypt_and_tag(
            &ctx, MBEDTLS_GCM_ENCRYPT, payload_size, nonce, sizeof(nonce),
            aad, sizeof(aad), plaintext, out_ciphertext_and_tag, kTagSize,
            out_ciphertext_and_tag + payload_size);
    }

    mbedtls_gcm_free(&ctx);
    return rc == 0 ? AeadError::Ok : AeadError::InvalidArgument;
}

AeadError aead_open_aes_gcm(const AeadKey& key, const Header& header,
                            std::uint16_t ciphertext_size,
                            const std::uint8_t* ciphertext_and_tag,
                            std::uint8_t* out_plaintext) noexcept {
    if (ciphertext_size < kTagSize) {
        return AeadError::InvalidArgument;
    }

    std::uint8_t aad[kAadSize];
    std::uint8_t nonce[kNonceSize];
    const AeadError prepared =
        prepare(key, kAesGcmKeySize, header, ciphertext_size,
                ciphertext_and_tag, ciphertext_size, out_plaintext, aad, nonce);
    if (prepared != AeadError::Ok) {
        return prepared;
    }

    const std::size_t plaintext_size =
        static_cast<std::size_t>(ciphertext_size) - kTagSize;

    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);

    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key.data,
                                static_cast<unsigned int>(key.size * 8U));
    if (rc == 0) {
        rc = mbedtls_gcm_auth_decrypt(
            &ctx, plaintext_size, nonce, sizeof(nonce), aad, sizeof(aad),
            ciphertext_and_tag + plaintext_size, kTagSize, ciphertext_and_tag,
            out_plaintext);
    }

    mbedtls_gcm_free(&ctx);

    if (rc == 0) {
        return AeadError::Ok;
    }
    if (rc == MBEDTLS_ERR_GCM_AUTH_FAILED) {
        return AeadError::TagMismatch;
    }
    return AeadError::InvalidArgument;
}

AeadError aead_seal_chacha20poly1305(
    const AeadKey& key, const Header& header, std::uint16_t payload_size,
    const std::uint8_t* plaintext,
    std::uint8_t* out_ciphertext_and_tag) noexcept {
    if (!seal_size_fits(payload_size)) {
        return AeadError::InvalidArgument;
    }

    std::uint8_t aad[kAadSize];
    std::uint8_t nonce[kNonceSize];
    const AeadError prepared = prepare(
        key, kChaCha20Poly1305KeySize, header,
        static_cast<std::uint16_t>(payload_size + kTagSize), plaintext,
        payload_size, out_ciphertext_and_tag, aad, nonce);
    if (prepared != AeadError::Ok) {
        return prepared;
    }

    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);

    int rc = mbedtls_chachapoly_setkey(&ctx, key.data);
    if (rc == 0) {
        rc = mbedtls_chachapoly_encrypt_and_tag(
            &ctx, payload_size, nonce, aad, sizeof(aad), plaintext,
            out_ciphertext_and_tag, out_ciphertext_and_tag + payload_size);
    }

    mbedtls_chachapoly_free(&ctx);
    return rc == 0 ? AeadError::Ok : AeadError::InvalidArgument;
}

AeadError aead_open_chacha20poly1305(const AeadKey& key, const Header& header,
                                     std::uint16_t ciphertext_size,
                                     const std::uint8_t* ciphertext_and_tag,
                                     std::uint8_t* out_plaintext) noexcept {
    if (ciphertext_size < kTagSize) {
        return AeadError::InvalidArgument;
    }

    std::uint8_t aad[kAadSize];
    std::uint8_t nonce[kNonceSize];
    const AeadError prepared =
        prepare(key, kChaCha20Poly1305KeySize, header, ciphertext_size,
                ciphertext_and_tag, ciphertext_size, out_plaintext, aad, nonce);
    if (prepared != AeadError::Ok) {
        return prepared;
    }

    const std::size_t plaintext_size =
        static_cast<std::size_t>(ciphertext_size) - kTagSize;

    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);

    int rc = mbedtls_chachapoly_setkey(&ctx, key.data);
    if (rc == 0) {
        rc = mbedtls_chachapoly_auth_decrypt(
            &ctx, plaintext_size, nonce, aad, sizeof(aad),
            ciphertext_and_tag + plaintext_size, ciphertext_and_tag,
            out_plaintext);
    }

    mbedtls_chachapoly_free(&ctx);

    if (rc == 0) {
        return AeadError::Ok;
    }
    if (rc == MBEDTLS_ERR_CHACHAPOLY_AUTH_FAILED) {
        return AeadError::TagMismatch;
    }
    return AeadError::InvalidArgument;
}

AeadError aead_seal(const AeadKey& key, const Header& header,
                    std::uint16_t payload_size, const std::uint8_t* plaintext,
                    std::uint8_t* out_ciphertext_and_tag) noexcept {
    switch (cipher_id(header.flags)) {
        case CipherId::AesGcm:
            return aead_seal_aes_gcm(key, header, payload_size, plaintext,
                                     out_ciphertext_and_tag);
        case CipherId::ChaCha20Poly1305:
            return aead_seal_chacha20poly1305(key, header, payload_size,
                                              plaintext,
                                              out_ciphertext_and_tag);
    }
    return AeadError::InvalidCipherId;
}

AeadError aead_open(const AeadKey& key, const Header& header,
                    std::uint16_t ciphertext_size,
                    const std::uint8_t* ciphertext_and_tag,
                    std::uint8_t* out_plaintext) noexcept {
    switch (cipher_id(header.flags)) {
        case CipherId::AesGcm:
            return aead_open_aes_gcm(key, header, ciphertext_size,
                                     ciphertext_and_tag, out_plaintext);
        case CipherId::ChaCha20Poly1305:
            return aead_open_chacha20poly1305(key, header, ciphertext_size,
                                              ciphertext_and_tag,
                                              out_plaintext);
    }
    return AeadError::InvalidCipherId;
}

}  // namespace btp

#endif  // BTP_AEAD_HAVE_MBEDTLS
