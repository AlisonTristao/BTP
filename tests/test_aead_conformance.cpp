// Real AEAD verification against the checked-in v2 conformance vectors.
//
// Deliberately separate from tests/test_conformance_v2.cpp: that suite is
// framing-only by design (it proves magic/version/CRC/flags and never calls
// a real cipher -- see test-vectors/v2/README.md's "What these vectors do and
// do not prove"). This file is what that README section points at: it links
// btp::aead and actually decrypts the
// real ciphertexts, proving byte-for-byte that what Python's `cryptography`
// package produced is exactly what btp::aead recovers.
#include "btp/aead.hpp"
#include "btp/codec.hpp"
#include "btp/fragmentation.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef BTP_VECTOR_ROOT_V2
#error BTP_VECTOR_ROOT_V2 must point to test-vectors/v2
#endif

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

std::string vector_path(const char* relative) {
    return std::string(BTP_VECTOR_ROOT_V2) + "/" + relative;
}

std::vector<std::uint8_t> read_binary(const char* relative) {
    const std::string path = vector_path(relative);
    std::ifstream stream(path.c_str(), std::ios::binary | std::ios::ate);
    if (!stream) {
        std::cerr << "cannot open conformance vector: " << path << '\n';
        ++failures;
        return std::vector<std::uint8_t>();
    }
    const std::ifstream::pos_type end = stream.tellg();
    if (end < 0) {
        std::cerr << "cannot determine conformance vector size: " << path << '\n';
        ++failures;
        return std::vector<std::uint8_t>();
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    stream.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream) {
        std::cerr << "cannot read conformance vector: " << path << '\n';
        ++failures;
        return std::vector<std::uint8_t>();
    }
    return bytes;
}

// test-vectors/v2/valid/aead_telemetry_gcm.json's "aead" block: key_hex and
// plaintext_hex, hardcoded here exactly as documented there.
void test_aes_gcm_vector_decrypts_real_ciphertext() {
    const std::vector<std::uint8_t> frame = read_binary("valid/aead_telemetry_gcm.bin");
    if (frame.empty()) {
        return;
    }

    // Reuse framing already trusted by test_conformance_v2.cpp instead of
    // re-deriving header bytes by hand: decode() gives back the real Header
    // and the ciphertext||tag payload view.
    btp::DecodedFrame decoded = {};
    CHECK(btp::decode(frame.data(), frame.size(), btp::kEspNowTransport,
                      &decoded) == btp::Error::Ok);

    const std::uint8_t key_bytes[btp::kAesGcmKeySize] = {
        0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
        0x08U, 0x09U, 0x0aU, 0x0bU, 0x0cU, 0x0dU, 0x0eU, 0x0fU
    };
    const btp::AeadKey key = {key_bytes, sizeof(key_bytes)};

    // plaintext_hex from the JSON's "aead" block ("BTP v2 AEAD test plaintext").
    const std::uint8_t expected_plaintext[] = {
        0x42U, 0x54U, 0x50U, 0x20U, 0x76U, 0x32U, 0x20U, 0x41U,
        0x45U, 0x41U, 0x44U, 0x20U, 0x74U, 0x65U, 0x73U, 0x74U,
        0x20U, 0x70U, 0x6cU, 0x61U, 0x69U, 0x6eU, 0x74U, 0x65U,
        0x78U, 0x74U
    };

    CHECK(decoded.payload.size == sizeof(expected_plaintext) + 16U);
    if (decoded.payload.size != sizeof(expected_plaintext) + 16U) {
        return;
    }

    std::vector<std::uint8_t> recovered(sizeof(expected_plaintext), 0U);

    // The actual cross-implementation proof: Python's `cryptography`
    // (AESGCM) produced frame's payload, and this call is the first time any
    // C++ code in this repository has ever decrypted it.
    CHECK(btp::aead_open_aes_gcm(
              key, decoded.header,
              static_cast<std::uint16_t>(decoded.payload.size),
              decoded.payload.data, recovered.data()) == btp::AeadError::Ok);
    CHECK(std::memcmp(recovered.data(), expected_plaintext,
                      sizeof(expected_plaintext)) == 0);
}

