// Unit tests for btp::messages.
//
//   * the internal cursor (src/messages_detail.hpp) -- included directly, a
//     deliberate white-box test of the one place the "check every declared
//     length before consuming it" rule lives;
//   * every fixed COMMAND / CONTROL payload against the checked-in vectors in
//     test-vectors/v2/messages/, which tools/test_messages.py produced from
//     the same payload_model by an independent implementation.

#include "btp/messages.hpp"

#include "../src/messages_detail.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << __FILE__ << ':' << __LINE__                            \
                      << ": check failed: " #condition << '\n';                 \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

void test_reader_reads_scalars_little_endian() {
    // u8, u16_le, u32_le, u64_le, then f64_le for 1.5 (0x3FF8000000000000).
    const std::uint8_t bytes[] = {
        0x2aU,                                            // u8  = 0x2A
        0x34U, 0x12U,                                     // u16 = 0x1234
        0x78U, 0x56U, 0x34U, 0x12U,                       // u32 = 0x12345678
        0x08U, 0x07U, 0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U,  // u64 = 0x0102030405060708
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xf8U, 0x3fU,  // f64 = 1.5
    };
    btp::detail::Reader reader(bytes, sizeof(bytes));

    CHECK(reader.u8() == 0x2aU);
    CHECK(reader.u16() == 0x1234U);
    CHECK(reader.u32() == 0x12345678U);
    CHECK(reader.u64() == 0x0102030405060708ULL);
    CHECK(reader.f64() == 1.5);
    CHECK(reader.ok());
    CHECK(reader.remaining() == 0U);
    CHECK(reader.require_exhausted() == btp::MessageError::Ok);
}

void test_reader_overflow_is_sticky_and_payload_too_short() {
    const std::uint8_t bytes[] = {0x01U, 0x02U, 0x03U};
    btp::detail::Reader reader(bytes, sizeof(bytes));

    CHECK(reader.u16() == 0x0201U);
    // Only one octet left; a u32 read overflows.
    CHECK(reader.u32() == 0U);
    CHECK(!reader.ok());
    CHECK(reader.error() == btp::MessageError::PayloadTooShort);
    // Sticky: a later valid-looking read stays failed and does not advance.
    CHECK(reader.u8() == 0U);
    CHECK(reader.error() == btp::MessageError::PayloadTooShort);
}

void test_reader_utf8_u16_happy_path() {
    const std::uint8_t bytes[] = {0x05U, 0x00U, 'h', 'e', 'l', 'l', 'o'};
    btp::detail::Reader reader(bytes, sizeof(bytes));

    const btp::ByteView text = reader.utf8_u16();
    CHECK(reader.ok());
    CHECK(text.size == 5U);
    CHECK(std::memcmp(text.data, "hello", 5U) == 0);
    // The view points into the source buffer, past the 2-octet length prefix.
    CHECK(text.data == bytes + 2);
    CHECK(reader.require_exhausted() == btp::MessageError::Ok);
}

void test_reader_utf8_u16_empty_string() {
    const std::uint8_t bytes[] = {0x00U, 0x00U};
    btp::detail::Reader reader(bytes, sizeof(bytes));
    const btp::ByteView text = reader.utf8_u16();
    CHECK(reader.ok());
    CHECK(text.size == 0U);
}

void test_reader_utf8_u16_length_past_buffer_is_length_overflow() {
    // Declares 10 octets but only 3 follow the prefix.
    const std::uint8_t bytes[] = {0x0aU, 0x00U, 'a', 'b', 'c'};
    btp::detail::Reader reader(bytes, sizeof(bytes));
    const btp::ByteView text = reader.utf8_u16();
    CHECK(text.size == 0U);
    CHECK(reader.error() == btp::MessageError::LengthOverflow);
}

void test_reader_utf8_u16_length_past_limit_is_count_too_large() {
    // 200-octet string, buffer physically holds it, but the caller's limit is
    // kMaxNameOrUnit (128).
    std::uint8_t bytes[2U + 200U] = {};
    bytes[0] = 0xc8U;  // 200
    bytes[1] = 0x00U;
    btp::detail::Reader reader(bytes, sizeof(bytes));
    const btp::ByteView text = reader.utf8_u16(btp::kMaxNameOrUnit);
    CHECK(text.size == 0U);
    CHECK(reader.error() == btp::MessageError::CountTooLarge);
}

void test_reader_bytes_u32_happy_and_overflow() {
    const std::uint8_t ok_bytes[] = {0x03U, 0x00U, 0x00U, 0x00U, 0xaaU, 0xbbU, 0xccU};
    btp::detail::Reader ok_reader(ok_bytes, sizeof(ok_bytes));
    const btp::ByteView blob = ok_reader.bytes_u32(btp::kMaxActionBody);
    CHECK(ok_reader.ok());
    CHECK(blob.size == 3U);
    CHECK(blob.data[0] == 0xaaU && blob.data[2] == 0xccU);

    const std::uint8_t bad_bytes[] = {0xffU, 0x00U, 0x00U, 0x00U, 0x01U};
    btp::detail::Reader bad_reader(bad_bytes, sizeof(bad_bytes));
    (void)bad_reader.bytes_u32(btp::kMaxActionBody);
    CHECK(bad_reader.error() == btp::MessageError::LengthOverflow);
}

void test_reader_expect_zero() {
    const std::uint8_t clean[] = {0x00U, 0x00U, 0x00U};
    btp::detail::Reader clean_reader(clean, sizeof(clean));
    clean_reader.expect_zero(3U);
    CHECK(clean_reader.ok());

    const std::uint8_t dirty[] = {0x00U, 0x01U, 0x00U};
    btp::detail::Reader dirty_reader(dirty, sizeof(dirty));
    dirty_reader.expect_zero(3U);
    CHECK(dirty_reader.error() == btp::MessageError::ReservedNotZero);
}

