#include "btp/aead.hpp"
#include "btp/fragmentation.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

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

// A fragmented message is sealed once, before fragmenting, and opened once,
// after reassembly -- but the header the receiver has in hand came off one
// fragment, with that fragment's FRAGMENTED bit, index and count. The AAD is
// the logical message's header (docs/encryption.md section 5), so none of those three
// fields may reach it: if they did, every fragmented message would fail to
// authenticate, and a gateway re-fragmenting between transports with different
// payload ceilings would break the tag it cannot recompute.
void test_aad_ignores_fragmentation_fields() {
    const std::uint8_t key_bytes[btp::kAesGcmKeySize] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    const btp::AeadKey key = {key_bytes, sizeof(key_bytes)};

    const btp::Header logical = make_aes_gcm_header();

    btp::Header fragment = logical;
    fragment.flags = static_cast<std::uint16_t>(fragment.flags | btp::kFlagFragmented);
    fragment.fragment_index = 2U;
    fragment.fragment_count = 3U;

    const std::uint8_t plaintext[] = "sealed once, reassembled from three fragments";
    const std::uint16_t plaintext_size = static_cast<std::uint16_t>(sizeof(plaintext));

    std::uint8_t sealed_logical[sizeof(plaintext) + 16U] = {};
    std::uint8_t sealed_fragment[sizeof(plaintext) + 16U] = {};
    std::uint8_t recovered[sizeof(plaintext)] = {};

    CHECK(btp::aead_seal(key, logical, plaintext_size, plaintext, sealed_logical) ==
          btp::AeadError::Ok);
    CHECK(btp::aead_seal(key, fragment, plaintext_size, plaintext, sealed_fragment) ==
          btp::AeadError::Ok);

    // Same ciphertext AND same tag: the fragmentation fields changed nothing.
    CHECK(std::memcmp(sealed_logical, sealed_fragment, sizeof(sealed_logical)) == 0);

    // Opening with a fragment's header authenticates what was sealed with the
    // logical one -- the case a receiver actually hits after reassembly.
    CHECK(btp::aead_open(key, fragment, static_cast<std::uint16_t>(sizeof(sealed_logical)),
                         sealed_logical, recovered) == btp::AeadError::Ok);
    CHECK(std::memcmp(recovered, plaintext, sizeof(plaintext)) == 0);

    // Every other header field stays authenticated: only the three
    // fragmentation fields are canonicalized away.
    btp::Header tampered = fragment;
    tampered.object_id = static_cast<std::uint16_t>(fragment.object_id + 1U);
    CHECK(btp::aead_open(key, tampered, static_cast<std::uint16_t>(sizeof(sealed_logical)),
                         sealed_logical, recovered) == btp::AeadError::TagMismatch);
}

