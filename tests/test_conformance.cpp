#include "btp/codec.hpp"
#include "btp/fragmentation.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef BTP_VECTOR_ROOT
#error BTP_VECTOR_ROOT must point to test-vectors/v1
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
    return std::string(BTP_VECTOR_ROOT) + "/" + relative;
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
    const std::uint8_t hello[] = {
        0x03U, 0x01U, 0x00U, 0x00U, 0x2eU, 0xd1U, 0x00U, 0x00U,
        0x04U, 0x00U, 0x20U, 0x00U, 0x80U, 0x00U, 0x00U, 0x00U,
        0x88U, 0x13U, 0x00U, 0x00U, 0x00U, 0x11U, 0x22U, 0x33U,
        0x44U, 0x55U, 0x66U, 0x77U, 0x88U, 0x99U, 0xaaU, 0xbbU,
        0xccU, 0xddU, 0xeeU, 0xffU, 0x00U, 0x00U, 0x00U, 0x00U,
        0x01U
    };
    const std::uint8_t log_utf8[] = {
        0x49U, 0x6eU, 0x69U, 0x63U, 0x69U, 0x61U, 0x6cU, 0x69U,
        0x7aU, 0x61U, 0xc3U, 0xa7U, 0xc3U, 0xa3U, 0x6fU, 0x20U,
        0x4fU, 0x4bU, 0x3aU, 0x20U, 0xc2U, 0xb5U
    };
    const std::uint8_t telemetry[] = {
        0x01U, 0x00U, 0x00U, 0x0aU, 0x0dU, 0x7fU, 0x80U, 0xffU
    };
    const std::uint8_t command[] = {
        0x44U, 0x33U, 0x22U, 0x11U, 0x88U, 0x77U, 0x66U, 0x55U,
        0x01U, 0x02U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x06U, 0x00U, 0x00U, 0x00U, 0x00U, 0x0aU, 0x0dU, 0x7fU,
        0x80U, 0xffU
    };
    const std::uint8_t a0[] = {0x10U, 0x11U, 0x12U};
    const std::uint8_t a1[] = {0x13U, 0x14U, 0x15U};
    const std::uint8_t b0[] = {0xa0U, 0xa1U, 0xa2U, 0xa3U};
    const std::uint8_t b1[] = {0xa4U, 0xa5U, 0xa6U, 0xa7U};
    const std::uint8_t usb_hid_telemetry[] = {
        0x00U, 0x0aU, 0x0dU, 0x7fU, 0x80U, 0xffU, 0x01U, 0x02U,
        0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U, 0x09U, 0x0aU,
        0x0bU, 0x0cU, 0x0dU, 0x0eU, 0x0fU, 0x10U
    };

    std::vector<ValidVector> result;
    result.push_back({
        "valid/hello.bin", btp::TransportProfile::Serial,
        header(btp::MessageType::Control, 0U, 0x0C0D0E0FU, 0x10203040U,
               1U, 1000U, 1U, 0U, 1U),
        std::vector<std::uint8_t>(hello, hello + sizeof(hello))});
    result.push_back({
        "valid/log_utf8.bin", btp::TransportProfile::EspNow,
        header(btp::MessageType::Log, 0U, 0x11223344U, 0xA1B2C3D4U,
               2U, 1000000U, 2U, 0U, 1U),
        std::vector<std::uint8_t>(log_utf8, log_utf8 + sizeof(log_utf8))});
    result.push_back({
        "valid/telemetry_packed_le.bin", btp::TransportProfile::EspNow,
        header(btp::MessageType::Telemetry, 0U, 0x11223344U, 0xA1B2C3D4U,
               3U, 1001000U, 0x0201U, 0U, 1U),
        std::vector<std::uint8_t>(telemetry,
                                  telemetry + sizeof(telemetry))});
    result.push_back({
        "valid/command_request.bin", btp::TransportProfile::Serial,
        header(btp::MessageType::Command, 0U, 0x0C0D0E0FU, 0x10203040U,
               2U, 2000U, 1U, 0U, 1U),
        std::vector<std::uint8_t>(command, command + sizeof(command))});
    result.push_back({
        "valid/fragment_source_a_0.bin", btp::TransportProfile::EspNow,
        header(btp::MessageType::Telemetry, btp::kFlagFragmented,
               0xAAA00001U, 0xAAA10001U, 0x10U, 100000U, 0x0301U, 0U, 2U),
        std::vector<std::uint8_t>(a0, a0 + sizeof(a0))});
    result.push_back({
        "valid/fragment_source_a_1.bin", btp::TransportProfile::EspNow,
        header(btp::MessageType::Telemetry, btp::kFlagFragmented,
               0xAAA00001U, 0xAAA10001U, 0x10U, 100000U, 0x0301U, 1U, 2U),
        std::vector<std::uint8_t>(a1, a1 + sizeof(a1))});
    result.push_back({
        "valid/fragment_source_b_0.bin", btp::TransportProfile::EspNow,
        header(btp::MessageType::Terminal, btp::kFlagFragmented,
               0xBBB00002U, 0xBBB10002U, 0x20U, 200000U, 2U, 0U, 2U),
        std::vector<std::uint8_t>(b0, b0 + sizeof(b0))});
    result.push_back({
        "valid/fragment_source_b_1.bin", btp::TransportProfile::EspNow,
        header(btp::MessageType::Terminal, btp::kFlagFragmented,
               0xBBB00002U, 0xBBB10002U, 0x20U, 200000U, 2U, 1U, 2U),
        std::vector<std::uint8_t>(b1, b1 + sizeof(b1))});
    result.push_back({
        "valid/usb_hid_telemetry.bin", btp::TransportProfile::UsbHid,
        header(btp::MessageType::Telemetry, 0U, 0x22334455U, 0xB2C3D4E5U,
               1U, 0x112233U, 0x0301U, 0U, 1U),
        std::vector<std::uint8_t>(usb_hid_telemetry,
                                  usb_hid_telemetry + sizeof(usb_hid_telemetry))});
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
    }
}

