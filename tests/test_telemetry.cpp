// Unit tests for btp::telemetry -- the TELEMETRY body codec.
//
//   * wire_type_width / field_spec;
//   * PACKED_LE round-trips (SampleWriter <-> SampleReader), with and without
//     nullable fields and arrays;
//   * value policy (non-finite float, bad bool, truncation, trailing bytes);
//   * TLV_LE decode (sparse body, ordering, unknown / missing fields);
//   * body() for a text encoding;
//   * the checked-in vectors in test-vectors/v2/telemetry/.

#include "btp/telemetry.hpp"

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

using btp::FieldSpec;
using btp::MessageError;
using btp::SampleReader;
using btp::SampleStep;
using btp::SampleValue;
using btp::SampleWriter;
using btp::WireType;

FieldSpec spec(std::uint16_t field_id, std::uint16_t order, WireType type,
               std::uint8_t flags = 0U, double scale = 1.0, double offset = 0.0,
               std::uint16_t element_count = 1U, std::uint16_t max_element_count = 0U) {
    FieldSpec s = {};
    s.field_id = field_id;
    s.order = order;
    s.type = static_cast<std::uint8_t>(type);
    s.flags = flags;
    s.element_count = element_count;
    s.max_element_count = max_element_count;
    s.scale = scale;
    s.offset = offset;
    return s;
}

// ---------------------------------------------------------------------------

void test_wire_type_width() {
    CHECK(btp::wire_type_width(static_cast<std::uint8_t>(WireType::Uint8)) == 1U);
    CHECK(btp::wire_type_width(static_cast<std::uint8_t>(WireType::Int16)) == 2U);
    CHECK(btp::wire_type_width(static_cast<std::uint8_t>(WireType::Float32)) == 4U);
    CHECK(btp::wire_type_width(static_cast<std::uint8_t>(WireType::Float64)) == 8U);
    CHECK(btp::wire_type_width(static_cast<std::uint8_t>(WireType::Bool)) == 1U);
    CHECK(btp::wire_type_width(static_cast<std::uint8_t>(WireType::Enum16)) == 2U);
    CHECK(btp::wire_type_width(0x00U) == 0U);
    CHECK(btp::wire_type_width(0x20U) == 0U);
}

void test_field_spec_bridge() {
    btp::FieldRecord r = {};
    r.field_id = 7;
    r.order = 3;
    r.type = static_cast<std::uint8_t>(WireType::Int32);
    r.flags = btp::kFieldNullable;
    r.element_count = 4;
    r.max_element_count = 0;
    r.scale = 0.5;
    r.offset = -1.0;
    const FieldSpec s = btp::field_spec(r);
    CHECK(s.field_id == 7);
    CHECK(s.order == 3);
    CHECK(s.type == static_cast<std::uint8_t>(WireType::Int32));
    CHECK(s.flags == btp::kFieldNullable);
    CHECK(s.element_count == 4);
    CHECK(s.scale == 0.5);
    CHECK(s.offset == -1.0);
}

// ---------------------------------------------------------------------------
// PACKED_LE, no nullable fields
// ---------------------------------------------------------------------------

void test_packed_roundtrip_scalars() {
    const FieldSpec fields[] = {
        spec(1, 0, WireType::Float32),
        spec(2, 1, WireType::Int16, 0U, 0.01, 0.0),  // engineering = raw * 0.01
        spec(3, 2, WireType::Enum8),
        spec(4, 3, WireType::Uint16),
    };
    const std::size_t n = 4U;

    std::uint8_t buffer[64];
    SampleWriter w(buffer, sizeof(buffer), fields, n);
    CHECK(w.begin(1U) == MessageError::Ok);
    CHECK(w.put_f64(12.5) == MessageError::Ok);      // float32
    CHECK(w.put_f64(3.14) == MessageError::Ok);      // int16, raw 314
    CHECK(w.put_i64(1) == MessageError::Ok);         // enum8
    CHECK(w.put_i64(1450) == MessageError::Ok);      // uint16
    std::size_t written = 0U;
    CHECK(w.finish(&written) == MessageError::Ok);
    // 2 (version) + 4 + 2 + 1 + 2
    CHECK(written == 11U);
    // int16 raw landed as 314 = 0x013A little-endian at offset 6
    CHECK(buffer[6] == 0x3AU && buffer[7] == 0x01U);

    SampleReader r(buffer, written, fields, n, btp::kEncodingPackedLe);
    CHECK(r.schema_version() == 1U);

    SampleValue v = {};
    CHECK(r.next(&v) == SampleStep::Item);
    CHECK(v.field->field_id == 1 && !v.is_null && v.count == 1U);
    CHECK(v.f64(0) > 12.49 && v.f64(0) < 12.51);

    CHECK(r.next(&v) == SampleStep::Item);
    CHECK(v.field->field_id == 2);
    CHECK(v.f64(0) > 3.13 && v.f64(0) < 3.15);        // 314 * 0.01
    CHECK(v.i64(0) == 314);                           // raw

    CHECK(r.next(&v) == SampleStep::Item);
    CHECK(v.field->field_id == 3);
    CHECK(v.i64(0) == 1);
    CHECK(v.f64(0) == 1.0);                           // enum: no scale

    CHECK(r.next(&v) == SampleStep::Item);
    CHECK(v.field->field_id == 4 && v.u64(0) == 1450U);

    CHECK(r.next(&v) == SampleStep::End);
    CHECK(r.finish() == MessageError::Ok);
}

