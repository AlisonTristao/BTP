#include "btp/fragmentation.hpp"
#include "btp/stream.hpp"

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

btp::Header header(std::uint32_t source,
                   std::uint32_t sequence = 1U) {
    btp::Header result = {};
    result.type = btp::MessageType::Telemetry;
    result.source_id = source;
    result.boot_id = source + 100U;
    result.sequence = sequence;
    result.timestamp_us = 123456U;
    result.object_id = 7U;
    result.fragment_count = 1U;
    return result;
}

std::vector<std::uint8_t> encode_frame(const btp::Frame& frame) {
    std::size_t size = 0U;
    CHECK(btp::encoded_size(frame.payload.size, btp::kSerialTransport,
                            &size) == btp::Error::Ok);
    std::vector<std::uint8_t> output(size);
    std::size_t written = 0U;
    CHECK(btp::encode(frame, btp::kSerialTransport, output.data(),
                      output.size(), &written) == btp::Error::Ok);
    CHECK(written == output.size());
    return output;
}

std::vector<std::uint8_t> cobs_encode(const std::vector<std::uint8_t>& input) {
    std::size_t capacity = 0U;
    CHECK(btp::cobs_max_encoded_size(input.size(), &capacity) ==
          btp::CobsError::Ok);
    std::vector<std::uint8_t> output(capacity);
    std::size_t written = 0U;
    CHECK(btp::cobs_encode(input.empty() ? nullptr : input.data(), input.size(),
                           output.data(), output.size(), &written) ==
          btp::CobsError::Ok);
    output.resize(written);
    return output;
}

void check_cobs_round_trip(const std::vector<std::uint8_t>& input) {
    const std::vector<std::uint8_t> encoded = cobs_encode(input);
    CHECK(!encoded.empty());
    for (std::size_t index = 0U; index < encoded.size(); ++index) {
        CHECK(encoded[index] != 0U);
    }
    std::vector<std::uint8_t> decoded(input.size() + 1U, 0xA5U);
    std::size_t written = 999U;
    CHECK(btp::cobs_decode(encoded.data(), encoded.size(), decoded.data(),
                           decoded.size(), &written) == btp::CobsError::Ok);
    CHECK(written == input.size());
    CHECK(std::memcmp(decoded.data(), input.data(), input.size()) == 0);
}

void test_cobs() {
    std::size_t maximum_size = 0U;
    CHECK(btp::cobs_max_encoded_size(btp::kSerialMaxFrameSize,
                                     &maximum_size) == btp::CobsError::Ok);
    CHECK(maximum_size == btp::kSerialMaxCobsBlockSize);
    check_cobs_round_trip(std::vector<std::uint8_t>());
    check_cobs_round_trip(std::vector<std::uint8_t>{0U, 0U, 1U, 0U, 0U});

    std::vector<std::uint8_t> every_byte(256U);
    for (std::size_t index = 0U; index < every_byte.size(); ++index) {
        every_byte[index] = static_cast<std::uint8_t>(index);
    }
    check_cobs_round_trip(every_byte);

    std::vector<std::uint8_t> maximum(btp::kSerialMaxFrameSize, 0x55U);
    const std::vector<std::uint8_t> encoded = cobs_encode(maximum);
    CHECK(encoded.size() == btp::kSerialMaxCobsBlockSize);
    check_cobs_round_trip(maximum);

    const std::uint8_t invalid_zero[] = {2U, 0U};
    std::array<std::uint8_t, 8> output;
    output.fill(0xA5U);
    std::size_t written = 77U;
    CHECK(btp::cobs_decode(invalid_zero, sizeof(invalid_zero), output.data(),
                           output.size(), &written) ==
          btp::CobsError::InvalidEncoding);
    CHECK(written == 77U);
    CHECK(output[0] == 0xA5U);

    const std::uint8_t truncated[] = {3U, 1U};
    CHECK(btp::cobs_decode(truncated, sizeof(truncated), output.data(),
                           output.size(), &written) ==
          btp::CobsError::InvalidEncoding);

    const std::uint8_t zero = 0U;
    output.fill(0xA5U);
    written = 77U;
    CHECK(btp::cobs_encode(&zero, 1U, output.data(), 1U, &written) ==
          btp::CobsError::BufferTooSmall);
    CHECK(written == 77U);
    CHECK(output[0] == 0xA5U);

    const std::uint8_t encoded_zero[] = {1U, 1U};
    CHECK(btp::cobs_decode(encoded_zero, sizeof(encoded_zero), output.data(),
                           0U, &written) == btp::CobsError::BufferTooSmall);
    CHECK(written == 77U);
    CHECK(output[0] == 0xA5U);
}