void test_reader_require_exhausted_flags_trailing_bytes() {
    const std::uint8_t bytes[] = {0x01U, 0x02U, 0x03U, 0x04U};
    btp::detail::Reader reader(bytes, sizeof(bytes));
    CHECK(reader.u16() == 0x0201U);
    CHECK(reader.require_exhausted() == btp::MessageError::TrailingBytes);
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

void test_writer_scalars_round_trip_through_reader() {
    std::uint8_t buffer[64] = {};
    btp::detail::Writer writer(buffer, sizeof(buffer));
    writer.u8(0x2aU);
    writer.u16(0x1234U);
    writer.u32(0x12345678U);
    writer.u64(0x0102030405060708ULL);
    writer.f64(-2.25);
    CHECK(writer.ok());
    CHECK(writer.written() == 1U + 2U + 4U + 8U + 8U);

    btp::detail::Reader reader(buffer, writer.written());
    CHECK(reader.u8() == 0x2aU);
    CHECK(reader.u16() == 0x1234U);
    CHECK(reader.u32() == 0x12345678U);
    CHECK(reader.u64() == 0x0102030405060708ULL);
    CHECK(reader.f64() == -2.25);
    CHECK(reader.require_exhausted() == btp::MessageError::Ok);
}

void test_writer_overflow_is_sticky_and_buffer_too_small() {
    std::uint8_t buffer[3] = {};
    btp::detail::Writer writer(buffer, sizeof(buffer));
    writer.u16(0xbeefU);
    writer.u32(0xdeadbeefU);  // only one octet of room -> overflow
    CHECK(!writer.ok());
    CHECK(writer.error() == btp::MessageError::BufferTooSmall);
    writer.u8(0x11U);  // sticky
    CHECK(writer.error() == btp::MessageError::BufferTooSmall);
    CHECK(writer.written() == 2U);
}

void test_writer_utf8_and_bytes_round_trip() {
    std::uint8_t buffer[64] = {};
    btp::detail::Writer writer(buffer, sizeof(buffer));
    const std::uint8_t name[] = {'m', 'o', 't', 'o', 'r'};
    const std::uint8_t blob[] = {0xde, 0xad, 0xbe, 0xef};
    writer.utf8_u16(btp::ByteView{name, sizeof(name)});
    writer.bytes_u32(btp::ByteView{blob, sizeof(blob)}, btp::kMaxActionBody);
    CHECK(writer.ok());

    btp::detail::Reader reader(buffer, writer.written());
    const btp::ByteView read_name = reader.utf8_u16();
    const btp::ByteView read_blob = reader.bytes_u32(btp::kMaxActionBody);
    CHECK(reader.require_exhausted() == btp::MessageError::Ok);
    CHECK(read_name.size == 5U && std::memcmp(read_name.data, "motor", 5U) == 0);
    CHECK(read_blob.size == 4U && std::memcmp(read_blob.data, blob, 4U) == 0);
}

void test_writer_utf8_rejects_over_limit() {
    std::uint8_t buffer[512] = {};
    std::uint8_t big[200] = {};
    btp::detail::Writer writer(buffer, sizeof(buffer));
    writer.utf8_u16(btp::ByteView{big, sizeof(big)}, btp::kMaxNameOrUnit);
    CHECK(writer.error() == btp::MessageError::InvalidArgument);
}

void test_writer_reserve_and_patch_u32() {
    std::uint8_t buffer[32] = {};
    btp::detail::Writer writer(buffer, sizeof(buffer));
    const std::size_t slot = writer.reserve_u32();
    writer.u8(0xaaU);
    writer.u8(0xbbU);
    // Backpatch the slot with the number of content octets written after it.
    writer.patch_u32(slot, static_cast<std::uint32_t>(writer.written() - slot - 4U));
    CHECK(writer.ok());

    btp::detail::Reader reader(buffer, writer.written());
    CHECK(reader.u32() == 2U);
    CHECK(reader.u8() == 0xaaU);
    CHECK(reader.u8() == 0xbbU);
    CHECK(reader.require_exhausted() == btp::MessageError::Ok);
}

// ---------------------------------------------------------------------------
// message_error_string covers every enumerator
// ---------------------------------------------------------------------------

void test_message_error_string_is_total() {
    const btp::MessageError all[] = {
        btp::MessageError::Ok,
        btp::MessageError::InvalidArgument,
        btp::MessageError::BufferTooSmall,
        btp::MessageError::PayloadTooShort,
        btp::MessageError::TrailingBytes,
        btp::MessageError::ReservedNotZero,
        btp::MessageError::LengthOverflow,
        btp::MessageError::ZeroField,
        btp::MessageError::InvalidValue,
        btp::MessageError::CountTooLarge,
        btp::MessageError::RecordSizeMismatch,
        btp::MessageError::CountMismatch,
        btp::MessageError::NotAscending,
        btp::MessageError::UnsupportedFormat,
        btp::MessageError::UnknownObject,
        btp::MessageError::WrongOrder,
    };
    for (std::size_t index = 0U; index < sizeof(all) / sizeof(all[0]); ++index) {
        const char* text = btp::message_error_string(all[index]);
        CHECK(text != nullptr);
        CHECK(text[0] != '\0');
    }
}

// ---------------------------------------------------------------------------
// Vectors (test-vectors/v2/messages/)
// ---------------------------------------------------------------------------

#ifndef BTP_VECTOR_ROOT_V2
#error BTP_VECTOR_ROOT_V2 must point to test-vectors/v2
#endif

std::vector<std::uint8_t> read_vector(const std::string& relative) {
    const std::string path =
        std::string(BTP_VECTOR_ROOT_V2) + "/messages/" + relative;
    std::ifstream stream(path.c_str(), std::ios::binary | std::ios::ate);
    if (!stream) {
        std::cerr << "cannot open message vector: " << path << '\n';
        ++failures;
        return std::vector<std::uint8_t>();
    }
    const std::streamoff end = stream.tellg();
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    stream.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream) {
        std::cerr << "cannot read message vector: " << path << '\n';
        ++failures;
    }
    return bytes;
}

// Encodes `value` and checks it reproduces `expected` byte for byte.
template <typename T, typename EncodeFn>
void check_reencode(EncodeFn encode, const T& value,
                    const std::vector<std::uint8_t>& expected) {
    std::vector<std::uint8_t> buffer(expected.size() + 16U, 0xCCU);
    std::size_t written = 0U;
    CHECK(encode(value, buffer.data(), buffer.size(), &written) ==
          btp::MessageError::Ok);
    CHECK(written == expected.size());
    CHECK(std::memcmp(buffer.data(), expected.data(), expected.size()) == 0);
}

bool ref_equals(const btp::RequestRef& a, const btp::RequestRef& b) {
    return a.request_source_id == b.request_source_id &&
           a.request_boot_id == b.request_boot_id &&
           a.reply_to_sequence == b.reply_to_sequence;
}

const btp::RequestRef kRef = {0x11223344U, 0x55667788U, 9U};
const std::uint8_t kUuid[16] = {0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U,
                                0x66U, 0x77U, 0x88U, 0x99U, 0xaaU, 0xbbU,
                                0xccU, 0xddU, 0xeeU, 0xffU};

void test_vector_hello() {
    const std::vector<std::uint8_t> bytes =
        read_vector("hello/valid/hello_dual_version.bin");
    btp::Hello hello = {};
    CHECK(btp::decode_hello(bytes.data(), bytes.size(), &hello) ==
          btp::MessageError::Ok);
    CHECK(hello.role == 3U);
    CHECK(hello.version_count == 2U);
    CHECK(hello.versions[0] == 1U && hello.versions[1] == 2U);
    CHECK(hello.max_logical_payload == 16384U);
    CHECK(hello.max_inflight_reassemblies == 4U);
    CHECK(hello.max_subscriptions == 16U);
    CHECK(hello.max_dedup_entries == 32U);
    CHECK(hello.session_timeout_ms == 10000U);
    CHECK(std::memcmp(hello.peer_uuid, kUuid, 16U) == 0);
    CHECK(hello.config_revision == 7U);
    check_reencode(btp::encode_hello, hello, bytes);
}

void test_vector_hello_result() {
    const std::vector<std::uint8_t> bytes =
        read_vector("hello_result/valid/hello_result_success.bin");
    btp::HelloResult result = {};
    CHECK(btp::decode_hello_result(bytes.data(), bytes.size(), &result) ==
          btp::MessageError::Ok);
    CHECK(ref_equals(result.request, kRef));
    CHECK(result.status == static_cast<std::uint8_t>(btp::ResultStatus::Success));
    CHECK(result.selected_version == 2U);
    CHECK(result.error_code == 0U);
    CHECK(result.max_logical_payload == 8192U);
    CHECK(result.max_inflight_reassemblies == 2U);
    CHECK(result.max_subscriptions == 8U);
    CHECK(result.max_dedup_entries == 16U);
    CHECK(result.session_timeout_ms == 10000U);
    CHECK(std::memcmp(result.peer_uuid, kUuid, 16U) == 0);
    CHECK(result.config_revision == 1U);
    check_reencode(btp::encode_hello_result, result, bytes);
}

