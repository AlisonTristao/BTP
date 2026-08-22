#include "btp/aead.hpp"

#include <mbedtls/chachapoly.h>
#include <mbedtls/gcm.h>

namespace btp {

namespace {

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
// matters because the dongle relays between transports with different payload
// ceilings (BTP_ESPNOW_MAX_PAYLOAD_SIZE vs BTP_USB_HID_MAX_PAYLOAD_SIZE): a
// gateway may re-fragment without holding the key.
//
// For an unfragmented message this is a no-op -- FRAGMENTED is already clear
// and the fields are already 0/1 -- so every existing conformance vector
// keeps producing byte-identical AAD.
Header aad_header(const Header& header) noexcept {
    Header canonical = header;
    canonical.flags =
        static_cast<std::uint16_t>(canonical.flags & static_cast<std::uint16_t>(~kFlagFragmented));
    canonical.fragment_index = 0U;
    canonical.fragment_count = 1U;
    return canonical;
}

}  // namespace

AeadError aead_seal_aes_gcm(const AeadKey& key, const Header& header,
                            std::uint16_t payload_size,
                            const std::uint8_t* plaintext,
                            std::uint8_t* out_ciphertext_and_tag) noexcept {
    if (key.data == nullptr || key.size != kAesGcmKeySize) {
        return AeadError::InvalidArgument;
    }

    // The AAD records the WIRE payload size of the logical message --
    // ciphertext plus the 16-octet tag (docs/encryption.md sections 2 and 5),
    // not the plaintext size received here. Guard the sum: payload_size is
    // already a uint16, so a caller near the top of the range would wrap it
    // silently and authenticate a size that no receiver can reproduce.
    if (payload_size > static_cast<std::uint16_t>(0xFFFFU - 16U)) {
        return AeadError::InvalidArgument;
    }
    std::uint8_t aad[36];
    const std::uint16_t wire_payload_size = static_cast<std::uint16_t>(payload_size + 16U);
    if (encode_header(aad_header(header), wire_payload_size, aad) != Error::Ok) {
        return AeadError::InvalidArgument;
    }

    std::uint8_t nonce[12];
    aead_nonce(header, nonce);

    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);

    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key.data,
                                static_cast<unsigned int>(key.size * 8U));
    if (rc == 0) {
        rc = mbedtls_gcm_crypt_and_tag(
            &ctx, MBEDTLS_GCM_ENCRYPT, payload_size, nonce, sizeof(nonce),
            aad, sizeof(aad), plaintext, out_ciphertext_and_tag, 16U,
            out_ciphertext_and_tag + payload_size);
    }

    mbedtls_gcm_free(&ctx);
    return rc == 0 ? AeadError::Ok : AeadError::InvalidArgument;
}

AeadError aead_open_aes_gcm(const AeadKey& key, const Header& header,
                            std::uint16_t ciphertext_size,
                            const std::uint8_t* ciphertext_and_tag,
                            std::uint8_t* out_plaintext) noexcept {
    if (key.data == nullptr || key.size != kAesGcmKeySize) {
        return AeadError::InvalidArgument;
    }
    if (ciphertext_size < 16U) {
        return AeadError::InvalidArgument;
    }

    // ciphertext_size already includes the trailing tag, so it IS the wire
    // payload size that went into the AAD at seal time -- no +16 here,
    // unlike aead_seal_aes_gcm() above.
    std::uint8_t aad[36];
    if (encode_header(aad_header(header), ciphertext_size, aad) != Error::Ok) {
        return AeadError::InvalidArgument;
    }

    std::uint8_t nonce[12];
    aead_nonce(header, nonce);

    const std::size_t plaintext_size = static_cast<std::size_t>(ciphertext_size) - 16U;

    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);

    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key.data,
                                static_cast<unsigned int>(key.size * 8U));
    if (rc == 0) {
        rc = mbedtls_gcm_auth_decrypt(
            &ctx, plaintext_size, nonce, sizeof(nonce), aad, sizeof(aad),
            ciphertext_and_tag + plaintext_size, 16U, ciphertext_and_tag,
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

AeadError aead_seal_chacha20poly1305(const AeadKey& key, const Header& header,
                                     std::uint16_t payload_size,
                                     const std::uint8_t* plaintext,
                                     std::uint8_t* out_ciphertext_and_tag) noexcept {
    if (key.data == nullptr || key.size != kChaCha20Poly1305KeySize) {
        return AeadError::InvalidArgument;
    }

    // The AAD records the WIRE payload size of the logical message --
    // ciphertext plus the 16-octet tag (docs/encryption.md sections 2 and 5),
    // not the plaintext size received here. Guard the sum: payload_size is
    // already a uint16, so a caller near the top of the range would wrap it
    // silently and authenticate a size that no receiver can reproduce.
    if (payload_size > static_cast<std::uint16_t>(0xFFFFU - 16U)) {
        return AeadError::InvalidArgument;
    }
    std::uint8_t aad[36];
    const std::uint16_t wire_payload_size = static_cast<std::uint16_t>(payload_size + 16U);
    if (encode_header(aad_header(header), wire_payload_size, aad) != Error::Ok) {
        return AeadError::InvalidArgument;
    }

    std::uint8_t nonce[12];
    aead_nonce(header, nonce);

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
    if (key.data == nullptr || key.size != kChaCha20Poly1305KeySize) {
        return AeadError::InvalidArgument;
    }
    if (ciphertext_size < 16U) {
        return AeadError::InvalidArgument;
    }

    // ciphertext_size already includes the trailing tag, so it IS the wire
    // payload size that went into the AAD at seal time -- no +16 here,
    // unlike aead_seal_chacha20poly1305() above.
    std::uint8_t aad[36];
    if (encode_header(aad_header(header), ciphertext_size, aad) != Error::Ok) {
        return AeadError::InvalidArgument;
    }

    std::uint8_t nonce[12];
    aead_nonce(header, nonce);

    const std::size_t plaintext_size = static_cast<std::size_t>(ciphertext_size) - 16U;

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
            return aead_seal_aes_gcm(key, header, payload_size, plaintext, out_ciphertext_and_tag);
        case CipherId::ChaCha20Poly1305:
            return aead_seal_chacha20poly1305(key, header, payload_size, plaintext, out_ciphertext_and_tag);
    }
    return AeadError::InvalidCipherId;
}

AeadError aead_open(const AeadKey& key, const Header& header,
                    std::uint16_t ciphertext_size,
                    const std::uint8_t* ciphertext_and_tag,
                    std::uint8_t* out_plaintext) noexcept {
    switch (cipher_id(header.flags)) {
        case CipherId::AesGcm:
            return aead_open_aes_gcm(key, header, ciphertext_size, ciphertext_and_tag, out_plaintext);
        case CipherId::ChaCha20Poly1305:
            return aead_open_chacha20poly1305(key, header, ciphertext_size, ciphertext_and_tag, out_plaintext);
    }
    return AeadError::InvalidCipherId;
}

}  // namespace btp