// The end-to-end claim behind the AAD canonicalization
// (docs/encryption.md section 5): the tag
// is computed once over the logical message, so a message sealed whole, cut
// into transport-sized fragments and reassembled on the far side still opens.
// Nothing fragmentation touches -- per-fragment size, index, count, per-frame
// CRC -- enters the tag, which is what lets a gateway re-fragment an encrypted
// message across transports without holding the key.
void test_sealed_message_survives_fragmentation_and_reassembly() {
    const std::uint16_t kPlaintextSize = 300U;  // forces 2 EspNow fragments

    btp::Header logical = {};
    logical.type = btp::MessageType::Telemetry;
    logical.flags = btp::kFlagEncrypted;
    logical.source_id = 0x0C0D0E0FU;
    logical.boot_id = 0x10203040U;
    logical.sequence = 7U;
    logical.timestamp_us = 987654U;
    logical.object_id = 42U;
    logical.fragment_index = 0U;
    logical.fragment_count = 1U;

    std::uint8_t key_bytes[btp::kAesGcmKeySize];
    for (std::size_t i = 0U; i < sizeof(key_bytes); ++i) {
        key_bytes[i] = static_cast<std::uint8_t>(0xA0U + i);
    }
    const btp::AeadKey key = {key_bytes, sizeof(key_bytes)};

    std::vector<std::uint8_t> plaintext(kPlaintextSize);
    for (std::size_t i = 0U; i < plaintext.size(); ++i) {
        plaintext[i] = static_cast<std::uint8_t>((i * 7U) & 0xFFU);
    }

    std::vector<std::uint8_t> sealed(kPlaintextSize + 16U);
    CHECK(btp::aead_seal(key, logical, kPlaintextSize, plaintext.data(),
                         sealed.data()) == btp::AeadError::Ok);

    std::uint8_t count = 0U;
    CHECK(btp::fragment_count(sealed.size(), btp::kEspNowTransport,
                              &count) == btp::Error::Ok);
    CHECK(count == 2U);

    btp::ReassemblySlot slots[1];
    std::vector<std::uint8_t> storage(sealed.size());
    const btp::ReassemblyStorage storage_view = {storage.data(),
                                                 storage.size()};
    btp::Reassembler reassembler(slots, &storage_view, 1U, 1000U);
    CHECK(reassembler.valid());

    // Push the fragments out of order: reassembly must not depend on arrival
    // order, and neither must the tag.
    btp::ReassembledMessage message = {};
    btp::ReassemblyEvent last = btp::ReassemblyEvent::InvalidArgument;
    const std::uint8_t order[2] = {1U, 0U};
    for (std::size_t step = 0U; step < 2U; ++step) {
        btp::Frame fragment = {};
        CHECK(btp::make_fragment(logical, {sealed.data(), sealed.size()},
                                 btp::kEspNowTransport, order[step],
                                 &fragment) == btp::Error::Ok);
        last = reassembler.push(fragment, 5U, &message);
    }
    CHECK(last == btp::ReassemblyEvent::Complete);
    CHECK(message.payload.size == sealed.size());

    std::vector<std::uint8_t> opened(kPlaintextSize);
    CHECK(btp::aead_open(key, message.header,
                         static_cast<std::uint16_t>(message.payload.size),
                         message.payload.data,
                         opened.data()) == btp::AeadError::Ok);
    CHECK(std::memcmp(opened.data(), plaintext.data(), plaintext.size()) == 0);
}

// ---------------------------------------------------------------------------
// AeadCipher -- one reset(), many seal()/open() calls against the same key
// ---------------------------------------------------------------------------

void test_aead_cipher_aes_gcm_round_trip_reuses_key() {
    const std::uint8_t key_bytes[btp::kAesGcmKeySize] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    const btp::AeadKey key = {key_bytes, sizeof(key_bytes)};

    btp::AeadCipher cipher;
    CHECK(cipher.reset(btp::CipherId::AesGcm, key));
    CHECK(cipher.valid());
    CHECK(cipher.cipher() == btp::CipherId::AesGcm);

    // Several messages under the ONE import -- the case reset() exists for.
    // Only `sequence` (part of the nonce) changes between them.
    for (std::uint32_t sequence = 1U; sequence <= 5U; ++sequence) {
        btp::Header header = make_aes_gcm_header();
        header.sequence = sequence;

        const std::uint8_t plaintext[] = "AeadCipher reused across many frames";
        const std::uint16_t plaintext_size =
            static_cast<std::uint16_t>(sizeof(plaintext));
        std::uint8_t sealed[sizeof(plaintext) + 16U] = {};
        std::uint8_t recovered[sizeof(plaintext)] = {};

        CHECK(cipher.seal(header, plaintext_size, plaintext, sealed) ==
              btp::AeadError::Ok);
        CHECK(cipher.open(header, static_cast<std::uint16_t>(sizeof(sealed)),
                          sealed, recovered) == btp::AeadError::Ok);
        CHECK(std::memcmp(plaintext, recovered, sizeof(plaintext)) == 0);
    }
}