// The tag-corrupted, structurally-valid negative case that
// test-vectors/v2/README.md explains does not belong in the framing-only
// test_conformance_v2.cpp suite: decode() cannot detect it (it has no AEAD
// dependency), but btp::aead can and must.
void test_aes_gcm_vector_tag_corruption_is_rejected() {
    const std::vector<std::uint8_t> frame = read_binary("valid/aead_telemetry_gcm.bin");
    if (frame.empty()) {
        return;
    }

    btp::DecodedFrame decoded = {};
    CHECK(btp::decode(frame.data(), frame.size(), btp::kEspNowTransport,
                      &decoded) == btp::Error::Ok);
    if (decoded.payload.size < 16U) {
        ++failures;
        return;
    }

    std::vector<std::uint8_t> tampered(decoded.payload.data,
                                       decoded.payload.data + decoded.payload.size);
    tampered.back() ^= 0x01U;  // flip one bit in the last tag byte

    const std::uint8_t key_bytes[btp::kAesGcmKeySize] = {
        0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
        0x08U, 0x09U, 0x0aU, 0x0bU, 0x0cU, 0x0dU, 0x0eU, 0x0fU
    };
    const btp::AeadKey key = {key_bytes, sizeof(key_bytes)};

    std::vector<std::uint8_t> recovered(tampered.size() - 16U, 0U);
    CHECK(btp::aead_open_aes_gcm(
              key, decoded.header, static_cast<std::uint16_t>(tampered.size()),
              tampered.data(), recovered.data()) == btp::AeadError::TagMismatch);
}

// test-vectors/v2/valid/aead_telemetry_chacha20poly1305.json's "aead" block:
// key_hex and plaintext_hex, hardcoded here exactly as documented there.
void test_chacha20poly1305_vector_decrypts_real_ciphertext() {
    const std::vector<std::uint8_t> frame =
        read_binary("valid/aead_telemetry_chacha20poly1305.bin");
    if (frame.empty()) {
        return;
    }

    btp::DecodedFrame decoded = {};
    CHECK(btp::decode(frame.data(), frame.size(), btp::kEspNowTransport,
                      &decoded) == btp::Error::Ok);

    const std::uint8_t key_bytes[btp::kChaCha20Poly1305KeySize] = {
        0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
        0x08U, 0x09U, 0x0aU, 0x0bU, 0x0cU, 0x0dU, 0x0eU, 0x0fU,
        0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U,
        0x18U, 0x19U, 0x1aU, 0x1bU, 0x1cU, 0x1dU, 0x1eU, 0x1fU
    };
    const btp::AeadKey key = {key_bytes, sizeof(key_bytes)};

    // plaintext_hex from the JSON's "aead" block
    // ("BTP v2 ChaCha20-Poly1305 AEAD test plaintext").
    const std::uint8_t expected_plaintext[] = {
        0x42U, 0x54U, 0x50U, 0x20U, 0x76U, 0x32U, 0x20U, 0x43U,
        0x68U, 0x61U, 0x43U, 0x68U, 0x61U, 0x32U, 0x30U, 0x2dU,
        0x50U, 0x6fU, 0x6cU, 0x79U, 0x31U, 0x33U, 0x30U, 0x35U,
        0x20U, 0x41U, 0x45U, 0x41U, 0x44U, 0x20U, 0x74U, 0x65U,
        0x73U, 0x74U, 0x20U, 0x70U, 0x6cU, 0x61U, 0x69U, 0x6eU,
        0x74U, 0x65U, 0x78U, 0x74U
    };

    CHECK(decoded.payload.size == sizeof(expected_plaintext) + 16U);
    if (decoded.payload.size != sizeof(expected_plaintext) + 16U) {
        return;
    }

    std::vector<std::uint8_t> recovered(sizeof(expected_plaintext), 0U);

    // The actual cross-implementation proof: Python's `cryptography`
    // (ChaCha20Poly1305) produced frame's payload, and this call is the
    // first time any C++ code in this repository has ever decrypted it.
    CHECK(btp::aead_open_chacha20poly1305(
              key, decoded.header,
              static_cast<std::uint16_t>(decoded.payload.size),
              decoded.payload.data, recovered.data()) == btp::AeadError::Ok);
    CHECK(std::memcmp(recovered.data(), expected_plaintext,
                      sizeof(expected_plaintext)) == 0);
}