void test_vector_session_close() {
    const std::vector<std::uint8_t> bytes =
        read_vector("session_close/valid/session_close_normal.bin");
    btp::SessionClose close = {};
    CHECK(btp::decode_session_close(bytes.data(), bytes.size(), &close) ==
          btp::MessageError::Ok);
    CHECK(close.reason == static_cast<std::uint8_t>(btp::CloseReason::Normal));
    CHECK(close.drain_timeout_ms == 500U);
    check_reencode(btp::encode_session_close, close, bytes);
}

void test_vector_control_results() {
    const char* files[] = {
        "session_close_result/valid/session_close_result_ok.bin",
        "unsubscribe_result/valid/unsubscribe_result_ok.bin",
    };
    for (std::size_t index = 0U; index < 2U; ++index) {
        const std::vector<std::uint8_t> bytes = read_vector(files[index]);
        btp::ControlResult result = {};
        const btp::MessageError rc =
            (index == 0U)
                ? btp::decode_session_close_result(bytes.data(), bytes.size(), &result)
                : btp::decode_unsubscribe_result(bytes.data(), bytes.size(), &result);
        CHECK(rc == btp::MessageError::Ok);
        CHECK(ref_equals(result.request, kRef));
        CHECK(result.status == 0U);
        CHECK(result.error_code == 0U);
        if (index == 0U) {
            check_reencode(btp::encode_session_close_result, result, bytes);
        } else {
            check_reencode(btp::encode_unsubscribe_result, result, bytes);
        }
    }
}

void test_vector_command_request() {
    const std::vector<std::uint8_t> bytes =
        read_vector("command_request/valid/command_request_shell.bin");
    btp::CommandRequest request = {};
    CHECK(btp::decode_command_request(bytes.data(), bytes.size(), &request) ==
          btp::MessageError::Ok);
    CHECK(request.target_source_id == 0x0C0D0E0FU);
    CHECK(request.target_boot_id == 0x10203040U);
    CHECK(request.action_id == 1U);
    CHECK(request.action_version == 1U);
    CHECK(request.parameters.size == 12U);
    CHECK(std::memcmp(request.parameters.data, "dongle -ping", 12U) == 0);
    check_reencode(btp::encode_command_request, request, bytes);
}

void test_vector_command_result() {
    const std::vector<std::uint8_t> bytes =
        read_vector("command_result/valid/command_result_success.bin");
    btp::CommandResult result = {};
    CHECK(btp::decode_command_result(bytes.data(), bytes.size(), &result) ==
          btp::MessageError::Ok);
    CHECK(ref_equals(result.request, kRef));
    CHECK(result.action_id == 1U);
    CHECK(result.action_version == 1U);
    CHECK(result.status == 0U);
    CHECK(result.error_code == 0U);
    CHECK(result.message.size == 4U);
    CHECK(std::memcmp(result.message.data, "pong", 4U) == 0);
    CHECK(result.result.size == 0U);
    check_reencode(btp::encode_command_result, result, bytes);
}

void test_vector_manifest_request() {
    const std::vector<std::uint8_t> bytes =
        read_vector("manifest_request/valid/manifest_request_targeted.bin");
    btp::ManifestRequest request = {};
    CHECK(btp::decode_manifest_request(bytes.data(), bytes.size(), &request) ==
          btp::MessageError::Ok);
    CHECK(request.target_source_id == 0x11223344U);
    CHECK(request.target_boot_id == 0x55667788U);
    CHECK(request.known_config_revision == 0U);
    check_reencode(btp::encode_manifest_request, request, bytes);
}

void test_vector_subscribe() {
    const std::vector<std::uint8_t> bytes =
        read_vector("subscribe/valid/subscribe_basic.bin");
    btp::Subscribe subscribe = {};
    CHECK(btp::decode_subscribe(bytes.data(), bytes.size(), &subscribe) ==
          btp::MessageError::Ok);
    CHECK(subscribe.target_source_id == 0x11223344U);
    CHECK(subscribe.target_boot_id == 0x55667788U);
    CHECK(subscribe.topic_id == 0x0101U);
    CHECK(subscribe.requested_rate_millihz == 10000U);
    CHECK(subscribe.requested_lease_ms == 30000U);
    check_reencode(btp::encode_subscribe, subscribe, bytes);
}

void test_vector_subscribe_result() {
    const std::vector<std::uint8_t> bytes =
        read_vector("subscribe_result/valid/subscribe_result_success.bin");
    btp::SubscribeResult result = {};
    CHECK(btp::decode_subscribe_result(bytes.data(), bytes.size(), &result) ==
          btp::MessageError::Ok);
    CHECK(ref_equals(result.request, kRef));
    CHECK(result.status == 0U);
    CHECK(result.subscription_id == 0x00A10001U);
    CHECK(result.effective_rate_millihz == 10000U);
    CHECK(result.granted_lease_ms == 30000U);
    check_reencode(btp::encode_subscribe_result, result, bytes);
}

void test_vector_unsubscribe() {
    const std::vector<std::uint8_t> bytes =
        read_vector("unsubscribe/valid/unsubscribe_basic.bin");
    btp::Unsubscribe unsubscribe = {};
    CHECK(btp::decode_unsubscribe(bytes.data(), bytes.size(), &unsubscribe) ==
          btp::MessageError::Ok);
    CHECK(unsubscribe.target_source_id == 0x11223344U);
    CHECK(unsubscribe.target_boot_id == 0x55667788U);
    CHECK(unsubscribe.subscription_id == 0x00A10001U);
    check_reencode(btp::encode_unsubscribe, unsubscribe, bytes);
}

void check_status_counters(const btp::StatusV1& s) {
    CHECK(s.uptime_us == 123456789ULL);
    CHECK(s.frames_rx == 4200U);
    CHECK(s.frames_tx == 3900U);
    CHECK(s.frames_dropped == 5U);
    CHECK(s.crc_errors == 1U);
    CHECK(s.decode_errors == 0U);
    CHECK(s.reassembly_completed == 40U);
    CHECK(s.reassembly_timeouts == 2U);
    CHECK(s.reassembly_rejected == 0U);
    CHECK(s.command_duplicates == 3U);
    CHECK(s.telemetry_dropped == 17U);
}

void test_vector_status_v1() {
    const std::vector<std::uint8_t> bytes =
        read_vector("status/valid/status_v1_counters.bin");
    CHECK(bytes.size() == btp::kStatusV1Size);

    std::uint16_t declared = 0xFFFFU;
    CHECK(btp::status_topic_count(bytes.data(), bytes.size(), &declared) ==
          btp::MessageError::Ok);
    CHECK(declared == 0U);

    btp::StatusV1 base = {};
    btp::TopicStatusRecord topics[4] = {};
    std::size_t written = 7U;
    CHECK(btp::decode_status(bytes.data(), bytes.size(), &base, topics, 4U,
                             &written) == btp::MessageError::Ok);
    CHECK(base.status_version == 1U);
    CHECK(base.flags == btp::kStatusDegraded);
    CHECK(written == 0U);
    check_status_counters(base);

    std::vector<std::uint8_t> buffer(btp::kStatusV1Size, 0xCCU);
    std::size_t out_written = 0U;
    CHECK(btp::encode_status_v1(base, buffer.data(), buffer.size(), &out_written) ==
          btp::MessageError::Ok);
    CHECK(out_written == bytes.size());
    CHECK(buffer == bytes);
}

