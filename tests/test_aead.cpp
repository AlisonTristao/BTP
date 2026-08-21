#include "btp/aead.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            std::cerr << __FILE__ << ':' << __LINE__                            \
                      << ": check failed: " #condition << '\n';                \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

void test_skeleton_functions_link_and_return_ok() {
    btp::Header header = {};
    header.type = btp::MessageType::Telemetry;
    header.flags = btp::kFlagEncrypted;
    header.source_id = 0x0C0D0E0FU;
    header.boot_id = 0x10203040U;
    header.sequence = 1U;
    header.timestamp_us = 1000U;
    header.object_id = 1U;
    header.fragment_index = 0U;
    header.fragment_count = 1U;

    std::uint8_t aes_key_bytes[btp::kAesGcmKeySize] = {};
    std::uint8_t chacha_key_bytes[btp::kChaCha20Poly1305KeySize] = {};
    const btp::AeadKey aes_key = {aes_key_bytes, sizeof(aes_key_bytes)};
    const btp::AeadKey chacha_key = {chacha_key_bytes, sizeof(chacha_key_bytes)};

    std::uint8_t plaintext[8] = {};
    std::uint8_t ciphertext_and_tag[sizeof(plaintext) + 16U] = {};
    std::uint8_t decrypted[sizeof(plaintext)] = {};

    CHECK(btp::aead_seal_aes_gcm(aes_key, header,
                                 static_cast<std::uint16_t>(sizeof(plaintext)),
                                 plaintext,
                                 ciphertext_and_tag) == btp::AeadError::Ok);
    CHECK(btp::aead_open_aes_gcm(
              aes_key, header,
              static_cast<std::uint16_t>(sizeof(ciphertext_and_tag)),
              ciphertext_and_tag, decrypted) == btp::AeadError::Ok);

    CHECK(btp::aead_seal_chacha20poly1305(
              chacha_key, header,
              static_cast<std::uint16_t>(sizeof(plaintext)), plaintext,
              ciphertext_and_tag) == btp::AeadError::Ok);
    CHECK(btp::aead_open_chacha20poly1305(
              chacha_key, header,
              static_cast<std::uint16_t>(sizeof(ciphertext_and_tag)),
              ciphertext_and_tag, decrypted) == btp::AeadError::Ok);

    CHECK(btp::aead_seal(aes_key, header,
                         static_cast<std::uint16_t>(sizeof(plaintext)),
                         plaintext, ciphertext_and_tag) == btp::AeadError::Ok);
    CHECK(btp::aead_open(aes_key, header,
                         static_cast<std::uint16_t>(sizeof(ciphertext_and_tag)),
                         ciphertext_and_tag, decrypted) == btp::AeadError::Ok);
}

btp::Header make_aes_gcm_header() {
    btp::Header header = {};
    header.type = btp::MessageType::Telemetry;
    header.flags = btp::kFlagEncrypted;  // CIPHER_ID bits (2-3) left at 00 == AES-128-GCM
    header.source_id = 0x0C0D0E0FU;
    header.boot_id = 0x10203040U;
    header.sequence = 7U;
    header.timestamp_us = 10000U;
    header.object_id = 1U;
    header.fragment_index = 0U;
    header.fragment_count = 1U;
    return header;
}

void test_aes_gcm_round_trip() {
    const std::uint8_t key_bytes[btp::kAesGcmKeySize] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    const btp::AeadKey key = {key_bytes, sizeof(key_bytes)};
    const btp::Header header = make_aes_gcm_header();

    const std::uint8_t plaintext[] = "BTP v2 AEAD round-trip test";
    const std::uint16_t plaintext_size = static_cast<std::uint16_t>(sizeof(plaintext));

    std::uint8_t sealed[sizeof(plaintext) + 16U] = {};
    std::uint8_t recovered[sizeof(plaintext)] = {};

    CHECK(btp::aead_seal_aes_gcm(key, header, plaintext_size, plaintext, sealed) ==
          btp::AeadError::Ok);
    CHECK(btp::aead_open_aes_gcm(
              key, header, static_cast<std::uint16_t>(sizeof(sealed)), sealed,
              recovered) == btp::AeadError::Ok);
    CHECK(std::memcmp(plaintext, recovered, sizeof(plaintext)) == 0);
}