void test_packed_signed_and_offset() {
    const FieldSpec fields[] = {
        spec(1, 0, WireType::Int16, 0U, 1.0, 100.0),   // engineering = raw + 100
        spec(2, 1, WireType::Int8),
    };
    std::uint8_t buffer[32];
    SampleWriter w(buffer, sizeof(buffer), fields, 2U);
    CHECK(w.begin(2U) == MessageError::Ok);
    CHECK(w.put_f64(50.0) == MessageError::Ok);   // raw -50
    CHECK(w.put_i64(-7) == MessageError::Ok);
    std::size_t written = 0U;
    CHECK(w.finish(&written) == MessageError::Ok);

    SampleReader r(buffer, written, fields, 2U, btp::kEncodingPackedLe);
    SampleValue v = {};
    CHECK(r.next(&v) == SampleStep::Item);
    CHECK(v.i64(0) == -50);
    CHECK(v.f64(0) == 50.0);
    CHECK(r.next(&v) == SampleStep::Item);
    CHECK(v.i64(0) == -7);
    CHECK(r.finish() == MessageError::Ok);
}

// ---------------------------------------------------------------------------
// PACKED_LE, nullable fields + presence bitmap
// ---------------------------------------------------------------------------

void test_packed_nullable_bitmap() {
    const FieldSpec fields[] = {
        spec(1, 0, WireType::Float32),                       // non-nullable
        spec(2, 1, WireType::Uint16, btp::kFieldNullable),   // nullable A
        spec(3, 2, WireType::Uint16, btp::kFieldNullable),   // nullable B
        spec(4, 3, WireType::Uint8,  btp::kFieldNullable),   // nullable C
    };
    const std::size_t n = 4U;

    std::uint8_t buffer[32];
    SampleWriter w(buffer, sizeof(buffer), fields, n);
    CHECK(w.begin(1U) == MessageError::Ok);
    CHECK(w.put_f64(1.0) == MessageError::Ok);
    CHECK(w.put_i64(500) == MessageError::Ok);   // A present
    CHECK(w.put_null() == MessageError::Ok);     // B null
    CHECK(w.put_i64(9) == MessageError::Ok);     // C present
    std::size_t written = 0U;
    CHECK(w.finish(&written) == MessageError::Ok);
    // bitmap is one octet at offset 2: A=bit0=1, B=bit1=0, C=bit2=1 -> 0b101
    CHECK(buffer[2] == 0x05U);
    // 2 (ver) + 1 (bitmap) + 4 (f32) + 2 (A) + 1 (C)  [B consumes nothing]
    CHECK(written == 10U);

    SampleReader r(buffer, written, fields, n, btp::kEncodingPackedLe);
    SampleValue v = {};
    CHECK(r.next(&v) == SampleStep::Item);  CHECK(!v.is_null && v.field->field_id == 1);
    CHECK(r.next(&v) == SampleStep::Item);  CHECK(!v.is_null && v.u64(0) == 500U);
    CHECK(r.next(&v) == SampleStep::Item);  CHECK(v.is_null && v.field->field_id == 3);
    CHECK(r.next(&v) == SampleStep::Item);  CHECK(!v.is_null && v.u64(0) == 9U);
    CHECK(r.next(&v) == SampleStep::End);
    CHECK(r.finish() == MessageError::Ok);
}