void test_vector_status_v2() {
    const std::vector<std::uint8_t> bytes =
        read_vector("status/valid/status_v2_two_topics.bin");

    std::uint16_t declared = 0U;
    CHECK(btp::status_topic_count(bytes.data(), bytes.size(), &declared) ==
          btp::MessageError::Ok);
    CHECK(declared == 2U);

    btp::StatusV1 base = {};
    btp::TopicStatusRecord topics[4] = {};
    std::size_t written = 0U;
    CHECK(btp::decode_status(bytes.data(), bytes.size(), &base, topics, 4U,
                             &written) == btp::MessageError::Ok);
    CHECK(base.status_version == 2U);
    CHECK(base.flags == 0U);
    check_status_counters(base);
    CHECK(written == 2U);
    CHECK(topics[0].source_id == 0x11223344U);
    CHECK(topics[0].topic_id == 0x0101U);
    CHECK(topics[0].subscriber_count == 2U);
    CHECK(topics[0].effective_rate_millihz == 10000U);
    CHECK(topics[0].bytes_total == 98304ULL);
    CHECK(topics[0].samples_dropped_total == 12U);
    CHECK(topics[1].topic_id == 0x0102U);
    CHECK(topics[1].subscriber_count == 1U);

    std::vector<std::uint8_t> buffer(bytes.size(), 0xCCU);
    std::size_t out_written = 0U;
    CHECK(btp::encode_status_v2(base, topics, written, buffer.data(),
                                buffer.size(), &out_written) == btp::MessageError::Ok);
    CHECK(out_written == bytes.size());
    CHECK(buffer == bytes);

    // A buffer that cannot hold the declared records is CountTooLarge, not a
    // truncated decode.
    btp::TopicStatusRecord one[1] = {};
    std::size_t small_written = 0U;
    CHECK(btp::decode_status(bytes.data(), bytes.size(), &base, one, 1U,
                             &small_written) == btp::MessageError::CountTooLarge);
}

void test_vector_status_v2_empty() {
    const std::vector<std::uint8_t> bytes =
        read_vector("status/valid/status_v2_no_topics.bin");
    CHECK(bytes.size() == btp::kStatusV1Size + 2U);
    btp::StatusV1 base = {};
    std::size_t written = 9U;
    CHECK(btp::decode_status(bytes.data(), bytes.size(), &base, nullptr, 0U,
                             &written) == btp::MessageError::Ok);
    CHECK(base.status_version == 2U);
    CHECK(written == 0U);
}

void test_invalid_status_vectors() {
    struct Case {
        const char* file;
        btp::MessageError error;
    };
    const Case cases[] = {
        {"status/invalid/status_bad_version.bin", btp::MessageError::UnsupportedFormat},
        {"status/invalid/status_v1_trailing_byte.bin", btp::MessageError::TrailingBytes},
        {"status/invalid/status_v1_truncated.bin", btp::MessageError::PayloadTooShort},
        {"status/invalid/status_v2_count_over_records.bin", btp::MessageError::PayloadTooShort},
        {"status/invalid/status_v2_count_under_records.bin", btp::MessageError::TrailingBytes},
    };
    for (std::size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const std::vector<std::uint8_t> bytes = read_vector(cases[index].file);
        btp::StatusV1 base = {};
        btp::TopicStatusRecord topics[8] = {};
        std::size_t written = 0U;
        const btp::MessageError rc = btp::decode_status(
            bytes.data(), bytes.size(), &base, topics, 8U, &written);
        if (rc != cases[index].error) {
            std::cerr << cases[index].file << ": expected "
                      << btp::message_error_string(cases[index].error) << ", got "
                      << btp::message_error_string(rc) << '\n';
            ++failures;
        }
    }
}

void test_invalid_vectors() {
    struct Case {
        const char* file;
        btp::MessageError error;
        int kind;  // 0 hello, 1 session_close, 2 command_request,
                   // 3 command_result, 4 subscribe_result
    };
    const Case cases[] = {
        {"hello/invalid/hello_truncated.bin", btp::MessageError::PayloadTooShort, 0},
        {"hello/invalid/hello_trailing_byte.bin", btp::MessageError::TrailingBytes, 0},
        {"hello/invalid/hello_flags_reserved.bin", btp::MessageError::ReservedNotZero, 0},
        {"hello/invalid/hello_zero_session_timeout.bin", btp::MessageError::ZeroField, 0},
        {"hello/invalid/hello_bad_role.bin", btp::MessageError::InvalidValue, 0},
        {"hello/invalid/hello_versions_not_ascending.bin", btp::MessageError::NotAscending, 0},
        {"session_close/invalid/session_close_bad_reason.bin", btp::MessageError::InvalidValue, 1},
        {"session_close/invalid/session_close_reserved_set.bin", btp::MessageError::ReservedNotZero, 1},
        {"command_request/invalid/command_request_zero_action_id.bin", btp::MessageError::ZeroField, 2},
        {"command_request/invalid/command_request_parameter_size_over_limit.bin", btp::MessageError::CountTooLarge, 2},
        {"command_request/invalid/command_request_parameter_size_past_payload.bin", btp::MessageError::LengthOverflow, 2},
        {"command_result/invalid/command_result_message_over_limit.bin", btp::MessageError::CountTooLarge, 3},
        {"subscribe_result/invalid/subscribe_result_bad_status.bin", btp::MessageError::InvalidValue, 4},
    };
    for (std::size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const std::vector<std::uint8_t> bytes = read_vector(cases[index].file);
        btp::MessageError rc = btp::MessageError::Ok;
        switch (cases[index].kind) {
            case 0: {
                btp::Hello v = {};
                rc = btp::decode_hello(bytes.data(), bytes.size(), &v);
                break;
            }
            case 1: {
                btp::SessionClose v = {};
                rc = btp::decode_session_close(bytes.data(), bytes.size(), &v);
                break;
            }
            case 2: {
                btp::CommandRequest v = {};
                rc = btp::decode_command_request(bytes.data(), bytes.size(), &v);
                break;
            }
            case 3: {
                btp::CommandResult v = {};
                rc = btp::decode_command_result(bytes.data(), bytes.size(), &v);
                break;
            }
            default: {
                btp::SubscribeResult v = {};
                rc = btp::decode_subscribe_result(bytes.data(), bytes.size(), &v);
                break;
            }
        }
        if (rc != cases[index].error) {
            std::cerr << cases[index].file << ": expected "
                      << btp::message_error_string(cases[index].error) << ", got "
                      << btp::message_error_string(rc) << '\n';
            ++failures;
        }
    }
}

// ---------------------------------------------------------------------------
// MANIFEST_DATA
// ---------------------------------------------------------------------------

using btp::ManifestStep;

