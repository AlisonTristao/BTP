#include "btp/codec.hpp"

#include <algorithm>
#include <array>
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

const std::array<std::uint8_t, 40> kEmptyLogVector = {{
    0x42, 0x54, 0x50, 0x00, 0x01, 0x02, 0x00, 0x00,
    0x24, 0x00, 0x00, 0x00, 0x44, 0x33, 0x22, 0x11,
    0xd4, 0xc3, 0xb2, 0xa1, 0x01, 0x00, 0x00, 0x00,
    0x40, 0x42, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x01, 0x3a, 0x15, 0xe7, 0xdf
}};

const std::array<std::uint8_t, 44> kFragmentVector = {{
    0x42, 0x54, 0x50, 0x00, 0x01, 0x01, 0x01, 0x00,
    0x24, 0x00, 0x04, 0x00, 0x04, 0x03, 0x02, 0x01,
    0x40, 0x30, 0x20, 0x10, 0x08, 0x07, 0x06, 0x05,
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0x34, 0x12, 0x01, 0x02, 0x00, 0x0a, 0x0d, 0xff,
    0x2d, 0x80, 0x1f, 0x40
}};

btp::Header empty_log_header() {
    btp::Header header = {};
    header.type = btp::MessageType::Log;
    header.source_id = 0x11223344U;
    header.boot_id = 0xA1B2C3D4U;
    header.sequence = 1U;
    header.timestamp_us = 1000000U;
    header.object_id = 2U;
    header.fragment_count = 1U;
    return header;
}

std::vector<std::uint8_t> encode_frame(const btp::Frame& frame,
                                       btp::TransportProfile transport) {
    std::size_t required = 0U;
    CHECK(btp::encoded_size(frame.payload.size, transport, &required) ==
          btp::Error::Ok);
    std::vector<std::uint8_t> output(required, 0xA5U);
    std::size_t written = 0U;
    CHECK(btp::encode(frame, transport, output.data(), output.size(), &written) ==
          btp::Error::Ok);
    CHECK(written == required);
    return output;
}