void test_packed_bitmap_reserved_bit_rejected() {
    const FieldSpec fields[] = {
        spec(1, 0, WireType::Uint8, btp::kFieldNullable),
    };
    // version + a bitmap octet with bit 1 set (only bit 0 is a real field).
    const std::uint8_t bytes[] = {0x01U, 0x00U, 0x02U};
    SampleReader r(bytes, sizeof(bytes), fields, 1U, btp::kEncodingPackedLe);
    SampleValue v = {};
    CHECK(r.next(&v) == SampleStep::Error);
    CHECK(r.error() == MessageError::ReservedNotZero);
}

// ---------------------------------------------------------------------------
// Arrays
// ---------------------------------------------------------------------------

void test_packed_fixed_array() {
    const FieldSpec fields[] = {
        spec(1, 0, WireType::Int16, 0U, 1.0, 0.0, /*element_count=*/3),
    };
    const std::int64_t values[] = {10, -20, 30};

    std::uint8_t buffer[32];
    SampleWriter w(buffer, sizeof(buffer), fields, 1U);
    CHECK(w.begin(1U) == MessageError::Ok);
    CHECK(w.put_array_i64(values, 3U) == MessageError::Ok);
    CHECK(w.put_array_i64(values, 2U) == MessageError::CountMismatch);  // wrong count for a fixed array
    std::size_t written = 0U;
    // writer is now stuck on the error above; make a fresh one for the happy path
    SampleWriter w2(buffer, sizeof(buffer), fields, 1U);
    CHECK(w2.begin(1U) == MessageError::Ok);
    CHECK(w2.put_array_i64(values, 3U) == MessageError::Ok);
    CHECK(w2.finish(&written) == MessageError::Ok);
    CHECK(written == 2U + 6U);

    SampleReader r(buffer, written, fields, 1U, btp::kEncodingPackedLe);
    SampleValue v = {};
    CHECK(r.next(&v) == SampleStep::Item);
    CHECK(v.count == 3U);
    CHECK(v.i64(0) == 10 && v.i64(1) == -20 && v.i64(2) == 30);
    CHECK(r.finish() == MessageError::Ok);
}