void test_aes_gcm_tag_corruption_is_rejected() {
    const std::uint8_t key_bytes[btp::kAesGcmKeySize] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    const btp::AeadKey key = {key_bytes, sizeof(key_bytes)};
    const btp::Header header = make_aes_gcm_header();

    const std::uint8_t plaintext[] = "tamper the tag, not the data";
    const std::uint16_t plaintext_size = static_cast<std::uint16_t>(sizeof(plaintext));

    std::uint8_t sealed[sizeof(plaintext) + 16U] = {};
    std::uint8_t recovered[sizeof(plaintext)] = {};

    CHECK(btp::aead_seal_aes_gcm(key, header, plaintext_size, plaintext, sealed) ==
          btp::AeadError::Ok);

    sealed[sizeof(sealed) - 1U] ^= 0x01U;  // flip one bit in the last tag byte

    CHECK(btp::aead_open_aes_gcm(
              key, header, static_cast<std::uint16_t>(sizeof(sealed)), sealed,
              recovered) == btp::AeadError::TagMismatch);
}

void test_aes_gcm_header_tamper_is_rejected() {
    const std::uint8_t key_bytes[btp::kAesGcmKeySize] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    const btp::AeadKey key = {key_bytes, sizeof(key_bytes)};
    const btp::Header seal_header = make_aes_gcm_header();

    const std::uint8_t plaintext[] = "the header is the AAD, tamper it";
    const std::uint16_t plaintext_size = static_cast<std::uint16_t>(sizeof(plaintext));

    std::uint8_t sealed[sizeof(plaintext) + 16U] = {};
    std::uint8_t recovered[sizeof(plaintext)] = {};

    CHECK(btp::aead_seal_aes_gcm(key, seal_header, plaintext_size, plaintext, sealed) ==
          btp::AeadError::Ok);

    btp::Header open_header = seal_header;
    open_header.sequence = seal_header.sequence + 1U;  // one field differs from sealing

    CHECK(btp::aead_open_aes_gcm(
              key, open_header, static_cast<std::uint16_t>(sizeof(sealed)), sealed,
              recovered) == btp::AeadError::TagMismatch);
}

void test_aes_gcm_wrong_key_size_is_rejected() {
    const std::uint8_t key_bytes[btp::kChaCha20Poly1305KeySize] = {};
    const btp::AeadKey wrong_size_key = {key_bytes, sizeof(key_bytes)};
    const btp::Header header = make_aes_gcm_header();

    const std::uint8_t plaintext[8] = {};
    std::uint8_t sealed[sizeof(plaintext) + 16U] = {};

    CHECK(btp::aead_seal_aes_gcm(wrong_size_key, header,
                                 static_cast<std::uint16_t>(sizeof(plaintext)),
                                 plaintext, sealed) == btp::AeadError::InvalidArgument);
}

btp::Header make_chacha20poly1305_header() {
    btp::Header header = {};
    header.type = btp::MessageType::Telemetry;
    // CIPHER_ID bits (2-3) set to 01 == ChaCha20-Poly1305.
    header.flags = static_cast<std::uint16_t>(
        btp::kFlagEncrypted |
        (static_cast<std::uint16_t>(btp::CipherId::ChaCha20Poly1305) << btp::kCipherIdShift));
    header.source_id = 0x0C0D0E0FU;
    header.boot_id = 0x10203040U;
    header.sequence = 7U;
    header.timestamp_us = 10000U;
    header.object_id = 1U;
    header.fragment_index = 0U;
    header.fragment_count = 1U;
    return header;
}

void test_chacha20poly1305_round_trip() {
    const std::uint8_t key_bytes[btp::kChaCha20Poly1305KeySize] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
    const btp::AeadKey key = {key_bytes, sizeof(key_bytes)};
    const btp::Header header = make_chacha20poly1305_header();

    const std::uint8_t plaintext[] = "BTP v2 AEAD round-trip test";
    const std::uint16_t plaintext_size = static_cast<std::uint16_t>(sizeof(plaintext));

    std::uint8_t sealed[sizeof(plaintext) + 16U] = {};
    std::uint8_t recovered[sizeof(plaintext)] = {};

    CHECK(btp::aead_seal_chacha20poly1305(key, header, plaintext_size, plaintext, sealed) ==
          btp::AeadError::Ok);
    CHECK(btp::aead_open_chacha20poly1305(
              key, header, static_cast<std::uint16_t>(sizeof(sealed)), sealed,
              recovered) == btp::AeadError::Ok);
    CHECK(std::memcmp(plaintext, recovered, sizeof(plaintext)) == 0);
}