btp::SerialDecodeResult feed(btp::SerialDecoder* decoder,
                             const std::vector<std::uint8_t>& bytes,
                             btp::DecodedFrame* frame,
                             std::size_t* frame_events) {
    btp::SerialDecodeResult last = {btp::SerialDecodeEvent::None,
                                    btp::Error::Ok};
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        last = decoder->push(bytes[index], frame);
        if (last.event == btp::SerialDecodeEvent::Frame) {
            ++*frame_events;
        }
    }
    return last;
}

void test_incremental_serial_decoder() {
    const std::uint8_t payload_bytes[] = {0U, 10U, 13U, 0xFFU};
    const btp::Frame original = {
        header(1U), {payload_bytes, sizeof(payload_bytes)}};
    const std::vector<std::uint8_t> raw = encode_frame(original);
    const std::vector<std::uint8_t> encoded = cobs_encode(raw);

    std::array<std::uint8_t, btp::kSerialMaxCobsBlockSize> encoded_buffer;
    std::array<std::uint8_t, btp::kSerialMaxFrameSize> decoded_buffer;
    btp::SerialDecoder decoder(encoded_buffer.data(), encoded_buffer.size(),
                               decoded_buffer.data(), decoded_buffer.size());
    CHECK(decoder.valid());
    btp::DecodedFrame decoded = {};
    std::size_t events = 0U;

    // Noise is ignored until synchronization. Consecutive delimiters are empty.
    feed(&decoder, std::vector<std::uint8_t>{1U, 2U, 3U, 0U, 0U},
         &decoded, &events);
    CHECK(events == 0U);
    btp::SerialDecodeResult result = feed(&decoder, encoded, &decoded, &events);
    CHECK(result.event == btp::SerialDecodeEvent::None);
    result = decoder.push(0U, &decoded);
    CHECK(result.event == btp::SerialDecodeEvent::Frame);
    ++events;
    CHECK(events == 1U);
    CHECK(decoded.payload.size == sizeof(payload_bytes));
    CHECK(std::memcmp(decoded.payload.data, payload_bytes,
                      sizeof(payload_bytes)) == 0);

    // Invalid COBS consumes only its candidate; its trailing zero resyncs.
    feed(&decoder, std::vector<std::uint8_t>{3U, 1U, 0U}, &decoded, &events);
    CHECK(events == 1U);
    feed(&decoder, encoded, &decoded, &events);
    result = decoder.push(0U, &decoded);
    CHECK(result.event == btp::SerialDecodeEvent::Frame);
    ++events;

    std::vector<std::uint8_t> bad_crc_raw = raw;
    bad_crc_raw.back() ^= 1U;
    const std::vector<std::uint8_t> bad_crc = cobs_encode(bad_crc_raw);
    feed(&decoder, bad_crc, &decoded, &events);
    result = decoder.push(0U, &decoded);
    CHECK(result.event == btp::SerialDecodeEvent::FrameError);
    CHECK(result.frame_error == btp::Error::CrcMismatch);
    CHECK(events == 2U);
    feed(&decoder, encoded, &decoded, &events);
    result = decoder.push(0U, &decoded);
    CHECK(result.event == btp::SerialDecodeEvent::Frame);
    ++events;

    // A partial candidate is rejected and the next complete one is recovered.
    std::vector<std::uint8_t> partial(encoded.begin(),
                                      encoded.begin() + encoded.size() / 2U);
    partial.push_back(0U);
    feed(&decoder, partial, &decoded, &events);
    feed(&decoder, encoded, &decoded, &events);
    result = decoder.push(0U, &decoded);
    CHECK(result.event == btp::SerialDecodeEvent::Frame);
    ++events;

    // 4114 encoded octets overflow exactly once, then discard to delimiter.
    bool overflow_seen = false;
    for (std::size_t index = 0U;
         index < btp::kSerialMaxCobsBlockSize + 1U; ++index) {
        result = decoder.push(1U, &decoded);
        overflow_seen = overflow_seen ||
                        result.event == btp::SerialDecodeEvent::Overflow;
    }
    CHECK(overflow_seen);
    CHECK(decoder.push(0U, &decoded).event == btp::SerialDecodeEvent::None);
    feed(&decoder, encoded, &decoded, &events);
    result = decoder.push(0U, &decoded);
    CHECK(result.event == btp::SerialDecodeEvent::Frame);

    // The largest decoded serial frame is accepted without dynamic growth.
    std::vector<std::uint8_t> maximum_payload(btp::kSerialMaxPayloadSize);
    for (std::size_t index = 0U; index < maximum_payload.size(); ++index) {
        maximum_payload[index] = static_cast<std::uint8_t>(index);
    }
    const btp::Frame maximum_frame = {
        header(3U), {maximum_payload.data(), maximum_payload.size()}};
    const std::vector<std::uint8_t> maximum_encoded =
        cobs_encode(encode_frame(maximum_frame));
    feed(&decoder, maximum_encoded, &decoded, &events);
    result = decoder.push(0U, &decoded);
    CHECK(result.event == btp::SerialDecodeEvent::Frame);
    CHECK(decoded.payload.size == btp::kSerialMaxPayloadSize);
    CHECK(std::memcmp(decoded.payload.data, maximum_payload.data(),
                      maximum_payload.size()) == 0);
}