// Reads a manifest .bin end to end and rebuilds it with ManifestWriter,
// checking the output is byte-identical. Exercises every reader and writer
// path in one pass.
void check_manifest_roundtrip(const std::vector<std::uint8_t>& bytes) {
    btp::ManifestReader reader(bytes.data(), bytes.size());
    btp::ManifestHeader header = {};
    CHECK(reader.header(&header) == btp::MessageError::Ok);

    std::vector<std::uint8_t> out(bytes.size() + 64U, 0xCCU);
    btp::ManifestWriter writer(out.data(), out.size());
    CHECK(writer.begin(header) == btp::MessageError::Ok);

    btp::SourceInfoEntry si = {};
    for (ManifestStep s = reader.next_source_info(&si); s == ManifestStep::Item;
         s = reader.next_source_info(&si)) {
        CHECK(writer.add_source_info(si) == btp::MessageError::Ok);
    }
    CHECK(reader.error() == btp::MessageError::Ok);

    btp::TopicRecord topic = {};
    btp::ByteView field_bytes = {};
    for (ManifestStep s = reader.next_topic(&topic, &field_bytes);
         s == ManifestStep::Item; s = reader.next_topic(&topic, &field_bytes)) {
        CHECK(writer.begin_topic(topic) == btp::MessageError::Ok);
        btp::FieldRecordReader fields(field_bytes, topic.field_count);
        btp::FieldRecord field = {};
        btp::ByteView enum_bytes = {};
        for (ManifestStep fs = fields.next(&field, &enum_bytes);
             fs == ManifestStep::Item; fs = fields.next(&field, &enum_bytes)) {
            CHECK(writer.add_field(field) == btp::MessageError::Ok);
            btp::EnumEntryReader enums(enum_bytes, field.enum_count);
            btp::EnumEntry entry = {};
            for (ManifestStep es = enums.next(&entry); es == ManifestStep::Item;
                 es = enums.next(&entry)) {
                CHECK(writer.add_enum(entry) == btp::MessageError::Ok);
            }
            CHECK(enums.error() == btp::MessageError::Ok);
        }
        CHECK(fields.error() == btp::MessageError::Ok);
        CHECK(writer.end_topic() == btp::MessageError::Ok);
    }
    CHECK(reader.error() == btp::MessageError::Ok);

    btp::ActionRecord action = {};
    btp::ByteView params = {};
    btp::ByteView results = {};
    btp::ByteView errors = {};
    for (ManifestStep s = reader.next_action(&action, &params, &results, &errors);
         s == ManifestStep::Item;
         s = reader.next_action(&action, &params, &results, &errors)) {
        CHECK(writer.begin_action(action) == btp::MessageError::Ok);

        btp::FieldRecordReader param_fields(params, action.parameter_field_count);
        btp::FieldRecord field = {};
        btp::ByteView enum_bytes = {};
        for (ManifestStep fs = param_fields.next(&field, &enum_bytes);
             fs == ManifestStep::Item;
             fs = param_fields.next(&field, &enum_bytes)) {
            CHECK(writer.add_action_param(field) == btp::MessageError::Ok);
            btp::EnumEntryReader enums(enum_bytes, field.enum_count);
            btp::EnumEntry entry = {};
            for (ManifestStep es = enums.next(&entry); es == ManifestStep::Item;
                 es = enums.next(&entry)) {
                CHECK(writer.add_enum(entry) == btp::MessageError::Ok);
            }
            CHECK(enums.error() == btp::MessageError::Ok);
        }
        CHECK(param_fields.error() == btp::MessageError::Ok);

        btp::FieldRecordReader result_fields(results, action.result_field_count);
        for (ManifestStep fs = result_fields.next(&field, &enum_bytes);
             fs == ManifestStep::Item;
             fs = result_fields.next(&field, &enum_bytes)) {
            CHECK(writer.add_action_result(field) == btp::MessageError::Ok);
            btp::EnumEntryReader enums(enum_bytes, field.enum_count);
            btp::EnumEntry entry = {};
            for (ManifestStep es = enums.next(&entry); es == ManifestStep::Item;
                 es = enums.next(&entry)) {
                CHECK(writer.add_enum(entry) == btp::MessageError::Ok);
            }
            CHECK(enums.error() == btp::MessageError::Ok);
        }
        CHECK(result_fields.error() == btp::MessageError::Ok);

        btp::ActionErrorReader action_errors(errors, action.error_count);
        btp::ActionError action_error = {};
        for (ManifestStep es = action_errors.next(&action_error);
             es == ManifestStep::Item; es = action_errors.next(&action_error)) {
            CHECK(writer.add_action_error(action_error) == btp::MessageError::Ok);
        }
        CHECK(action_errors.error() == btp::MessageError::Ok);
        CHECK(writer.end_action() == btp::MessageError::Ok);
    }
    CHECK(reader.error() == btp::MessageError::Ok);
    CHECK(reader.finish() == btp::MessageError::Ok);

    std::size_t written = 0U;
    CHECK(writer.finish(&written) == btp::MessageError::Ok);
    CHECK(written == bytes.size());
    CHECK(std::memcmp(out.data(), bytes.data(), bytes.size()) == 0);
}

void test_vector_manifest_source_info_only() {
    const std::vector<std::uint8_t> bytes = read_vector(
        "manifest_data/valid/manifest_data_source_info_only.bin");
    btp::ManifestReader reader(bytes.data(), bytes.size());
    btp::ManifestHeader header = {};
    CHECK(reader.header(&header) == btp::MessageError::Ok);
    CHECK(ref_equals(header.request, kRef));
    CHECK(header.status == 0U);
    CHECK((header.flags & btp::kManifestCatalogComplete) != 0U);
    CHECK(header.manifest_format_version == 2U);
    CHECK(header.config_revision == 1U);
    CHECK(header.source_role == static_cast<std::uint8_t>(btp::Role::Producer));
    CHECK((header.source_flags & btp::kSourceOnline) != 0U);
    CHECK(header.topic_count == 0U);
    CHECK(header.action_count == 0U);
    CHECK(header.source_name.size == 11U);
    CHECK(std::memcmp(header.source_name.data, "sensor-node", 11U) == 0);

    const char* keys[] = {"fw_version", "chip", "name"};
    const char* values[] = {"2.1.0-rc1", "ESP32-S3", "lab bench 2"};
    btp::SourceInfoEntry si = {};
    std::size_t seen = 0U;
    for (ManifestStep s = reader.next_source_info(&si); s == ManifestStep::Item;
         s = reader.next_source_info(&si), ++seen) {
        CHECK(seen < 3U);
        CHECK(si.key.size == std::strlen(keys[seen]));
        CHECK(std::memcmp(si.key.data, keys[seen], si.key.size) == 0);
        CHECK(si.value.size == std::strlen(values[seen]));
        CHECK(std::memcmp(si.value.data, values[seen], si.value.size) == 0);
    }
    CHECK(seen == 3U);
    CHECK(reader.error() == btp::MessageError::Ok);

    btp::TopicRecord topic = {};
    btp::ByteView field_bytes = {};
    CHECK(reader.next_topic(&topic, &field_bytes) == ManifestStep::End);
    btp::ActionRecord action = {};
    btp::ByteView p = {}, r = {}, e = {};
    CHECK(reader.next_action(&action, &p, &r, &e) == ManifestStep::End);
    CHECK(reader.finish() == btp::MessageError::Ok);

    check_manifest_roundtrip(bytes);
}

