#include "btp/codec.hpp"

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

struct ValidVector {
    const char* file;
    btp::TransportProfile transport;
    btp::Header header;
    std::vector<std::uint8_t> payload;
};

struct InvalidVector {
    const char* file;
    btp::TransportProfile transport;
    btp::Error error;
};

btp::Header header(btp::MessageType type,
                   std::uint16_t flags,
                   std::uint32_t source_id,
                   std::uint32_t boot_id,
                   std::uint32_t sequence,
                   std::uint64_t timestamp_us,
                   std::uint16_t object_id,
                   std::uint8_t fragment_index,
                   std::uint8_t fragment_count) {
    btp::Header result = {};
    result.type = type;
    result.flags = flags;
    result.source_id = source_id;
    result.boot_id = boot_id;
    result.sequence = sequence;
    result.timestamp_us = timestamp_us;
    result.object_id = object_id;
    result.fragment_index = fragment_index;
    result.fragment_count = fragment_count;
    return result;
}

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

bool same_header(const btp::Header& left, const btp::Header& right) {
    return left.type == right.type && left.flags == right.flags &&
           left.source_id == right.source_id && left.boot_id == right.boot_id &&
           left.sequence == right.sequence &&
           left.timestamp_us == right.timestamp_us &&
           left.object_id == right.object_id &&
           left.fragment_index == right.fragment_index &&
           left.fragment_count == right.fragment_count;
}