void test_fragmenter() {
    std::vector<std::uint8_t> payload(500U);
    for (std::size_t index = 0U; index < payload.size(); ++index) {
        payload[index] = static_cast<std::uint8_t>(index);
    }
    std::uint8_t count = 0U;
    CHECK(btp::fragment_count(payload.size(), btp::kEspNowTransport,
                              &count) == btp::Error::Ok);
    CHECK(count == 3U);

    const btp::Header logical_header = header(42U, 99U);
    for (std::uint8_t index = 0U; index < count; ++index) {
        btp::Frame fragment = {};
        CHECK(btp::make_fragment(logical_header,
                                 {payload.data(), payload.size()},
                                 btp::kEspNowTransport, index,
                                 &fragment) == btp::Error::Ok);
        CHECK(fragment.header.source_id == logical_header.source_id);
        CHECK(fragment.header.boot_id == logical_header.boot_id);
        CHECK(fragment.header.sequence == logical_header.sequence);
        CHECK(fragment.header.type == logical_header.type);
        CHECK(fragment.header.fragment_index == index);
        CHECK(fragment.header.fragment_count == count);
        CHECK(fragment.payload.size == (index < 2U ? 210U : 80U));
    }

    CHECK(btp::fragment_count(btp::kEspNowMaxPayloadSize * 255U + 1U,
                              btp::kEspNowTransport, &count) ==
          btp::Error::PayloadTooLarge);
}