void test_vector_manifest_full() {
    const std::vector<std::uint8_t> bytes =
        read_vector("manifest_data/valid/manifest_data_full.bin");
    btp::ManifestReader reader(bytes.data(), bytes.size());
    btp::ManifestHeader header = {};
    CHECK(reader.header(&header) == btp::MessageError::Ok);
    CHECK(header.topic_count == 1U);
    CHECK(header.action_count == 1U);

    btp::SourceInfoEntry si = {};
    CHECK(reader.next_source_info(&si) == ManifestStep::Item);
    CHECK(reader.next_source_info(&si) == ManifestStep::End);

    btp::TopicRecord topic = {};
    btp::ByteView field_bytes = {};
    CHECK(reader.next_topic(&topic, &field_bytes) == ManifestStep::Item);
    CHECK(topic.topic_id == 0x0101U);
    CHECK(topic.schema_version == 1U);
    CHECK((topic.flags & btp::kTopicSubscribable) != 0U);
    CHECK(topic.field_count == 2U);
    CHECK(std::memcmp(topic.name.data, "motor_state", topic.name.size) == 0);

    btp::FieldRecordReader fields(field_bytes, topic.field_count);
    btp::FieldRecord field = {};
    btp::ByteView enum_bytes = {};
    CHECK(fields.next(&field, &enum_bytes) == ManifestStep::Item);
    CHECK(field.field_id == 1U);
    CHECK(field.enum_count == 0U);
    CHECK(field.scale == 1.0);
    CHECK(std::memcmp(field.name.data, "left_speed", field.name.size) == 0);

    CHECK(fields.next(&field, &enum_bytes) == ManifestStep::Item);
    CHECK(field.field_id == 2U);
    CHECK(field.enum_count == 2U);
    btp::EnumEntryReader enums(enum_bytes, field.enum_count);
    btp::EnumEntry entry = {};
    CHECK(enums.next(&entry) == ManifestStep::Item);
    CHECK(entry.value == 0U);
    CHECK(std::memcmp(entry.label.data, "idle", entry.label.size) == 0);
    CHECK(enums.next(&entry) == ManifestStep::Item);
    CHECK(entry.value == 1U);
    CHECK(std::memcmp(entry.label.data, "run", entry.label.size) == 0);
    CHECK(enums.next(&entry) == ManifestStep::End);

    CHECK(fields.next(&field, &enum_bytes) == ManifestStep::End);
    CHECK(reader.next_topic(&topic, &field_bytes) == ManifestStep::End);

    btp::ActionRecord action = {};
    btp::ByteView params = {}, results = {}, errors = {};
    CHECK(reader.next_action(&action, &params, &results, &errors) == ManifestStep::Item);
    CHECK(action.action_id == 1U);
    CHECK((action.flags & btp::kActionDangerous) != 0U);
    CHECK(action.parameter_field_count == 1U);
    CHECK(action.result_field_count == 0U);
    CHECK(action.error_count == 1U);
    btp::ActionErrorReader action_errors(errors, action.error_count);
    btp::ActionError action_error = {};
    CHECK(action_errors.next(&action_error) == ManifestStep::Item);
    CHECK(action_error.error_code == 0x8001U);
    CHECK(std::memcmp(action_error.label.data, "motor disarmed", action_error.label.size) == 0);
    CHECK(action_errors.next(&action_error) == ManifestStep::End);
    CHECK(reader.next_action(&action, &params, &results, &errors) == ManifestStep::End);
    CHECK(reader.finish() == btp::MessageError::Ok);

    check_manifest_roundtrip(bytes);
}

void test_vector_manifest_not_modified() {
    const std::vector<std::uint8_t> bytes =
        read_vector("manifest_data/valid/manifest_data_not_modified.bin");
    btp::ManifestReader reader(bytes.data(), bytes.size());
    btp::ManifestHeader header = {};
    CHECK(reader.header(&header) == btp::MessageError::Ok);
    CHECK((header.flags & btp::kManifestNotModified) != 0U);
    CHECK(header.manifest_format_version == 1U);
    CHECK(header.topic_count == 0U);
    btp::SourceInfoEntry si = {};
    CHECK(reader.next_source_info(&si) == ManifestStep::End);  // format 1: none
    CHECK(reader.finish() == btp::MessageError::Ok);
    check_manifest_roundtrip(bytes);
}

void test_invalid_manifest_vectors() {
    struct Case {
        const char* file;
        btp::MessageError error;
    };
    const Case cases[] = {
        {"manifest_data/invalid/manifest_bad_format.bin", btp::MessageError::UnsupportedFormat},
        {"manifest_data/invalid/manifest_reserved_set.bin", btp::MessageError::ReservedNotZero},
        {"manifest_data/invalid/manifest_bad_source_role.bin", btp::MessageError::InvalidValue},
        {"manifest_data/invalid/manifest_trailing_byte.bin", btp::MessageError::TrailingBytes},
    };
    for (std::size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const std::vector<std::uint8_t> bytes = read_vector(cases[index].file);
        btp::ManifestReader reader(bytes.data(), bytes.size());
        btp::ManifestHeader header = {};
        btp::MessageError rc = reader.header(&header);
        if (rc == btp::MessageError::Ok) {
            btp::SourceInfoEntry si = {};
            while (reader.next_source_info(&si) == ManifestStep::Item) {
            }
            rc = reader.finish();
        }
        if (rc != cases[index].error) {
            std::cerr << cases[index].file << ": expected "
                      << btp::message_error_string(cases[index].error) << ", got "
                      << btp::message_error_string(rc) << '\n';
            ++failures;
        }
    }
}

void test_manifest_writer_count_mismatch() {
    std::uint8_t buffer[256] = {};
    btp::ManifestHeader header = {};
    header.manifest_format_version = 1U;
    header.status = 0U;
    header.source_role = static_cast<std::uint8_t>(btp::Role::Producer);
    header.topic_count = 1U;   // promises one topic
    header.action_count = 0U;
    btp::ByteView name = {reinterpret_cast<const std::uint8_t*>("x"), 1U};
    header.source_name = name;

    btp::ManifestWriter writer(buffer, sizeof(buffer));
    CHECK(writer.begin(header) == btp::MessageError::Ok);
    std::size_t written = 0U;
    // finish() before the promised topic is written.
    CHECK(writer.finish(&written) == btp::MessageError::CountMismatch);
}

// A non-SUCCESS MANIFEST_DATA (REJECTED / NOT_FOUND) describes no source, so
// source_role is conventionally zero -- valid for a rejected response, invalid
// for a SUCCESS descriptor.
void test_manifest_rejected_response_allows_zero_role() {
    std::uint8_t buffer[128] = {};
    btp::ManifestHeader header = {};
    header.manifest_format_version = 1U;
    header.status = static_cast<std::uint8_t>(btp::ResultStatus::Rejected);
    header.error_code = static_cast<std::uint16_t>(btp::ResultError::NotFound);
    header.source_role = 0U;  // no source to describe
    btp::ByteView name = {reinterpret_cast<const std::uint8_t*>("unknown source"), 14U};
    header.source_name = name;

    btp::ManifestWriter writer(buffer, sizeof(buffer));
    CHECK(writer.begin(header) == btp::MessageError::Ok);
    std::size_t written = 0U;
    CHECK(writer.finish(&written) == btp::MessageError::Ok);

    btp::ManifestReader reader(buffer, written);
    btp::ManifestHeader parsed = {};
    CHECK(reader.header(&parsed) == btp::MessageError::Ok);
    CHECK(parsed.status == static_cast<std::uint8_t>(btp::ResultStatus::Rejected));
    CHECK(parsed.source_role == 0U);
    CHECK(reader.finish() == btp::MessageError::Ok);

    // But a SUCCESS descriptor with role 0 is still rejected.
    header.status = static_cast<std::uint8_t>(btp::ResultStatus::Success);
    btp::ManifestWriter writer2(buffer, sizeof(buffer));
    CHECK(writer2.begin(header) == btp::MessageError::InvalidValue);
}

// ---------------------------------------------------------------------------
// MANIFEST_DATA -- verbatim relay path (raw_source_info / raw_records /
// put_raw_source_info / put_raw_records)
// ---------------------------------------------------------------------------