void test_chacha20poly1305_tag_corruption_is_rejected() {
    const std::uint8_t key_bytes[btp::kChaCha20Poly1305KeySize] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
    const btp::AeadKey key = {key_bytes, sizeof(key_bytes)};
    const btp::Header header = make_chacha20poly1305_header();

    const std::uint8_t plaintext[] = "tamper the tag, not the data";
    const std::uint16_t plaintext_size = static_cast<std::uint16_t>(sizeof(plaintext));

    std::uint8_t sealed[sizeof(plaintext) + 16U] = {};
    std::uint8_t recovered[sizeof(plaintext)] = {};

    CHECK(btp::aead_seal_chacha20poly1305(key, header, plaintext_size, plaintext, sealed) ==
          btp::AeadError::Ok);

    sealed[sizeof(sealed) - 1U] ^= 0x01U;  // flip one bit in the last tag byte

    CHECK(btp::aead_open_chacha20poly1305(
              key, header, static_cast<std::uint16_t>(sizeof(sealed)), sealed,
              recovered) == btp::AeadError::TagMismatch);
}

void test_chacha20poly1305_header_tamper_is_rejected() {
    const std::uint8_t key_bytes[btp::kChaCha20Poly1305KeySize] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
    const btp::AeadKey key = {key_bytes, sizeof(key_bytes)};
    const btp::Header seal_header = make_chacha20poly1305_header();

    const std::uint8_t plaintext[] = "the header is the AAD, tamper it";
    const std::uint16_t plaintext_size = static_cast<std::uint16_t>(sizeof(plaintext));

    std::uint8_t sealed[sizeof(plaintext) + 16U] = {};
    std::uint8_t recovered[sizeof(plaintext)] = {};

    CHECK(btp::aead_seal_chacha20poly1305(key, seal_header, plaintext_size, plaintext, sealed) ==
          btp::AeadError::Ok);

    btp::Header open_header = seal_header;
    open_header.sequence = seal_header.sequence + 1U;  // one field differs from sealing

    CHECK(btp::aead_open_chacha20poly1305(
              key, open_header, static_cast<std::uint16_t>(sizeof(sealed)), sealed,
              recovered) == btp::AeadError::TagMismatch);
}

void test_chacha20poly1305_wrong_key_size_is_rejected() {
    const std::uint8_t key_bytes[btp::kAesGcmKeySize] = {};
    const btp::AeadKey wrong_size_key = {key_bytes, sizeof(key_bytes)};
    const btp::Header header = make_chacha20poly1305_header();

    const std::uint8_t plaintext[8] = {};
    std::uint8_t sealed[sizeof(plaintext) + 16U] = {};

    CHECK(btp::aead_seal_chacha20poly1305(wrong_size_key, header,
                                          static_cast<std::uint16_t>(sizeof(plaintext)),
                                          plaintext, sealed) == btp::AeadError::InvalidArgument);
}

void test_dispatch_aead_seal_open_aes_gcm_round_trip() {
    const std::uint8_t key_bytes[btp::kAesGcmKeySize] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    const btp::AeadKey key = {key_bytes, sizeof(key_bytes)};
    const btp::Header header = make_aes_gcm_header();  // CIPHER_ID == AesGcm (0)

    const std::uint8_t plaintext[] = "dispatch through aead_seal/aead_open, AES-GCM path";
    const std::uint16_t plaintext_size = static_cast<std::uint16_t>(sizeof(plaintext));

    std::uint8_t sealed[sizeof(plaintext) + 16U] = {};
    std::uint8_t recovered[sizeof(plaintext)] = {};

    CHECK(btp::aead_seal(key, header, plaintext_size, plaintext, sealed) == btp::AeadError::Ok);
    CHECK(btp::aead_open(key, header, static_cast<std::uint16_t>(sizeof(sealed)), sealed,
                         recovered) == btp::AeadError::Ok);
    CHECK(std::memcmp(plaintext, recovered, sizeof(plaintext)) == 0);
}