void test_aead_cipher_chacha20poly1305_round_trip_reuses_key() {
    const std::uint8_t key_bytes[btp::kChaCha20Poly1305KeySize] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
    const btp::AeadKey key = {key_bytes, sizeof(key_bytes)};

    btp::AeadCipher cipher;
    CHECK(cipher.reset(btp::CipherId::ChaCha20Poly1305, key));

    for (std::uint32_t sequence = 1U; sequence <= 5U; ++sequence) {
        btp::Header header = make_chacha20poly1305_header();
        header.sequence = sequence;

        const std::uint8_t plaintext[] = "same key, five different nonces";
        const std::uint16_t plaintext_size =
            static_cast<std::uint16_t>(sizeof(plaintext));
        std::uint8_t sealed[sizeof(plaintext) + 16U] = {};
        std::uint8_t recovered[sizeof(plaintext)] = {};

        CHECK(cipher.seal(header, plaintext_size, plaintext, sealed) ==
              btp::AeadError::Ok);
        CHECK(cipher.open(header, static_cast<std::uint16_t>(sizeof(sealed)),
                          sealed, recovered) == btp::AeadError::Ok);
        CHECK(std::memcmp(plaintext, recovered, sizeof(plaintext)) == 0);
    }
}

// AeadCipher must be byte-for-byte interchangeable with the stateless
// functions -- a peer built against one has to interoperate with a peer built
// against the other, the same promise the two backends already make each
// other (aead.hpp's own top comment).
void test_aead_cipher_matches_stateless_functions() {
    const std::uint8_t key_bytes[btp::kAesGcmKeySize] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    const btp::AeadKey key = {key_bytes, sizeof(key_bytes)};
    const btp::Header header = make_aes_gcm_header();
    const std::uint8_t plaintext[] = "one key, two call styles, one ciphertext";
    const std::uint16_t plaintext_size = static_cast<std::uint16_t>(sizeof(plaintext));

    std::uint8_t sealed_stateless[sizeof(plaintext) + 16U] = {};
    CHECK(btp::aead_seal_aes_gcm(key, header, plaintext_size, plaintext,
                                 sealed_stateless) == btp::AeadError::Ok);

    btp::AeadCipher cipher;
    CHECK(cipher.reset(btp::CipherId::AesGcm, key));
    std::uint8_t sealed_cached[sizeof(plaintext) + 16U] = {};
    CHECK(cipher.seal(header, plaintext_size, plaintext, sealed_cached) ==
          btp::AeadError::Ok);

    CHECK(std::memcmp(sealed_stateless, sealed_cached, sizeof(sealed_stateless)) == 0);

    // Cross-open: AeadCipher opens what the stateless function sealed, and
    // vice versa.
    std::uint8_t opened_by_cipher[sizeof(plaintext)] = {};
    CHECK(cipher.open(header, static_cast<std::uint16_t>(sizeof(sealed_stateless)),
                      sealed_stateless, opened_by_cipher) == btp::AeadError::Ok);
    CHECK(std::memcmp(plaintext, opened_by_cipher, sizeof(plaintext)) == 0);

    std::uint8_t opened_by_stateless[sizeof(plaintext)] = {};
    CHECK(btp::aead_open_aes_gcm(
              key, header, static_cast<std::uint16_t>(sizeof(sealed_cached)),
              sealed_cached, opened_by_stateless) == btp::AeadError::Ok);
    CHECK(std::memcmp(plaintext, opened_by_stateless, sizeof(plaintext)) == 0);
}

void test_aead_cipher_tag_corruption_is_rejected() {
    const std::uint8_t key_bytes[btp::kAesGcmKeySize] = {};
    const btp::AeadKey key = {key_bytes, sizeof(key_bytes)};
    const btp::Header header = make_aes_gcm_header();
    const std::uint8_t plaintext[] = "tamper the tag, not the data";
    const std::uint16_t plaintext_size = static_cast<std::uint16_t>(sizeof(plaintext));

    btp::AeadCipher cipher;
    CHECK(cipher.reset(btp::CipherId::AesGcm, key));
    std::uint8_t sealed[sizeof(plaintext) + 16U] = {};
    CHECK(cipher.seal(header, plaintext_size, plaintext, sealed) == btp::AeadError::Ok);

    sealed[sizeof(sealed) - 1U] ^= 0x01U;
    std::uint8_t recovered[sizeof(plaintext)] = {};
    CHECK(cipher.open(header, static_cast<std::uint16_t>(sizeof(sealed)), sealed,
                      recovered) == btp::AeadError::TagMismatch);
}