// Reads a manifest with the raw spans and rebuilds it from those spans alone;
// the output must be byte-identical to the input.
void check_manifest_verbatim_roundtrip(const std::vector<std::uint8_t>& bytes) {
    btp::ManifestReader reader(bytes.data(), bytes.size());
    btp::ManifestHeader header = {};
    CHECK(reader.header(&header) == btp::MessageError::Ok);

    btp::ByteView info = {};
    CHECK(reader.raw_source_info(&info) == btp::MessageError::Ok);
    btp::ByteView topics = {};
    btp::ByteView actions = {};
    CHECK(reader.raw_records(&topics, &actions) == btp::MessageError::Ok);
    CHECK((header.manifest_format_version == 2U) == (info.size >= 2U));

    std::vector<std::uint8_t> out(bytes.size() + 64U, 0xCCU);
    btp::ManifestWriter writer(out.data(), out.size());
    CHECK(writer.begin(header) == btp::MessageError::Ok);
    if (header.manifest_format_version == 2U) {
        CHECK(writer.put_raw_source_info(info) == btp::MessageError::Ok);
    }
    CHECK(writer.put_raw_records(topics, actions) == btp::MessageError::Ok);
    std::size_t written = 0U;
    CHECK(writer.finish(&written) == btp::MessageError::Ok);
    CHECK(written == bytes.size());
    CHECK(std::memcmp(out.data(), bytes.data(), bytes.size()) == 0);
}

void test_manifest_verbatim_roundtrip() {
    check_manifest_verbatim_roundtrip(
        read_vector("manifest_data/valid/manifest_data_full.bin"));
    check_manifest_verbatim_roundtrip(
        read_vector("manifest_data/valid/manifest_data_source_info_only.bin"));
    check_manifest_verbatim_roundtrip(
        read_vector("manifest_data/valid/manifest_data_not_modified.bin"));
}

void test_manifest_verbatim_truncates() {
    const std::vector<std::uint8_t> bytes =
        read_vector("manifest_data/valid/manifest_data_full.bin");
    btp::ManifestReader reader(bytes.data(), bytes.size());
    btp::ManifestHeader header = {};
    CHECK(reader.header(&header) == btp::MessageError::Ok);
    btp::ByteView info = {};
    btp::ByteView topics = {};
    btp::ByteView actions = {};
    CHECK(reader.raw_source_info(&info) == btp::MessageError::Ok);
    CHECK(reader.raw_records(&topics, &actions) == btp::MessageError::Ok);
    CHECK(header.topic_count == 1U);
    CHECK(header.action_count == 1U);

    std::uint8_t scratch[512];

    // header + source_info, no records.
    btp::ManifestWriter base_writer(scratch, sizeof(scratch));
    CHECK(base_writer.begin(header) == btp::MessageError::Ok);
    CHECK(base_writer.put_raw_source_info(info) == btp::MessageError::Ok);
    CHECK(base_writer.put_raw_records(btp::ByteView{nullptr, 0U},
                                     btp::ByteView{nullptr, 0U}) ==
          btp::MessageError::Ok);
    std::size_t base = 0U;
    CHECK(base_writer.finish(&base) == btp::MessageError::Ok);

    // Capacity one octet short of the whole message: the action record is
    // dropped, action_count patched to 0, the topic still fits.
    {
        std::vector<std::uint8_t> out(bytes.size() - 1U, 0xCCU);
        btp::ManifestWriter writer(out.data(), out.size());
        CHECK(writer.begin(header) == btp::MessageError::Ok);
        CHECK(writer.put_raw_source_info(info) == btp::MessageError::Ok);
        CHECK(writer.put_raw_records(topics, actions) == btp::MessageError::Ok);
        std::size_t written = 0U;
        CHECK(writer.finish(&written) == btp::MessageError::Ok);

        btp::ManifestReader back(out.data(), written);
        btp::ManifestHeader parsed = {};
        CHECK(back.header(&parsed) == btp::MessageError::Ok);
        CHECK(parsed.topic_count == 1U);
        CHECK(parsed.action_count == 0U);
        btp::ByteView i2 = {};
        btp::ByteView t2 = {};
        btp::ByteView a2 = {};
        CHECK(back.raw_source_info(&i2) == btp::MessageError::Ok);
        CHECK(back.raw_records(&t2, &a2) == btp::MessageError::Ok);
        CHECK(t2.size == topics.size);
        CHECK(a2.size == 0U);
    }

    // Capacity too small for even the first topic record: both counts 0.
    {
        std::vector<std::uint8_t> out(base + 2U, 0xCCU);
        btp::ManifestWriter writer(out.data(), out.size());
        CHECK(writer.begin(header) == btp::MessageError::Ok);
        CHECK(writer.put_raw_source_info(info) == btp::MessageError::Ok);
        CHECK(writer.put_raw_records(topics, actions) == btp::MessageError::Ok);
        std::size_t written = 0U;
        CHECK(writer.finish(&written) == btp::MessageError::Ok);
        CHECK(written == base);

        btp::ManifestReader back(out.data(), written);
        btp::ManifestHeader parsed = {};
        CHECK(back.header(&parsed) == btp::MessageError::Ok);
        CHECK(parsed.topic_count == 0U);
        CHECK(parsed.action_count == 0U);
    }
}

void test_manifest_verbatim_wrong_order() {
    const std::vector<std::uint8_t> bytes =
        read_vector("manifest_data/valid/manifest_data_full.bin");

    // raw_records once a topic has been walked with next_topic.
    {
        btp::ManifestReader reader(bytes.data(), bytes.size());
        btp::ManifestHeader header = {};
        CHECK(reader.header(&header) == btp::MessageError::Ok);
        btp::TopicRecord topic = {};
        btp::ByteView field_bytes = {};
        CHECK(reader.next_topic(&topic, &field_bytes) == ManifestStep::Item);
        btp::ByteView t = {};
        btp::ByteView a = {};
        CHECK(reader.raw_records(&t, &a) == btp::MessageError::WrongOrder);
    }

    // raw_source_info once raw_records has consumed the payload.
    {
        btp::ManifestReader reader(bytes.data(), bytes.size());
        btp::ManifestHeader header = {};
        CHECK(reader.header(&header) == btp::MessageError::Ok);
        btp::ByteView t = {};
        btp::ByteView a = {};
        CHECK(reader.raw_records(&t, &a) == btp::MessageError::Ok);
        btp::ByteView si = {};
        CHECK(reader.raw_source_info(&si) == btp::MessageError::WrongOrder);
    }

    // put_raw_records after a structured begin_topic.
    {
        std::uint8_t buffer[256] = {};
        btp::ManifestHeader header = {};
        header.manifest_format_version = 1U;
        header.status = 0U;
        header.source_role = static_cast<std::uint8_t>(btp::Role::Producer);
        header.topic_count = 1U;
        const btp::ByteView name = {reinterpret_cast<const std::uint8_t*>("x"), 1U};
        header.source_name = name;

        btp::ManifestWriter writer(buffer, sizeof(buffer));
        CHECK(writer.begin(header) == btp::MessageError::Ok);
        btp::TopicRecord topic = {};
        topic.topic_id = 1U;
        topic.schema_version = 1U;
        topic.name = {reinterpret_cast<const std::uint8_t*>("t"), 1U};
        CHECK(writer.begin_topic(topic) == btp::MessageError::Ok);
        CHECK(writer.put_raw_records(btp::ByteView{nullptr, 0U},
                                    btp::ByteView{nullptr, 0U}) ==
              btp::MessageError::WrongOrder);
    }

    // put_raw_source_info needs the block's own info_count prefix.
    {
        std::uint8_t buffer[256] = {};
        btp::ManifestHeader header = {};
        header.manifest_format_version = 2U;
        header.status = 0U;
        header.source_role = static_cast<std::uint8_t>(btp::Role::Producer);
        const btp::ByteView name = {reinterpret_cast<const std::uint8_t*>("x"), 1U};
        header.source_name = name;

        btp::ManifestWriter writer(buffer, sizeof(buffer));
        CHECK(writer.begin(header) == btp::MessageError::Ok);
        const std::uint8_t one_octet[1] = {0U};
        CHECK(writer.put_raw_source_info(btp::ByteView{one_octet, 1U}) ==
              btp::MessageError::InvalidArgument);
    }
}