void test_usb_hid_transport() {
    // The whole BTP frame (header + payload + CRC) must fit one 64-byte HID
    // report once the mandatory Report ID octet is set aside -- see
    // docs/fragmentation-and-transports.md section 3.3.
    std::vector<std::uint8_t> maximum_payload(btp::kUsbHidMaxPayloadSize);
    for (std::size_t index = 0U; index < maximum_payload.size(); ++index) {
        maximum_payload[index] = static_cast<std::uint8_t>(index);
    }
    const btp::Frame maximum_frame = {
        header(7U), {maximum_payload.data(), maximum_payload.size()}};
    std::size_t size = 0U;
    CHECK(btp::encoded_size(maximum_payload.size(), btp::kUsbHidTransport,
                            &size) == btp::Error::Ok);
    CHECK(size == btp::kUsbHidMaxFrameSize);
    std::array<std::uint8_t, btp::kUsbHidMaxFrameSize> encoded;
    std::size_t written = 0U;
    CHECK(btp::encode(maximum_frame, btp::kUsbHidTransport,
                      encoded.data(), encoded.size(), &written) ==
          btp::Error::Ok);
    CHECK(written == btp::kUsbHidMaxFrameSize);

    btp::DecodedFrame decoded = {};
    CHECK(btp::decode(encoded.data(), encoded.size(), btp::kUsbHidTransport,
                      &decoded) == btp::Error::Ok);
    CHECK(decoded.payload.size == btp::kUsbHidMaxPayloadSize);

    // One octet of payload beyond the report's usable capacity is rejected by
    // the encoder before it ever builds a buffer...
    CHECK(btp::encoded_size(btp::kUsbHidMaxPayloadSize + 1U,
                            btp::kUsbHidTransport, &size) ==
          btp::Error::PayloadTooLarge);

    // ...and a raw buffer one octet past the report ceiling is rejected by
    // the decoder on total size alone, before payload_size is even read.
    std::array<std::uint8_t, btp::kUsbHidMaxFrameSize + 1U> oversized = {};
    std::memcpy(oversized.data(), encoded.data(), encoded.size());
    CHECK(btp::decode(oversized.data(), oversized.size(),
                      btp::kUsbHidTransport, &decoded) ==
          btp::Error::FrameTooLarge);

    // A logical message that does not fit one report's 23 usable octets
    // fragments the same way ESP-NOW does, just with a much smaller ceiling
    // per fragment.
    std::vector<std::uint8_t> logical(50U);
    for (std::size_t index = 0U; index < logical.size(); ++index) {
        logical[index] = static_cast<std::uint8_t>(index);
    }
    std::uint8_t count = 0U;
    CHECK(btp::fragment_count(logical.size(), btp::kUsbHidTransport,
                              &count) == btp::Error::Ok);
    CHECK(count == 3U);  // 22 + 22 + 6

    const btp::Header logical_header = header(8U, 55U);
    for (std::uint8_t index = 0U; index < count; ++index) {
        btp::Frame fragment = {};
        CHECK(btp::make_fragment(logical_header, {logical.data(), logical.size()},
                                 btp::kUsbHidTransport, index,
                                 &fragment) == btp::Error::Ok);
        CHECK(fragment.header.fragment_index == index);
        CHECK(fragment.header.fragment_count == count);
        CHECK(fragment.payload.size == (index < 2U ? 22U : 6U));
    }

    // The maximum logical message this profile can fragment: 255 reports of
    // 23 usable octets each.
    CHECK(btp::fragment_count(btp::kUsbHidMaxPayloadSize * 255U,
                              btp::kUsbHidTransport, &count) ==
          btp::Error::Ok);
    CHECK(count == 255U);
    CHECK(btp::fragment_count(btp::kUsbHidMaxPayloadSize * 255U + 1U,
                              btp::kUsbHidTransport, &count) ==
          btp::Error::PayloadTooLarge);
}

btp::Frame fragment(const btp::Header& logical_header,
                    const std::vector<std::uint8_t>& payload,
                    std::uint8_t index) {
    btp::Frame result = {};
    CHECK(btp::make_fragment(logical_header,
                             {payload.data(), payload.size()},
                             btp::kEspNowTransport, index, &result) ==
          btp::Error::Ok);
    return result;
}