std::vector<ValidVector> valid_vectors() {
    // Identical to test_conformance.cpp's "hello" vector: same fields, same
    // payload, ENCRYPTED clear. test-vectors/v2/valid/hello.bin is checked to
    // be byte-for-byte identical to test-vectors/v1/valid/hello.bin -- the
    // AEAD plumbing must not change a single bit of the unencrypted path.
    const std::uint8_t hello[] = {
        0x03U, 0x01U, 0x00U, 0x00U, 0x2eU, 0xd1U, 0x00U, 0x00U,
        0x04U, 0x00U, 0x20U, 0x00U, 0x80U, 0x00U, 0x00U, 0x00U,
        0x88U, 0x13U, 0x00U, 0x00U, 0x00U, 0x11U, 0x22U, 0x33U,
        0x44U, 0x55U, 0x66U, 0x77U, 0x88U, 0x99U, 0xaaU, 0xbbU,
        0xccU, 0xddU, 0xeeU, 0xffU, 0x00U, 0x00U, 0x00U, 0x00U,
        0x01U
    };
    // MANIFEST_DATA in manifest_format_version 2 (test-vectors/v2/valid/
    // manifest_data.json). btp::codec treats the payload as opaque, so this
    // vector pins the frame around the new source_info block for the codec
    // while the hand-written manifest encoders in the consuming firmwares use
    // the same bytes as their own byte reference.
    const std::uint8_t manifest_data[] = {
        0x44U, 0x33U, 0x22U, 0x11U, 0x88U, 0x77U, 0x66U, 0x55U,
        0x09U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U,
        0x02U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U,
        0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U, 0x88U,
        0x99U, 0xaaU, 0xbbU, 0xccU, 0xddU, 0xeeU, 0xffU, 0x00U,
        0x44U, 0x33U, 0x22U, 0x11U, 0x88U, 0x77U, 0x66U, 0x55U,
        0x01U, 0x01U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x0bU, 0x00U, 0x73U, 0x65U, 0x6eU, 0x73U,
        0x6fU, 0x72U, 0x2dU, 0x6eU, 0x6fU, 0x64U, 0x65U, 0x03U,
        0x00U, 0x0aU, 0x00U, 0x66U, 0x77U, 0x5fU, 0x76U, 0x65U,
        0x72U, 0x73U, 0x69U, 0x6fU, 0x6eU, 0x08U, 0x00U, 0x46U,
        0x69U, 0x72U, 0x6dU, 0x77U, 0x61U, 0x72U, 0x65U, 0x09U,
        0x00U, 0x32U, 0x2eU, 0x31U, 0x2eU, 0x30U, 0x2dU, 0x72U,
        0x63U, 0x31U, 0x04U, 0x00U, 0x63U, 0x68U, 0x69U, 0x70U,
        0x04U, 0x00U, 0x43U, 0x68U, 0x69U, 0x70U, 0x08U, 0x00U,
        0x45U, 0x53U, 0x50U, 0x33U, 0x32U, 0x2dU, 0x53U, 0x33U,
        0x04U, 0x00U, 0x6eU, 0x61U, 0x6dU, 0x65U, 0x00U, 0x00U,
        0x0bU, 0x00U, 0x6cU, 0x61U, 0x62U, 0x20U, 0x62U, 0x65U,
        0x6eU, 0x63U, 0x68U, 0x20U, 0x32U
    };
    // ciphertext || tag from test-vectors/v2/valid/aead_telemetry_gcm.json,
    // real AES-128-GCM output (see the JSON's "aead" block for key/nonce/AAD/
    // plaintext) -- not a placeholder.
    const std::uint8_t aead_ciphertext_and_tag[] = {
        0xb0U, 0x78U, 0x56U, 0xf4U, 0x38U, 0x4fU, 0xa0U, 0xadU,
        0x2dU, 0x5fU, 0xbbU, 0x32U, 0x0aU, 0x07U, 0x28U, 0x40U,
        0x19U, 0x97U, 0xe3U, 0x6eU, 0x66U, 0x9dU, 0x7eU, 0x7bU,
        0x53U, 0xaaU, 0xeaU, 0x23U, 0xb3U, 0x7aU, 0x47U, 0x4bU,
        0x44U, 0x77U, 0x1bU, 0x22U, 0x6bU, 0x91U, 0x80U, 0x0eU,
        0xc8U, 0xd7U
    };

    std::vector<ValidVector> result;
    result.push_back({
        "valid/hello.bin", btp::TransportProfile::Serial,
        header(btp::MessageType::Control, 0U, 0x0C0D0E0FU, 0x10203040U,
               1U, 1000U, 1U, 0U, 1U),
        std::vector<std::uint8_t>(hello, hello + sizeof(hello))});
    result.push_back({
        "valid/manifest_data.bin", btp::TransportProfile::Serial,
        header(btp::MessageType::Control, 0U, 0x0C0D0E0FU, 0x10203040U,
               2U, 1000U, 0x0004U, 0U, 1U),
        std::vector<std::uint8_t>(manifest_data,
                                  manifest_data + sizeof(manifest_data))});
    result.push_back({
        "valid/aead_telemetry_gcm.bin", btp::TransportProfile::EspNow,
        header(btp::MessageType::Telemetry, btp::kFlagEncrypted, 0x0C0D0E0FU,
               0x10203040U, 7U, 10000U, 1U, 0U, 1U),
        std::vector<std::uint8_t>(aead_ciphertext_and_tag,
                                  aead_ciphertext_and_tag +
                                  sizeof(aead_ciphertext_and_tag))});
    return result;
}