void test_dispatch_aead_seal_open_chacha20poly1305_round_trip() {
    const std::uint8_t key_bytes[btp::kChaCha20Poly1305KeySize] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
    const btp::AeadKey key = {key_bytes, sizeof(key_bytes)};
    const btp::Header header = make_chacha20poly1305_header();  // CIPHER_ID == ChaCha20Poly1305 (1)

    const std::uint8_t plaintext[] = "dispatch through aead_seal/aead_open, ChaCha20 path";
    const std::uint16_t plaintext_size = static_cast<std::uint16_t>(sizeof(plaintext));

    std::uint8_t sealed[sizeof(plaintext) + 16U] = {};
    std::uint8_t recovered[sizeof(plaintext)] = {};

    CHECK(btp::aead_seal(key, header, plaintext_size, plaintext, sealed) == btp::AeadError::Ok);
    CHECK(btp::aead_open(key, header, static_cast<std::uint16_t>(sizeof(sealed)), sealed,
                         recovered) == btp::AeadError::Ok);
    CHECK(std::memcmp(plaintext, recovered, sizeof(plaintext)) == 0);
}

// Builds a Header whose CIPHER_ID sub-field (bits 2-3 of flags, mask 0x000C)
// holds a reserved raw value (2 or 3), which has no named CipherId
// enumerator (codec.hpp). aead_seal()/aead_open() must reject these before
// dispatching to either backend.
btp::Header make_reserved_cipher_id_header(std::uint16_t reserved_cipher_id_bits) {
    btp::Header header = {};
    header.type = btp::MessageType::Telemetry;
    header.flags = static_cast<std::uint16_t>(btp::kFlagEncrypted | reserved_cipher_id_bits);
    header.source_id = 0x0C0D0E0FU;
    header.boot_id = 0x10203040U;
    header.sequence = 7U;
    header.timestamp_us = 10000U;
    header.object_id = 1U;
    header.fragment_index = 0U;
    header.fragment_count = 1U;
    return header;
}

void test_dispatch_rejects_reserved_cipher_id() {
    const btp::Header header_2 = make_reserved_cipher_id_header(0x0008U);  // CIPHER_ID == 2 (reserved)
    const btp::Header header_3 = make_reserved_cipher_id_header(0x000CU);  // CIPHER_ID == 3 (reserved)

    // Deliberately null/mismatched key: if the dispatcher mistakenly called a
    // backend anyway, it would dereference a null pointer or fail on key
    // size, not return InvalidCipherId, so this also proves no backend ran.
    const btp::AeadKey no_key = {nullptr, 0U};

    const std::uint8_t plaintext[8] = {};
    std::uint8_t sealed[sizeof(plaintext) + 16U] = {};
    std::uint8_t recovered[sizeof(plaintext)] = {};

    CHECK(btp::aead_seal(no_key, header_2, static_cast<std::uint16_t>(sizeof(plaintext)),
                         plaintext, sealed) == btp::AeadError::InvalidCipherId);
    CHECK(btp::aead_open(no_key, header_2, static_cast<std::uint16_t>(sizeof(sealed)),
                         sealed, recovered) == btp::AeadError::InvalidCipherId);

    CHECK(btp::aead_seal(no_key, header_3, static_cast<std::uint16_t>(sizeof(plaintext)),
                         plaintext, sealed) == btp::AeadError::InvalidCipherId);
    CHECK(btp::aead_open(no_key, header_3, static_cast<std::uint16_t>(sizeof(sealed)),
                         sealed, recovered) == btp::AeadError::InvalidCipherId);
}

}  // namespace

int main() {
    test_skeleton_functions_link_and_return_ok();
    test_aes_gcm_round_trip();
    test_aes_gcm_tag_corruption_is_rejected();
    test_aes_gcm_header_tamper_is_rejected();
    test_aes_gcm_wrong_key_size_is_rejected();
    test_chacha20poly1305_round_trip();
    test_chacha20poly1305_tag_corruption_is_rejected();
    test_chacha20poly1305_header_tamper_is_rejected();
    test_chacha20poly1305_wrong_key_size_is_rejected();
    test_dispatch_aead_seal_open_aes_gcm_round_trip();
    test_dispatch_aead_seal_open_chacha20poly1305_round_trip();
    test_dispatch_rejects_reserved_cipher_id();

    if (failures != 0) {
        std::cerr << failures << " aead test(s) failed\n";
        return 1;
    }
    std::cout << "All BTP aead skeleton tests passed\n";
    return 0;
}