// The end-to-end interop proof for docs/encryption.md section 5: Python's
// `cryptography` sealed
// one 220-octet message whole and cut it into two ESP-NOW fragments, which are
// checked in as two separate .bin vectors. Here C++ decodes both frames,
// reassembles them, and opens the result under mbedtls. Nothing re-derives the
// tag, and neither fragment can be opened alone -- the tag only exists over the
// whole logical message. This is what makes key-less gateway re-fragmentation
// possible, and it is the case the reassembler used to reject outright.
void test_aes_gcm_fragmented_vector_reassembles_and_decrypts() {
    const std::vector<std::uint8_t> first =
        read_binary("valid/aead_fragmented_gcm_0.bin");
    const std::vector<std::uint8_t> second =
        read_binary("valid/aead_fragmented_gcm_1.bin");
    if (first.empty() || second.empty()) {
        return;
    }

    btp::ReassemblySlot slots[1];
    std::vector<std::uint8_t> storage(256U);
    const btp::ReassemblyStorage storage_view = {storage.data(),
                                                 storage.size()};
    btp::Reassembler reassembler(slots, &storage_view, 1U, 1000U);
    CHECK(reassembler.valid());

    // Feed fragment 1 before fragment 0: arrival order must not matter.
    const std::vector<std::uint8_t>* order[2] = {&second, &first};
    btp::ReassembledMessage message = {};
    btp::ReassemblyEvent last = btp::ReassemblyEvent::InvalidArgument;
    for (std::size_t step = 0U; step < 2U; ++step) {
        btp::DecodedFrame decoded = {};
        CHECK(btp::decode(order[step]->data(), order[step]->size(),
                          btp::kEspNowTransport,
                          &decoded) == btp::Error::Ok);
        last = reassembler.push(decoded, 1U, &message);
    }

    CHECK(last == btp::ReassemblyEvent::Complete);
    if (last != btp::ReassemblyEvent::Complete) {
        return;
    }
    // 220 octets of plaintext plus the 16-octet tag.
    CHECK(message.payload.size == 236U);
    // Completion restores the logical header, which is what the AAD was built
    // over: FRAGMENTED gone, ENCRYPTED kept, index 0 and count 1.
    CHECK(message.header.flags == btp::kFlagEncrypted);
    CHECK(message.header.fragment_index == 0U);
    CHECK(message.header.fragment_count == 1U);
    if (message.payload.size != 236U) {
        return;
    }

    const std::uint8_t key_bytes[btp::kAesGcmKeySize] = {
        0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
        0x08U, 0x09U, 0x0aU, 0x0bU, 0x0cU, 0x0dU, 0x0eU, 0x0fU
    };
    const btp::AeadKey key = {key_bytes, sizeof(key_bytes)};

    std::vector<std::uint8_t> recovered(220U, 0U);
    CHECK(btp::aead_open(key, message.header,
                         static_cast<std::uint16_t>(message.payload.size),
                         message.payload.data,
                         recovered.data()) == btp::AeadError::Ok);

    // plaintext_note in the JSON: byte i is (i * 7) & 0xFF.
    for (std::size_t index = 0U; index < recovered.size(); ++index) {
        CHECK(recovered[index] ==
              static_cast<std::uint8_t>((index * 7U) & 0xFFU));
    }
}

}  // namespace

int main() {
    test_aes_gcm_vector_decrypts_real_ciphertext();
    test_aes_gcm_vector_tag_corruption_is_rejected();
    test_chacha20poly1305_vector_decrypts_real_ciphertext();
    test_aes_gcm_fragmented_vector_reassembles_and_decrypts();

    if (failures != 0) {
        std::cerr << failures << " aead conformance test(s) failed\n";
        return 1;
    }
    std::cout << "All BTP v2 AEAD conformance vectors passed\n";
    return 0;
}