void test_reassembly_interleaved_out_of_order() {
    std::vector<std::uint8_t> first(500U);
    std::vector<std::uint8_t> second(350U);
    for (std::size_t index = 0U; index < first.size(); ++index) {
        first[index] = static_cast<std::uint8_t>(index * 3U);
    }
    for (std::size_t index = 0U; index < second.size(); ++index) {
        second[index] = static_cast<std::uint8_t>(255U - index);
    }

    btp::ReassemblySlot slots[2];
    std::array<std::uint8_t, 500> first_storage;
    std::array<std::uint8_t, 500> second_storage;
    const btp::ReassemblyStorage storage[2] = {
        {first_storage.data(), first_storage.size()},
        {second_storage.data(), second_storage.size()}};
    btp::Reassembler reassembler(slots, storage, 2U, 100U);
    CHECK(reassembler.valid());
    btp::ReassembledMessage message = {};
    const btp::Header first_header = header(1U);
    const btp::Header second_header = header(2U);

    CHECK(reassembler.push(fragment(first_header, first, 2U), 0U, &message) ==
          btp::ReassemblyEvent::Accepted);
    CHECK(reassembler.push(fragment(second_header, second, 1U), 1U, &message) ==
          btp::ReassemblyEvent::Accepted);
    CHECK(reassembler.push(fragment(first_header, first, 2U), 2U, &message) ==
          btp::ReassemblyEvent::Duplicate);
    CHECK(reassembler.push(fragment(first_header, first, 0U), 3U, &message) ==
          btp::ReassemblyEvent::Accepted);
    CHECK(reassembler.push(fragment(second_header, second, 0U), 4U, &message) ==
          btp::ReassemblyEvent::Complete);
    CHECK(message.header.source_id == 2U);
    CHECK(message.header.flags == 0U);
    CHECK(message.header.fragment_count == 1U);
    CHECK(message.payload.size == second.size());
    CHECK(std::memcmp(message.payload.data, second.data(), second.size()) == 0);
    btp::Frame completed_conflict = fragment(second_header, second, 0U);
    completed_conflict.header.fragment_count = 3U;
    CHECK(reassembler.push(completed_conflict, 4U, &message) ==
          btp::ReassemblyEvent::Conflict);
    CHECK(std::memcmp(message.payload.data, second.data(), second.size()) == 0);
    CHECK(reassembler.release(message.slot_index));

    CHECK(reassembler.push(fragment(first_header, first, 1U), 5U, &message) ==
          btp::ReassemblyEvent::Complete);
    CHECK(message.header.source_id == 1U);
    CHECK(message.payload.size == first.size());
    CHECK(std::memcmp(message.payload.data, first.data(), first.size()) == 0);
}

void test_reassembly_conflicts_limits_and_timeout() {
    btp::ReassemblySlot slots[1];
    std::array<std::uint8_t, 100> bytes;
    const btp::ReassemblyStorage storage[1] = {{bytes.data(), bytes.size()}};
    btp::Reassembler reassembler(slots, storage, 1U, 100U);
    btp::ReassembledMessage message = {};

    std::array<std::uint8_t, 60> part_a;
    std::array<std::uint8_t, 60> part_b;
    part_a.fill(1U);
    part_b.fill(2U);
    btp::Header fragmented = header(10U);
    fragmented.flags = btp::kFlagFragmented;
    fragmented.fragment_count = 2U;
    btp::Frame first_part = {fragmented, {part_a.data(), part_a.size()}};
    CHECK(reassembler.push(first_part, 0U, &message) ==
          btp::ReassemblyEvent::Accepted);
    btp::Frame conflicting = first_part;
    conflicting.header.fragment_count = 3U;
    CHECK(reassembler.push(conflicting, 1U, &message) ==
          btp::ReassemblyEvent::Conflict);

    CHECK(reassembler.push(first_part, 2U, &message) ==
          btp::ReassemblyEvent::Accepted);
    btp::Frame changed_duplicate = first_part;
    changed_duplicate.payload = {part_b.data(), part_b.size()};
    CHECK(reassembler.push(changed_duplicate, 3U, &message) ==
          btp::ReassemblyEvent::Conflict);

    CHECK(reassembler.push(first_part, 4U, &message) ==
          btp::ReassemblyEvent::Accepted);
    fragmented.fragment_index = 1U;
    btp::Frame second_part = {fragmented, {part_b.data(), part_b.size()}};
    CHECK(reassembler.push(second_part, 5U, &message) ==
          btp::ReassemblyEvent::MessageTooLarge);

    CHECK(reassembler.push(first_part, 10U, &message) ==
          btp::ReassemblyEvent::Accepted);
    btp::Header other_header = fragmented;
    other_header.source_id = 11U;
    other_header.boot_id = 111U;
    other_header.fragment_index = 0U;
    const btp::Frame other = {other_header, {part_a.data(), part_a.size()}};
    CHECK(reassembler.push(other, 109U, &message) ==
          btp::ReassemblyEvent::NoSlot);
    CHECK(reassembler.push(other, 110U, &message) ==
          btp::ReassemblyEvent::Accepted);

    btp::Frame invalid = other;
    invalid.header.fragment_index = invalid.header.fragment_count;
    CHECK(reassembler.push(invalid, 111U, &message) ==
          btp::ReassemblyEvent::InvalidFragment);
    CHECK(reassembler.expire(210U) == 1U);
}