void test_invalid_vectors_fail_for_documented_reason() {
    const InvalidVector vectors[] = {
        {"invalid/crc.bin", btp::TransportProfile::EspNow,
         btp::Error::CrcMismatch},
        {"invalid/magic.bin", btp::TransportProfile::Serial,
         btp::Error::InvalidMagic},
        {"invalid/version.bin", btp::TransportProfile::Serial,
         btp::Error::UnsupportedVersion},
        {"invalid/header_size.bin", btp::TransportProfile::Serial,
         btp::Error::InvalidHeaderSize},
        {"invalid/payload_size.bin", btp::TransportProfile::EspNow,
         btp::Error::SizeMismatch},
        {"invalid/fragment_index.bin", btp::TransportProfile::EspNow,
         btp::Error::InvalidFragmentation},
        {"invalid/fragment_count.bin", btp::TransportProfile::EspNow,
         btp::Error::InvalidFragmentation},
        {"invalid/usb_hid_payload_size.bin", btp::TransportProfile::UsbHid,
         btp::Error::PayloadTooLarge}
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

void test_two_sources_interleaved_out_of_order() {
    const char* arrival[] = {
        "valid/fragment_source_a_1.bin",
        "valid/fragment_source_b_0.bin",
        "valid/fragment_source_a_0.bin",
        "valid/fragment_source_b_1.bin"
    };
    std::array<std::vector<std::uint8_t>, 4> bytes;
    std::array<btp::DecodedFrame, 4> frames = {};
    for (std::size_t index = 0U; index < frames.size(); ++index) {
        bytes[index] = read_binary(arrival[index]);
        if (bytes[index].empty()) {
            return;
        }
        CHECK(btp::decode(bytes[index].data(), bytes[index].size(),
                          btp::TransportProfile::EspNow,
                          &frames[index]) == btp::Error::Ok);
    }

    btp::ReassemblySlot slots[2];
    std::array<std::uint8_t, 32> storage_a = {};
    std::array<std::uint8_t, 32> storage_b = {};
    const btp::ReassemblyStorage storage[] = {
        {storage_a.data(), storage_a.size()},
        {storage_b.data(), storage_b.size()}
    };
    btp::Reassembler reassembler(slots, storage, 2U, 1000U);
    CHECK(reassembler.valid());
    btp::ReassembledMessage completed = {};

    CHECK(reassembler.push(frames[0], 0U, &completed) ==
          btp::ReassemblyEvent::Accepted);
    CHECK(reassembler.push(frames[1], 1U, &completed) ==
          btp::ReassemblyEvent::Accepted);
    CHECK(reassembler.push(frames[2], 2U, &completed) ==
          btp::ReassemblyEvent::Complete);
    const std::uint8_t expected_a[] = {
        0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U
    };
    CHECK(completed.header.source_id == 0xAAA00001U);
    CHECK(completed.header.flags == 0U);
    CHECK(completed.header.fragment_index == 0U);
    CHECK(completed.header.fragment_count == 1U);
    CHECK(completed.payload.size == sizeof(expected_a));
    CHECK(std::memcmp(completed.payload.data, expected_a,
                      sizeof(expected_a)) == 0);
    CHECK(reassembler.release(completed.slot_index));

    CHECK(reassembler.push(frames[3], 3U, &completed) ==
          btp::ReassemblyEvent::Complete);
    const std::uint8_t expected_b[] = {
        0xa0U, 0xa1U, 0xa2U, 0xa3U, 0xa4U, 0xa5U, 0xa6U, 0xa7U
    };
    CHECK(completed.header.source_id == 0xBBB00002U);
    CHECK(completed.payload.size == sizeof(expected_b));
    CHECK(std::memcmp(completed.payload.data, expected_b,
                      sizeof(expected_b)) == 0);
}

}  // namespace

int main() {
    test_valid_vectors_encode_and_decode();
    test_invalid_vectors_fail_for_documented_reason();
    test_two_sources_interleaved_out_of_order();

    if (failures != 0) {
        std::cerr << failures << " conformance test(s) failed\n";
        return 1;
    }
    std::cout << "All BTP v1 conformance vectors passed\n";
    return 0;
}