void write_crc(std::vector<std::uint8_t>* frame) {
    const std::size_t crc_offset = frame->size() - btp::kV1CrcSize;
    const std::uint32_t value = btp::crc32(frame->data(), crc_offset);
    (*frame)[crc_offset] = static_cast<std::uint8_t>(value);
    (*frame)[crc_offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
    (*frame)[crc_offset + 2U] = static_cast<std::uint8_t>(value >> 16U);
    (*frame)[crc_offset + 3U] = static_cast<std::uint8_t>(value >> 24U);
}

btp::Error decode_serial(const std::vector<std::uint8_t>& bytes) {
    btp::DecodedFrame decoded = {};
    return btp::decode(bytes.data(), bytes.size(),
                       btp::TransportProfile::Serial, &decoded);
}

void test_crc_reference() {
    const std::uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(btp::crc32(check, sizeof(check)) == 0xCBF43926U);
    CHECK(btp::crc32(nullptr, 0U) == 0U);
}

void test_specification_vectors() {
    const btp::Frame empty_log = {
        empty_log_header(),
        {nullptr, 0U}
    };
    const std::vector<std::uint8_t> encoded =
        encode_frame(empty_log, btp::TransportProfile::EspNow);
    CHECK(encoded.size() == kEmptyLogVector.size());
    CHECK(std::memcmp(encoded.data(), kEmptyLogVector.data(), encoded.size()) == 0);

    const std::uint8_t payload[] = {0x00U, 0x0aU, 0x0dU, 0xffU};
    btp::Header header = {};
    header.type = btp::MessageType::Telemetry;
    header.flags = btp::kFlagFragmented;
    header.source_id = 0x01020304U;
    header.boot_id = 0x10203040U;
    header.sequence = 0x05060708U;
    header.timestamp_us = UINT64_C(0x0102030405060708);
    header.object_id = 0x1234U;
    header.fragment_index = 1U;
    header.fragment_count = 2U;
    const btp::Frame fragment = {header, {payload, sizeof(payload)}};
    const std::vector<std::uint8_t> encoded_fragment =
        encode_frame(fragment, btp::TransportProfile::EspNow);
    CHECK(encoded_fragment.size() == kFragmentVector.size());
    CHECK(std::memcmp(encoded_fragment.data(), kFragmentVector.data(),
                      encoded_fragment.size()) == 0);
}

void test_round_trip_and_boundaries() {
    std::vector<std::uint8_t> payload(btp::kSerialMaxPayloadSize);
    for (std::size_t index = 0U; index < payload.size(); ++index) {
        payload[index] = static_cast<std::uint8_t>(index);
    }

    btp::Header header = empty_log_header();
    header.type = btp::MessageType::Command;
    header.sequence = 0xFFFFFFFFU;
    header.timestamp_us = UINT64_MAX;
    header.object_id = 0xFFFFU;
    const btp::Frame original = {header, {payload.data(), payload.size()}};
    const std::vector<std::uint8_t> bytes =
        encode_frame(original, btp::TransportProfile::Serial);
    CHECK(bytes.size() == btp::kSerialMaxFrameSize);

    btp::DecodedFrame decoded = {};
    CHECK(btp::decode(bytes.data(), bytes.size(), btp::TransportProfile::Serial,
                      &decoded) == btp::Error::Ok);
    CHECK(decoded.header.type == original.header.type);
    CHECK(decoded.header.flags == original.header.flags);
    CHECK(decoded.header.source_id == original.header.source_id);
    CHECK(decoded.header.boot_id == original.header.boot_id);
    CHECK(decoded.header.sequence == original.header.sequence);
    CHECK(decoded.header.timestamp_us == original.header.timestamp_us);
    CHECK(decoded.header.object_id == original.header.object_id);
    CHECK(decoded.header.fragment_index == original.header.fragment_index);
    CHECK(decoded.header.fragment_count == original.header.fragment_count);
    CHECK(decoded.payload.size == payload.size());
    CHECK(decoded.payload.data == bytes.data() + btp::kV1HeaderSize);
    CHECK(std::memcmp(decoded.payload.data, payload.data(), payload.size()) == 0);

    std::size_t size = 0U;
    CHECK(btp::encoded_size(btp::kEspNowMaxPayloadSize,
                            btp::TransportProfile::EspNow, &size) == btp::Error::Ok);
    CHECK(size == btp::kEspNowMaxFrameSize);
    CHECK(btp::encoded_size(btp::kEspNowMaxPayloadSize + 1U,
                            btp::TransportProfile::EspNow, &size) ==
          btp::Error::PayloadTooLarge);
    CHECK(btp::decode(bytes.data(), bytes.size(), btp::TransportProfile::EspNow,
                      &decoded) == btp::Error::FrameTooLarge);
}

void test_encoder_rejections_are_atomic() {
    const std::uint8_t payload[] = {1U, 2U, 3U};
    btp::Header header = empty_log_header();
    btp::Frame frame = {header, {payload, sizeof(payload)}};
    std::array<std::uint8_t, 64> output;
    std::size_t written = 123U;

    const auto expect = [&](btp::Error expected) {
        output.fill(0xA5U);
        CHECK(btp::encode(frame, btp::TransportProfile::EspNow, output.data(),
                          output.size(), &written) == expected);
        for (std::size_t index = 0U; index < output.size(); ++index) {
            CHECK(output[index] == 0xA5U);
        }
    };

    frame.header.type = btp::MessageType::Invalid;
    expect(btp::Error::InvalidType);
    frame.header = header;
    frame.header.flags = 0x0004U;  // 0x0002 is now ENCRYPTED (valid); 0x0004 stays reserved.
    expect(btp::Error::InvalidFlags);
    frame.header = header;
    frame.header.source_id = 0U;
    expect(btp::Error::InvalidSourceId);
    frame.header = header;
    frame.header.boot_id = 0U;
    expect(btp::Error::InvalidBootId);
    frame.header = header;
    frame.header.fragment_index = 1U;
    expect(btp::Error::InvalidFragmentation);
    frame.header = header;
    frame.header.flags = btp::kFlagFragmented;
    frame.header.fragment_count = 1U;
    expect(btp::Error::InvalidFragmentation);
    frame.header.fragment_count = 2U;
    frame.header.fragment_index = 2U;
    expect(btp::Error::InvalidFragmentation);

    frame.header = header;
    frame.payload = {nullptr, 1U};
    expect(btp::Error::InvalidArgument);
    frame.payload = {payload, sizeof(payload)};
    output.fill(0xA5U);
    CHECK(btp::encode(frame, btp::TransportProfile::EspNow, output.data(), 42U,
                      &written) == btp::Error::BufferTooSmall);
    for (std::size_t index = 0U; index < output.size(); ++index) {
        CHECK(output[index] == 0xA5U);
    }
}

void test_encoder_supports_overlapping_payload() {
    std::array<std::uint8_t, 64> storage = {};
    const std::uint8_t expected[] = {0x00U, 0x0aU, 0x0dU, 0xffU};
    std::copy(expected, expected + sizeof(expected), storage.begin());

    const btp::Frame frame = {
        empty_log_header(),
        {storage.data(), sizeof(expected)}
    };
    std::size_t written = 0U;
    CHECK(btp::encode(frame, btp::TransportProfile::EspNow, storage.data(),
                      storage.size(), &written) == btp::Error::Ok);
    CHECK(written == btp::kV1MinimumFrameSize + sizeof(expected));
    CHECK(std::memcmp(storage.data() + btp::kV1HeaderSize, expected,
                      sizeof(expected)) == 0);

    btp::DecodedFrame decoded = {};
    CHECK(btp::decode(storage.data(), written, btp::TransportProfile::EspNow,
                      &decoded) == btp::Error::Ok);
}

void test_decoder_structural_rejections() {
    std::vector<std::uint8_t> valid(kFragmentVector.begin(), kFragmentVector.end());
    btp::DecodedFrame decoded = {};

    for (std::size_t size = 0U; size < btp::kV1MinimumFrameSize; ++size) {
        CHECK(btp::decode(valid.data(), size, btp::TransportProfile::Serial,
                          &decoded) == btp::Error::FrameTooShort);
    }

    std::vector<std::uint8_t> changed = valid;
    changed[0] ^= 1U;
    CHECK(decode_serial(changed) == btp::Error::InvalidMagic);
    changed = valid;
    changed[4] = 3U;  // 2 is now kV2Version (valid, section 8); 3 stays unsupported.
    CHECK(decode_serial(changed) == btp::Error::UnsupportedVersion);
    changed = valid;
    changed[8] = 35U;
    CHECK(decode_serial(changed) == btp::Error::InvalidHeaderSize);
    changed = valid;
    changed[10] = 5U;
    CHECK(decode_serial(changed) == btp::Error::SizeMismatch);
    changed = valid;
    changed.push_back(0U);
    CHECK(decode_serial(changed) == btp::Error::SizeMismatch);
    changed = valid;
    changed.back() ^= 0x80U;
    CHECK(decode_serial(changed) == btp::Error::CrcMismatch);
    changed = valid;
    changed[36] ^= 1U;
    CHECK(decode_serial(changed) == btp::Error::CrcMismatch);

    std::vector<std::uint8_t> oversized(btp::kEspNowMaxFrameSize + 1U, 0U);
    CHECK(btp::decode(oversized.data(), oversized.size(),
                      btp::TransportProfile::EspNow, &decoded) ==
          btp::Error::FrameTooLarge);
}

void test_decoder_semantic_rejections() {
    std::vector<std::uint8_t> changed(kEmptyLogVector.begin(), kEmptyLogVector.end());

    changed[5] = 0U;
    write_crc(&changed);
    CHECK(decode_serial(changed) == btp::Error::InvalidType);
    changed.assign(kEmptyLogVector.begin(), kEmptyLogVector.end());
    changed[5] = 6U;
    write_crc(&changed);
    CHECK(decode_serial(changed) == btp::Error::InvalidType);
    changed.assign(kEmptyLogVector.begin(), kEmptyLogVector.end());
    changed[6] = 4U;  // 0x0002 is now ENCRYPTED (valid); 0x0004 stays reserved.
    write_crc(&changed);
    CHECK(decode_serial(changed) == btp::Error::InvalidFlags);
    changed.assign(kEmptyLogVector.begin(), kEmptyLogVector.end());
    std::fill(changed.begin() + 12, changed.begin() + 16, 0U);
    write_crc(&changed);
    CHECK(decode_serial(changed) == btp::Error::InvalidSourceId);
    changed.assign(kEmptyLogVector.begin(), kEmptyLogVector.end());
    std::fill(changed.begin() + 16, changed.begin() + 20, 0U);
    write_crc(&changed);
    CHECK(decode_serial(changed) == btp::Error::InvalidBootId);
    changed.assign(kEmptyLogVector.begin(), kEmptyLogVector.end());
    changed[34] = 1U;
    write_crc(&changed);
    CHECK(decode_serial(changed) == btp::Error::InvalidFragmentation);
    changed.assign(kEmptyLogVector.begin(), kEmptyLogVector.end());
    changed[35] = 0U;
    write_crc(&changed);
    CHECK(decode_serial(changed) == btp::Error::InvalidFragmentation);

    changed.assign(kFragmentVector.begin(), kFragmentVector.end());
    changed[35] = 1U;
    write_crc(&changed);
    CHECK(decode_serial(changed) == btp::Error::InvalidFragmentation);
    changed.assign(kFragmentVector.begin(), kFragmentVector.end());
    changed[34] = 2U;
    write_crc(&changed);
    CHECK(decode_serial(changed) == btp::Error::InvalidFragmentation);
}

void test_corruption_of_every_field() {
    const std::array<std::size_t, 15> field_offsets = {{
        0U, 4U, 5U, 6U, 8U, 10U, 12U, 16U, 20U, 24U, 32U, 34U, 35U, 36U, 40U
    }};
    for (std::size_t index = 0U; index < field_offsets.size(); ++index) {
        std::vector<std::uint8_t> changed(kFragmentVector.begin(), kFragmentVector.end());
        changed[field_offsets[index]] ^= 0x01U;
        CHECK(decode_serial(changed) != btp::Error::Ok);
    }
}

void test_failure_does_not_publish_decoded_result() {
    btp::DecodedFrame decoded = {};
    decoded.header.source_id = 0xDEADBEEFU;
    decoded.payload.data = reinterpret_cast<const std::uint8_t*>(1U);
    decoded.payload.size = 99U;
    decoded.crc32 = 0x12345678U;
    std::vector<std::uint8_t> invalid(kEmptyLogVector.begin(), kEmptyLogVector.end());
    invalid.back() ^= 1U;
    CHECK(btp::decode(invalid.data(), invalid.size(), btp::TransportProfile::Serial,
                      &decoded) == btp::Error::CrcMismatch);
    CHECK(decoded.header.source_id == 0xDEADBEEFU);
    CHECK(decoded.payload.data == reinterpret_cast<const std::uint8_t*>(1U));
    CHECK(decoded.payload.size == 99U);
    CHECK(decoded.crc32 == 0x12345678U);
}

void test_aead_nonce_matches_header_fields() {
    btp::Header header = {};
    header.source_id = 0x11223344U;
    header.boot_id = 0xA1B2C3D4U;
    header.sequence = 0x05060708U;

    std::uint8_t nonce[12] = {};
    btp::aead_nonce(header, nonce);

    const std::uint8_t expected[12] = {
        0x44U, 0x33U, 0x22U, 0x11U,   // source_id, little-endian
        0xD4U, 0xC3U, 0xB2U, 0xA1U,   // boot_id, little-endian
        0x08U, 0x07U, 0x06U, 0x05U    // sequence, little-endian
    };
    CHECK(std::memcmp(nonce, expected, sizeof(expected)) == 0);
}

void test_encode_header_matches_encode_bytes() {
    btp::Header header = empty_log_header();
    header.type = btp::MessageType::Command;
    header.flags = btp::kFlagEncrypted;
    header.sequence = 0x0A0B0C0DU;

    const std::uint8_t payload[] = {1U, 2U, 3U, 4U, 5U};
    const btp::Frame frame = {header, {payload, sizeof(payload)}};
    const std::vector<std::uint8_t> encoded =
        encode_frame(frame, btp::TransportProfile::EspNow);

    std::uint8_t header_bytes[36] = {};
    CHECK(btp::encode_header(header, static_cast<std::uint16_t>(sizeof(payload)),
                             header_bytes) == btp::Error::Ok);
    CHECK(std::memcmp(encoded.data(), header_bytes, sizeof(header_bytes)) == 0);

    // Same invariant with ENCRYPTED clear: both paths still must agree.
    btp::Header plain_header = empty_log_header();
    plain_header.sequence = 7U;
    const btp::Frame plain_frame = {plain_header, {payload, sizeof(payload)}};
    const std::vector<std::uint8_t> plain_encoded =
        encode_frame(plain_frame, btp::TransportProfile::EspNow);
    std::uint8_t plain_header_bytes[36] = {};
    CHECK(btp::encode_header(plain_header, static_cast<std::uint16_t>(sizeof(payload)),
                             plain_header_bytes) == btp::Error::Ok);
    CHECK(std::memcmp(plain_encoded.data(), plain_header_bytes,
                      sizeof(plain_header_bytes)) == 0);
}

void test_encrypted_round_trip_with_placeholder_cipher() {
    btp::Header header = empty_log_header();
    header.type = btp::MessageType::Telemetry;
    header.flags = btp::kFlagEncrypted;
    header.sequence = 0x0A0B0C0DU;

    const std::uint8_t plaintext[] = {0x10U, 0x20U, 0x30U, 0x40U, 0x50U};
    const std::uint16_t sealed_size =
        static_cast<std::uint16_t>(sizeof(plaintext) + 16U);

    std::uint8_t aad[36] = {};
    CHECK(btp::encode_header(header, sealed_size, aad) == btp::Error::Ok);

    // PLACEHOLDER ONLY: XOR "ciphertext" plus a fixed "tag" stand in for a
    // real AES-128-GCM/ChaCha20-Poly1305 seal; this is not encryption and
    // proves nothing about confidentiality or authenticity. Real AEAD wiring
    // (mbedtls) is a future step outside btp::codec, in each consumer.
    std::uint8_t sealed[sizeof(plaintext) + 16U];
    for (std::size_t index = 0U; index < sizeof(plaintext); ++index) {
        sealed[index] = static_cast<std::uint8_t>(plaintext[index] ^ 0xFFU);
    }
    for (std::size_t index = 0U; index < 16U; ++index) {
        sealed[sizeof(plaintext) + index] = static_cast<std::uint8_t>(0xE0U + index);
    }

    const btp::Frame frame = {header, {sealed, sizeof(sealed)}};
    const std::vector<std::uint8_t> encoded =
        encode_frame(frame, btp::TransportProfile::EspNow);

    CHECK(encoded[4] == btp::kV2Version);
    CHECK(std::memcmp(encoded.data(), aad, sizeof(aad)) == 0);
    const std::uint16_t header_payload_size = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(encoded[10]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(encoded[11]) << 8U));
    CHECK(header_payload_size == sealed_size);

    btp::DecodedFrame decoded = {};
    CHECK(btp::decode(encoded.data(), encoded.size(), btp::TransportProfile::EspNow,
                      &decoded) == btp::Error::Ok);
    CHECK((decoded.header.flags & btp::kFlagEncrypted) != 0U);
    CHECK(decoded.payload.size == sizeof(sealed));
    CHECK(std::memcmp(decoded.payload.data, sealed, sizeof(sealed)) == 0);
}