void test_valid_vectors_encode_and_decode() {
    const std::vector<ValidVector> vectors = valid_vectors();
    for (std::size_t index = 0U; index < vectors.size(); ++index) {
        const ValidVector& vector = vectors[index];
        const std::vector<std::uint8_t> expected = read_binary(vector.file);
        if (expected.empty()) {
            continue;
        }

        // encode() treats an already-encrypted payload as opaque bytes and
        // selects the version octet from ENCRYPTED automatically (write_header
        // in src/codec.cpp), so calling it with ciphertext||tag as the
        // payload reproduces the checked-in frame exactly -- the same code
        // path a real encoder uses after encrypting.
        std::vector<std::uint8_t> encoded(expected.size(), 0U);
        const btp::Frame source = {
            vector.header,
            {vector.payload.empty() ? nullptr : vector.payload.data(),
             vector.payload.size()}
        };
        std::size_t written = 0U;
        CHECK(btp::encode(source, vector.transport, encoded.data(),
                          encoded.size(), &written) == btp::Error::Ok);
        CHECK(written == expected.size());
        CHECK(encoded == expected);

        // decode() only verifies framing: it accepts the ENCRYPTED frame
        // structurally (version == 2, flag present, payload_size correct,
        // envelope CRC valid) without attempting to verify the AEAD tag --
        // btp::codec has no AEAD dependency and never will (see
        // test-vectors/v2/README.md). The returned payload is still
        // ciphertext||tag, untouched.
        btp::DecodedFrame decoded = {};
        CHECK(btp::decode(expected.data(), expected.size(), vector.transport,
                          &decoded) == btp::Error::Ok);
        CHECK(same_header(decoded.header, vector.header));
        CHECK(decoded.payload.size == vector.payload.size());
        CHECK(decoded.payload.data == expected.data() + btp::kV1HeaderSize);
        CHECK(decoded.payload.size == 0U ||
              std::memcmp(decoded.payload.data, vector.payload.data(),
                          decoded.payload.size) == 0);
        const std::uint32_t stored_crc =
            static_cast<std::uint32_t>(expected[expected.size() - 4U]) |
            (static_cast<std::uint32_t>(expected[expected.size() - 3U]) << 8U) |
            (static_cast<std::uint32_t>(expected[expected.size() - 2U]) << 16U) |
            (static_cast<std::uint32_t>(expected[expected.size() - 1U]) << 24U);
        CHECK(decoded.crc32 == stored_crc);

        // ENCRYPTED-specific plumbing (docs/encryption.md): version 2 must
        // have been selected automatically, and encode_header()/aead_nonce()
        // must reproduce the exact AAD and nonce a future AEAD-capable
        // consumer needs, byte for byte, from the same Header.
        const bool encrypted = (vector.header.flags & btp::kFlagEncrypted) != 0U;
        CHECK(expected[4] == (encrypted ? btp::kV2Version : btp::kV1Version));
        if (encrypted) {
            std::uint8_t aad[36] = {};
            CHECK(btp::encode_header(
                      vector.header,
                      static_cast<std::uint16_t>(vector.payload.size()),
                      aad) == btp::Error::Ok);
            CHECK(std::memcmp(aad, expected.data(), sizeof(aad)) == 0);

            std::uint8_t nonce[12] = {};
            btp::aead_nonce(vector.header, nonce);
            const std::uint8_t expected_nonce[12] = {
                0x0fU, 0x0eU, 0x0dU, 0x0cU, 0x40U, 0x30U, 0x20U, 0x10U,
                0x07U, 0x00U, 0x00U, 0x00U
            };
            CHECK(std::memcmp(nonce, expected_nonce, sizeof(nonce)) == 0);
        }
    }
}

void test_invalid_vectors_fail_for_documented_reason() {
    const InvalidVector vectors[] = {
        {"invalid/encrypted_version_mismatch.bin", btp::TransportProfile::EspNow,
         btp::Error::EncryptedVersionMismatch},
        {"invalid/crc_mismatch_encrypted.bin", btp::TransportProfile::EspNow,
         btp::Error::CrcMismatch},
        {"invalid/reserved_flag.bin", btp::TransportProfile::Serial,
         btp::Error::InvalidFlags},
        {"invalid/cipher_id_reserved.bin", btp::TransportProfile::EspNow,
         btp::Error::InvalidCipherId},
        {"invalid/cipher_id_requires_encrypted.bin", btp::TransportProfile::Serial,
         btp::Error::InvalidCipherId}
    };
    for (std::size_t index = 0U; index < sizeof(vectors) / sizeof(vectors[0]);
         ++index) {
        const std::vector<std::uint8_t> bytes = read_binary(vectors[index].file);
        if (bytes.empty()) {
            continue;
        }
        btp::DecodedFrame decoded = {};
        CHECK(btp::decode(bytes.data(), bytes.size(), vectors[index].transport,
                          &decoded) == vectors[index].error);
    }
}

}  // namespace

int main() {
    test_valid_vectors_encode_and_decode();
    test_invalid_vectors_fail_for_documented_reason();

    if (failures != 0) {
        std::cerr << failures << " conformance test(s) failed\n";
        return 1;
    }
    std::cout << "All BTP v2 conformance vectors passed\n";
    return 0;
}