// ---------------------------------------------------------------------------
// negotiate() and is_message_object()
// ---------------------------------------------------------------------------

btp::Hello make_hello(std::uint8_t v0, std::uint8_t v1, std::uint32_t mlp,
                      std::uint16_t inflight, std::uint16_t subs,
                      std::uint32_t dedup, std::uint32_t timeout) {
    btp::Hello h = {};
    h.role = static_cast<std::uint8_t>(btp::Role::Consumer);
    if (v0 != 0U) { h.versions[h.version_count++] = v0; }
    if (v1 != 0U) { h.versions[h.version_count++] = v1; }
    h.max_logical_payload = mlp;
    h.max_inflight_reassemblies = inflight;
    h.max_subscriptions = subs;
    h.max_dedup_entries = dedup;
    h.session_timeout_ms = timeout;
    std::memcpy(h.peer_uuid, kUuid, 16U);
    return h;
}

void test_negotiate() {
    const btp::Hello a = make_hello(1U, 2U, 32768U, 8U, 32U, 64U, 15000U);
    const btp::Hello b = make_hello(1U, 2U, 8192U, 4U, 16U, 16U, 30000U);
    const btp::EffectiveLimits e = btp::negotiate(a, b);
    CHECK(e.selected_version == 2U);              // highest both list
    CHECK(e.max_logical_payload == 8192U);        // min
    CHECK(e.max_inflight_reassemblies == 4U);
    CHECK(e.max_subscriptions == 16U);
    CHECK(e.max_dedup_entries == 16U);
    CHECK(e.session_timeout_ms == 15000U);        // min, a is smaller here

    // The larger peer cannot force the smaller past its announced capability.
    const btp::EffectiveLimits swapped = btp::negotiate(b, a);
    CHECK(swapped.max_logical_payload == 8192U);
    CHECK(swapped.selected_version == 2U);

    // Only version 1 in common.
    const btp::Hello only1 = make_hello(1U, 0U, 4096U, 2U, 8U, 8U, 10000U);
    CHECK(btp::negotiate(a, only1).selected_version == 1U);

    // No common version -> everything zero (section 2.3).
    const btp::Hello only3 = make_hello(3U, 0U, 4096U, 2U, 8U, 8U, 10000U);
    const btp::EffectiveLimits none = btp::negotiate(a, only3);
    CHECK(none.selected_version == 0U);
    CHECK(none.max_logical_payload == 0U);
    CHECK(none.session_timeout_ms == 0U);

    // A caller-built Hello with version_count past the versions[] array must not
    // make negotiate() read out of bounds: only the first kMaxAnnouncedVersions
    // entries are considered, the rest of the (over-large) count is ignored.
    btp::Hello overrun = make_hello(1U, 2U, 4096U, 2U, 8U, 8U, 10000U);
    overrun.version_count = 200U;  // versions[2..] stays zero
    const btp::EffectiveLimits clamped = btp::negotiate(overrun, b);
    CHECK(clamped.selected_version == 2U);
    CHECK(clamped.max_logical_payload == 4096U);
    const btp::EffectiveLimits clamped_swapped = btp::negotiate(b, overrun);
    CHECK(clamped_swapped.selected_version == 2U);
}

void test_is_message_object() {
    CHECK(btp::is_message_object(btp::MessageType::Command, btp::object_id::kCommandRequest));
    CHECK(btp::is_message_object(btp::MessageType::Command, btp::object_id::kCommandResult));
    CHECK(!btp::is_message_object(btp::MessageType::Command, 0x0003U));
    CHECK(btp::is_message_object(btp::MessageType::Control, btp::object_id::kHello));
    CHECK(btp::is_message_object(btp::MessageType::Control, btp::object_id::kStatus));
    CHECK(btp::is_message_object(btp::MessageType::Control, btp::object_id::kSessionCloseResult));
    CHECK(!btp::is_message_object(btp::MessageType::Control, 0x000CU));
    CHECK(!btp::is_message_object(btp::MessageType::Terminal, btp::object_id::kTerminalIn));
    CHECK(!btp::is_message_object(btp::MessageType::Telemetry, 0x0101U));
}

void test_decode_rejects_null_and_encode_rejects_small_buffer() {
    btp::Hello hello = {};
    CHECK(btp::decode_hello(nullptr, 0U, &hello) == btp::MessageError::InvalidArgument);
    CHECK(btp::decode_hello(reinterpret_cast<const std::uint8_t*>(""), 0U, nullptr) ==
          btp::MessageError::InvalidArgument);

    // A valid HELLO struct, but the output buffer is one octet short.
    const std::vector<std::uint8_t> bytes =
        read_vector("hello/valid/hello_dual_version.bin");
    btp::Hello decoded = {};
    CHECK(btp::decode_hello(bytes.data(), bytes.size(), &decoded) ==
          btp::MessageError::Ok);
    std::vector<std::uint8_t> tight(bytes.size() - 1U, 0U);
    std::size_t written = 0U;
    CHECK(btp::encode_hello(decoded, tight.data(), tight.size(), &written) ==
          btp::MessageError::BufferTooSmall);
}

}  // namespace

int main() {
    test_reader_reads_scalars_little_endian();
    test_reader_overflow_is_sticky_and_payload_too_short();
    test_reader_utf8_u16_happy_path();
    test_reader_utf8_u16_empty_string();
    test_reader_utf8_u16_length_past_buffer_is_length_overflow();
    test_reader_utf8_u16_length_past_limit_is_count_too_large();
    test_reader_bytes_u32_happy_and_overflow();
    test_reader_expect_zero();
    test_reader_require_exhausted_flags_trailing_bytes();

    test_writer_scalars_round_trip_through_reader();
    test_writer_overflow_is_sticky_and_buffer_too_small();
    test_writer_utf8_and_bytes_round_trip();
    test_writer_utf8_rejects_over_limit();
    test_writer_reserve_and_patch_u32();

    test_message_error_string_is_total();

    test_vector_hello();
    test_vector_hello_result();
    test_vector_session_close();
    test_vector_control_results();
    test_vector_command_request();
    test_vector_command_result();
    test_vector_manifest_request();
    test_vector_subscribe();
    test_vector_subscribe_result();
    test_vector_unsubscribe();
    test_vector_status_v1();
    test_vector_status_v2();
    test_vector_status_v2_empty();
    test_invalid_status_vectors();
    test_vector_manifest_source_info_only();
    test_vector_manifest_full();
    test_vector_manifest_not_modified();
    test_invalid_manifest_vectors();
    test_manifest_writer_count_mismatch();
    test_manifest_rejected_response_allows_zero_role();
    test_manifest_verbatim_roundtrip();
    test_manifest_verbatim_truncates();
    test_manifest_verbatim_wrong_order();
    test_negotiate();
    test_is_message_object();
    test_invalid_vectors();
    test_decode_rejects_null_and_encode_rejects_small_buffer();

    if (failures != 0) {
        std::cerr << failures << " btp::messages test(s) failed\n";
        return 1;
    }
    std::cout << "All btp::messages tests passed\n";
    return 0;
}