// Regression test for the exact-equality flags gate that made every fragment
// of a wire v2 message unreceivable: make_fragment() preserves ENCRYPTED and
// CIPHER_ID and ORs FRAGMENTED in, so a fragment of an encrypted message
// carries flags 0x0003 (AES-GCM) or 0x0007 (ChaCha20-Poly1305), never the
// bare 0x0001 the old check demanded.
void test_reassembly_accepts_encrypted_fragments() {
    const std::uint16_t cases[2] = {
        static_cast<std::uint16_t>(btp::kFlagEncrypted),
        static_cast<std::uint16_t>(btp::kFlagEncrypted |
                                   (1U << btp::kCipherIdShift))};

    for (std::size_t index = 0U; index < 2U; ++index) {
        std::vector<std::uint8_t> logical(btp::kEspNowMaxPayloadSize + 40U);
        for (std::size_t byte = 0U; byte < logical.size(); ++byte) {
            logical[byte] = static_cast<std::uint8_t>(byte & 0xFFU);
        }

        btp::Header logical_header = header(0x2000U + static_cast<std::uint32_t>(index));
        logical_header.flags = cases[index];

        std::uint8_t count = 0U;
        CHECK(btp::fragment_count(logical.size(), btp::kEspNowTransport,
                                  &count) == btp::Error::Ok);
        CHECK(count == 2U);

        btp::ReassemblySlot slots[1];
        std::vector<std::uint8_t> storage(logical.size());
        const btp::ReassemblyStorage storage_view = {storage.data(),
                                                     storage.size()};
        btp::Reassembler reassembler(slots, &storage_view, 1U, 1000U);
        CHECK(reassembler.valid());

        btp::ReassembledMessage message = {};
        btp::ReassemblyEvent last = btp::ReassemblyEvent::InvalidArgument;
        for (std::uint8_t part = 0U; part < count; ++part) {
            btp::Frame fragment = {};
            CHECK(btp::make_fragment(logical_header,
                                     {logical.data(), logical.size()},
                                     btp::kEspNowTransport, part,
                                     &fragment) == btp::Error::Ok);
            CHECK((fragment.header.flags & btp::kFlagFragmented) != 0U);
            CHECK((fragment.header.flags & btp::kFlagEncrypted) != 0U);
            last = reassembler.push(fragment, 10U, &message);
        }

        CHECK(last == btp::ReassemblyEvent::Complete);
        CHECK(message.payload.size == logical.size());
        CHECK(std::memcmp(message.payload.data, logical.data(),
                          logical.size()) == 0);
        // Completion normalizes back to the logical header, matching the AAD
        // canonicalization, so ENCRYPTED and CIPHER_ID survive but FRAGMENTED
        // does not.
        CHECK(message.header.flags == cases[index]);
        CHECK(message.header.fragment_index == 0U);
        CHECK(message.header.fragment_count == 1U);
    }
}

}  // namespace

int main() {
    test_cobs();
    test_incremental_serial_decoder();
    test_fragmenter();
    test_usb_hid_transport();
    test_reassembly_interleaved_out_of_order();
    test_reassembly_conflicts_limits_and_timeout();
    test_reassembly_accepts_encrypted_fragments();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All BTP transport tests passed\n";
    return 0;
}