// header claims CIPHER_ID == ChaCha20-Poly1305 against a cipher reset() bound
// to AES-GCM -- a caller mistake this must not silently seal/open as AES-GCM
// anyway (the wire flags would then disagree with what actually ran).
void test_aead_cipher_rejects_cipher_mismatch() {
    const std::uint8_t key_bytes[btp::kAesGcmKeySize] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    const btp::AeadKey key = {key_bytes, sizeof(key_bytes)};

    btp::AeadCipher cipher;
    CHECK(cipher.reset(btp::CipherId::AesGcm, key));

    const btp::Header mismatched_header = make_chacha20poly1305_header();
    const std::uint8_t plaintext[8] = {};
    std::uint8_t sealed[sizeof(plaintext) + 16U] = {};
    CHECK(cipher.seal(mismatched_header, sizeof(plaintext), plaintext, sealed) ==
          btp::AeadError::InvalidCipherId);
}

// A wrong key size leaves the object exactly as unusable as before reset()
// was ever called -- not holding a half-imported key.
void test_aead_cipher_wrong_key_size_leaves_invalid() {
    const std::uint8_t key_bytes[btp::kChaCha20Poly1305KeySize] = {};
    const btp::AeadKey wrong_size_key = {key_bytes, sizeof(key_bytes)};

    btp::AeadCipher cipher;
    CHECK(!cipher.reset(btp::CipherId::AesGcm, wrong_size_key));
    CHECK(!cipher.valid());

    const btp::Header header = make_aes_gcm_header();
    const std::uint8_t plaintext[8] = {};
    std::uint8_t sealed[sizeof(plaintext) + 16U] = {};
    CHECK(cipher.seal(header, sizeof(plaintext), plaintext, sealed) ==
          btp::AeadError::InvalidArgument);
}

// reset() on an object that already holds a key must free that key before
// importing the new one -- not leak it. AES-GCM then ChaCha20-Poly1305 also
// exercises the classic backend switching which context TYPE lives in
// storage_ (mbedtls_gcm_context -> mbedtls_chachapoly_context) and, on PSA,
// that the first psa_destroy_key() actually ran (a leaked key id would
// exhaust PSA's fixed-size keystore well before this loop finishes on a
// build small enough for that table to matter).
void test_aead_cipher_reset_does_not_leak() {
    const std::uint8_t aes_key_bytes[btp::kAesGcmKeySize] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    const std::uint8_t chacha_key_bytes[btp::kChaCha20Poly1305KeySize] = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f};
    const btp::AeadKey aes_key = {aes_key_bytes, sizeof(aes_key_bytes)};
    const btp::AeadKey chacha_key = {chacha_key_bytes, sizeof(chacha_key_bytes)};

    btp::AeadCipher cipher;
    for (int i = 0; i < 64; ++i) {
        CHECK(cipher.reset(btp::CipherId::AesGcm, aes_key));
        CHECK(cipher.reset(btp::CipherId::ChaCha20Poly1305, chacha_key));
    }
    CHECK(cipher.valid());
    CHECK(cipher.cipher() == btp::CipherId::ChaCha20Poly1305);

    // Still fully usable after 128 resets, on whichever cipher it landed on.
    const btp::Header header = make_chacha20poly1305_header();
    const std::uint8_t plaintext[] = "still good after many resets";
    const std::uint16_t plaintext_size = static_cast<std::uint16_t>(sizeof(plaintext));
    std::uint8_t sealed[sizeof(plaintext) + 16U] = {};
    std::uint8_t recovered[sizeof(plaintext)] = {};
    CHECK(cipher.seal(header, plaintext_size, plaintext, sealed) == btp::AeadError::Ok);
    CHECK(cipher.open(header, static_cast<std::uint16_t>(sizeof(sealed)), sealed,
                      recovered) == btp::AeadError::Ok);
    CHECK(std::memcmp(plaintext, recovered, sizeof(plaintext)) == 0);
}