void test_decoder_rejects_encrypted_flag_with_v1_version() {
    btp::Header header = empty_log_header();
    header.flags = btp::kFlagEncrypted;
    const std::uint8_t payload[] = {0xAAU, 0xBBU};
    const btp::Frame frame = {header, {payload, sizeof(payload)}};

    std::vector<std::uint8_t> encoded =
        encode_frame(frame, btp::TransportProfile::EspNow);
    CHECK(encoded[4] == btp::kV2Version);

    // Patch the version octet back to 1 while ENCRYPTED stays marked, then
    // fix up the CRC so only that mismatch is under test.
    encoded[4] = btp::kV1Version;
    write_crc(&encoded);

    btp::DecodedFrame decoded = {};
    CHECK(btp::decode(encoded.data(), encoded.size(), btp::TransportProfile::EspNow,
                      &decoded) == btp::Error::EncryptedVersionMismatch);
}

}  // namespace

int main() {
    test_crc_reference();
    test_specification_vectors();
    test_round_trip_and_boundaries();
    test_encoder_rejections_are_atomic();
    test_encoder_supports_overlapping_payload();
    test_decoder_structural_rejections();
    test_decoder_semantic_rejections();
    test_corruption_of_every_field();
    test_failure_does_not_publish_decoded_result();
    test_aead_nonce_matches_header_fields();
    test_encode_header_matches_encode_bytes();
    test_encrypted_round_trip_with_placeholder_cipher();
    test_decoder_rejects_encrypted_flag_with_v1_version();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All BTP codec tests passed\n";
    return 0;
}