void test_packed_variable_array() {
    const FieldSpec fields[] = {
        spec(1, 0, WireType::Uint8, btp::kFieldVariableCount, 1.0, 0.0,
             /*element_count=*/0, /*max_element_count=*/8),
    };
    const std::int64_t values[] = {1, 2, 3};

    std::uint8_t buffer[32];
    SampleWriter w(buffer, sizeof(buffer), fields, 1U);
    CHECK(w.begin(1U) == MessageError::Ok);
    CHECK(w.put_array_i64(values, 3U) == MessageError::Ok);
    std::size_t written = 0U;
    CHECK(w.finish(&written) == MessageError::Ok);
    CHECK(written == 2U + 2U + 3U);       // version + count:u16 + 3 bytes
    CHECK(buffer[2] == 0x03U && buffer[3] == 0x00U);

    SampleReader r(buffer, written, fields, 1U, btp::kEncodingPackedLe);
    SampleValue v = {};
    CHECK(r.next(&v) == SampleStep::Item);
    CHECK(v.count == 3U && v.u64(2) == 3U);
    CHECK(r.finish() == MessageError::Ok);

    // A count over max_element_count is rejected.
    std::uint8_t bad[] = {0x01U, 0x00U, 0x09U, 0x00U, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    SampleReader r2(bad, sizeof(bad), fields, 1U, btp::kEncodingPackedLe);
    CHECK(r2.next(&v) == SampleStep::Error);
    CHECK(r2.error() == MessageError::CountTooLarge);
}

// ---------------------------------------------------------------------------
// Value policy
// ---------------------------------------------------------------------------

void test_value_policy() {
    const FieldSpec f32[] = { spec(1, 0, WireType::Float32) };
    // 0x7FC00000 = quiet NaN
    const std::uint8_t nan_bytes[] = {0x01U, 0x00U, 0x00U, 0x00U, 0xC0U, 0x7FU};
    SampleReader r(nan_bytes, sizeof(nan_bytes), f32, 1U, btp::kEncodingPackedLe);
    SampleValue v = {};
    CHECK(r.next(&v) == SampleStep::Error);
    CHECK(r.error() == MessageError::InvalidValue);

    const FieldSpec fb[] = { spec(1, 0, WireType::Bool) };
    const std::uint8_t bad_bool[] = {0x01U, 0x00U, 0x02U};
    SampleReader rb(bad_bool, sizeof(bad_bool), fb, 1U, btp::kEncodingPackedLe);
    CHECK(rb.next(&v) == SampleStep::Error);
    CHECK(rb.error() == MessageError::InvalidValue);

    const FieldSpec fu[] = { spec(1, 0, WireType::Uint32) };
    const std::uint8_t truncated[] = {0x01U, 0x00U, 0x11U, 0x22U};  // needs 4 body bytes, has 2
    SampleReader rt(truncated, sizeof(truncated), fu, 1U, btp::kEncodingPackedLe);
    CHECK(rt.next(&v) == SampleStep::Error);
    CHECK(rt.error() == MessageError::PayloadTooShort);

    const std::uint8_t trailing[] = {0x01U, 0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0xFFU};
    SampleReader rx(trailing, sizeof(trailing), fu, 1U, btp::kEncodingPackedLe);
    CHECK(rx.next(&v) == SampleStep::Item);
    CHECK(rx.finish() == MessageError::TrailingBytes);

    const std::uint8_t too_short[] = {0x01U};
    SampleReader rs(too_short, sizeof(too_short), fu, 1U, btp::kEncodingPackedLe);
    CHECK(rs.error() == MessageError::PayloadTooShort);
}

void test_writer_type_guards() {
    const FieldSpec fb[] = { spec(1, 0, WireType::Bool) };
    std::uint8_t buffer[16];
    SampleWriter w(buffer, sizeof(buffer), fb, 1U);
    CHECK(w.begin(1U) == MessageError::Ok);
    CHECK(w.put_f64(1.0) == MessageError::InvalidValue);   // bool needs put_bool

    const FieldSpec fi[] = { spec(1, 0, WireType::Uint8) };
    SampleWriter w2(buffer, sizeof(buffer), fi, 1U);
    CHECK(w2.begin(1U) == MessageError::Ok);
    CHECK(w2.put_i64(300) == MessageError::InvalidValue);  // 300 does not fit uint8

    // finish() before every field is written
    const FieldSpec ff[] = { spec(1, 0, WireType::Uint8), spec(2, 1, WireType::Uint8) };
    SampleWriter w3(buffer, sizeof(buffer), ff, 2U);
    CHECK(w3.begin(1U) == MessageError::Ok);
    CHECK(w3.put_i64(1) == MessageError::Ok);
    std::size_t written = 0U;
    CHECK(w3.finish(&written) == MessageError::CountMismatch);
}

// ---------------------------------------------------------------------------
// TLV_LE decode
// ---------------------------------------------------------------------------

void test_tlv_sparse() {
    const FieldSpec fields[] = {
        spec(1, 0, WireType::Uint16),
        spec(5, 1, WireType::Int16, btp::kFieldNullable),   // omitted below
        spec(9, 2, WireType::Uint8),
    };
    const std::size_t n = 3U;

    // version 1; entries for field_id 1 and 9, field 5 omitted (null).
    std::vector<std::uint8_t> b = {
        0x01U, 0x00U,
        0x01U, 0x00U, 0x02U, 0x00U, 0x2AU, 0x00U,   // id 1, size 2, value 42
        0x09U, 0x00U, 0x01U, 0x00U, 0x07U,          // id 9, size 1, value 7
    };
    SampleReader r(b.data(), b.size(), fields, n, btp::kEncodingTlvLe);
    SampleValue v = {};
    CHECK(r.next(&v) == SampleStep::Item);  CHECK(v.field->field_id == 1 && v.u64(0) == 42U);
    CHECK(r.next(&v) == SampleStep::Item);  CHECK(v.field->field_id == 5 && v.is_null);
    CHECK(r.next(&v) == SampleStep::Item);  CHECK(v.field->field_id == 9 && v.u64(0) == 7U);
    CHECK(r.next(&v) == SampleStep::End);
    CHECK(r.finish() == MessageError::Ok);
}

void test_tlv_errors() {
    const FieldSpec fields[] = {
        spec(1, 0, WireType::Uint8),
        spec(2, 1, WireType::Uint8),
    };

    // out-of-order field_ids
    std::vector<std::uint8_t> unordered = {
        0x01U, 0x00U,
        0x02U, 0x00U, 0x01U, 0x00U, 0x01U,
        0x01U, 0x00U, 0x01U, 0x00U, 0x02U,
    };
    SampleReader r1(unordered.data(), unordered.size(), fields, 2U, btp::kEncodingTlvLe);
    SampleValue v = {};
    CHECK(r1.next(&v) == SampleStep::Error);
    CHECK(r1.error() == MessageError::NotAscending);

    // unknown field_id
    std::vector<std::uint8_t> unknown = {
        0x01U, 0x00U,
        0x01U, 0x00U, 0x01U, 0x00U, 0x01U,
        0x63U, 0x00U, 0x01U, 0x00U, 0x02U,   // id 99, not in the schema
    };
    SampleReader r2(unknown.data(), unknown.size(), fields, 2U, btp::kEncodingTlvLe);
    CHECK(r2.next(&v) == SampleStep::Error);
    CHECK(r2.error() == MessageError::InvalidValue);

    // missing a non-nullable field (only field 1 present, field 2 required)
    std::vector<std::uint8_t> missing = {
        0x01U, 0x00U,
        0x01U, 0x00U, 0x01U, 0x00U, 0x01U,
    };
    SampleReader r3(missing.data(), missing.size(), fields, 2U, btp::kEncodingTlvLe);
    CHECK(r3.next(&v) == SampleStep::Error);
    CHECK(r3.error() == MessageError::CountMismatch);
}

void test_body_accessor() {
    const FieldSpec none[] = { spec(1, 0, WireType::Uint8) };  // unused for text
    const std::uint8_t bytes[] = {0x02U, 0x00U, 'h', 'i', '!'};
    SampleReader r(bytes, sizeof(bytes), none, 0U, btp::kEncodingUtf8);
    btp::ByteView body = {};
    CHECK(r.body(&body) == MessageError::Ok);
    CHECK(body.size == 3U);
    CHECK(std::memcmp(body.data, "hi!", 3U) == 0);
    // next() on a text encoding just ends
    SampleValue v = {};
    CHECK(r.next(&v) == SampleStep::End);
}

// ---------------------------------------------------------------------------
// Vectors
// ---------------------------------------------------------------------------

#ifdef BTP_VECTOR_ROOT_V2
std::vector<std::uint8_t> read_vector(const std::string& relative) {
    const std::string path =
        std::string(BTP_VECTOR_ROOT_V2) + "/telemetry/" + relative;
    std::ifstream stream(path.c_str(), std::ios::binary | std::ios::ate);
    if (!stream) {
        std::cerr << "cannot open telemetry vector: " << path << '\n';
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
    return bytes;
}

void test_vectors() {
    // packed_motor: version 1, {speed f32, current i16 scale 0.01, mode enum8}
    const FieldSpec motor[] = {
        spec(1, 0, WireType::Float32),
        spec(2, 1, WireType::Int16, 0U, 0.01, 0.0),
        spec(3, 2, WireType::Enum8),
    };
    const std::vector<std::uint8_t> bytes = read_vector("valid/packed_motor.bin");
    if (bytes.empty()) {
        return;
    }
    SampleReader r(bytes.data(), bytes.size(), motor, 3U, btp::kEncodingPackedLe);
    CHECK(r.schema_version() == 1U);
    SampleValue v = {};
    CHECK(r.next(&v) == SampleStep::Item);
    CHECK(v.f64(0) > 12.49 && v.f64(0) < 12.51);
    CHECK(r.next(&v) == SampleStep::Item);
    CHECK(v.i64(0) == 314);
    CHECK(r.next(&v) == SampleStep::Item);
    CHECK(v.i64(0) == 1);
    CHECK(r.next(&v) == SampleStep::End);
    CHECK(r.finish() == MessageError::Ok);
}
#endif

}  // namespace

int main() {
    test_wire_type_width();
    test_field_spec_bridge();
    test_packed_roundtrip_scalars();
    test_packed_signed_and_offset();
    test_packed_nullable_bitmap();
    test_packed_bitmap_reserved_bit_rejected();
    test_packed_fixed_array();
    test_packed_variable_array();
    test_value_policy();
    test_writer_type_guards();
    test_tlv_sparse();
    test_tlv_errors();
    test_body_accessor();
#ifdef BTP_VECTOR_ROOT_V2
    test_vectors();
#endif

    if (failures != 0) {
        std::cerr << failures << " btp::telemetry test(s) failed\n";
        return 1;
    }
    std::cout << "All btp::telemetry tests passed\n";
    return 0;
}