// The rest of the library rejects a null pointer rather than handing it to
// something that will dereference it; the AEAD entry points used to check only
// key.data and would have passed a null plaintext straight into mbedtls.
void test_null_pointers_are_rejected() {
    btp::Header header = {};
    header.type = btp::MessageType::Telemetry;
    header.flags = btp::kFlagEncrypted;
    header.source_id = 1U;
    header.boot_id = 1U;
    header.sequence = 1U;
    header.fragment_count = 1U;

    std::uint8_t key_bytes[btp::kAesGcmKeySize] = {};
    const btp::AeadKey key = {key_bytes, sizeof(key_bytes)};
    std::uint8_t plaintext[8] = {};
    std::uint8_t sealed[sizeof(plaintext) + 16U] = {};
    std::uint8_t opened[sizeof(plaintext)] = {};

    // A null input with a non-zero size, and a null output, on all four
    // entry points plus the two dispatchers.
    CHECK(btp::aead_seal_aes_gcm(key, header, sizeof(plaintext), nullptr,
                                 sealed) == btp::AeadError::InvalidArgument);
    CHECK(btp::aead_seal_aes_gcm(key, header, sizeof(plaintext), plaintext,
                                 nullptr) == btp::AeadError::InvalidArgument);
    CHECK(btp::aead_open_aes_gcm(key, header, sizeof(sealed), nullptr,
                                 opened) == btp::AeadError::InvalidArgument);
    CHECK(btp::aead_open_aes_gcm(key, header, sizeof(sealed), sealed,
                                 nullptr) == btp::AeadError::InvalidArgument);

    std::uint8_t chacha_key_bytes[btp::kChaCha20Poly1305KeySize] = {};
    const btp::AeadKey chacha_key = {chacha_key_bytes,
                                     sizeof(chacha_key_bytes)};
    btp::Header chacha_header = header;
    chacha_header.flags = static_cast<std::uint16_t>(
        btp::kFlagEncrypted | (1U << btp::kCipherIdShift));

    CHECK(btp::aead_seal_chacha20poly1305(
              chacha_key, chacha_header, sizeof(plaintext), nullptr,
              sealed) == btp::AeadError::InvalidArgument);
    CHECK(btp::aead_open_chacha20poly1305(
              chacha_key, chacha_header, sizeof(sealed), sealed,
              nullptr) == btp::AeadError::InvalidArgument);

    CHECK(btp::aead_seal(key, header, sizeof(plaintext), nullptr, sealed) ==
          btp::AeadError::InvalidArgument);
    CHECK(btp::aead_open(key, header, sizeof(sealed), sealed, nullptr) ==
          btp::AeadError::InvalidArgument);

    // A null input IS allowed when the size is zero, matching how the codec
    // treats an empty payload. Only the output stays mandatory.
    std::uint8_t tag_only[16] = {};
    CHECK(btp::aead_seal_aes_gcm(key, header, 0U, nullptr, tag_only) ==
          btp::AeadError::Ok);
    std::uint8_t empty_opened[1] = {};
    CHECK(btp::aead_open_aes_gcm(key, header, sizeof(tag_only), tag_only,
                                 empty_opened) == btp::AeadError::Ok);

    // The same zero-length contract applies to the other supported AEAD.
    std::uint8_t chacha_tag_only[16] = {};
    CHECK(btp::aead_seal_chacha20poly1305(
              chacha_key, chacha_header, 0U, nullptr, chacha_tag_only) ==
          btp::AeadError::Ok);
    CHECK(btp::aead_open_chacha20poly1305(
              chacha_key, chacha_header, sizeof(chacha_tag_only),
              chacha_tag_only, empty_opened) == btp::AeadError::Ok);
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
    test_aad_ignores_fragmentation_fields();
    test_sealed_message_survives_fragmentation_and_reassembly();
    test_aead_cipher_aes_gcm_round_trip_reuses_key();
    test_aead_cipher_chacha20poly1305_round_trip_reuses_key();
    test_aead_cipher_matches_stateless_functions();
    test_aead_cipher_tag_corruption_is_rejected();
    test_aead_cipher_rejects_cipher_mismatch();
    test_aead_cipher_wrong_key_size_leaves_invalid();
    test_aead_cipher_reset_does_not_leak();
    test_null_pointers_are_rejected();

    if (failures != 0) {
        std::cerr << failures << " aead test(s) failed\n";
        return 1;
    }
    std::cout << "All BTP aead skeleton tests passed\n";
    return 0;
}
