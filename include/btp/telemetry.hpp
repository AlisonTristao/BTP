#ifndef BTP_TELEMETRY_HPP
#define BTP_TELEMETRY_HPP

// The struct <-> bytes half of docs/telemetry.md: a TELEMETRY logical payload
// (schema_version + encoded_body) against a schema the caller already holds.
//
// btp::messages decodes the manifest FieldRecords that *describe* a schema.
// This header decodes and encodes a sample *against* that schema -- the
// PACKED_LE / TLV_LE body, the nullable presence bitmap and the engineering
// conversion (raw * scale + offset). Same guarantees as the rest of the
// library:
//
//   * no internal allocation -- the caller owns every buffer and the schema;
//   * noexcept -- errors are returned, never thrown;
//   * no clock, no I/O, no global state;
//   * no partial output on failure;
//   * decode is lazy / zero-copy -- SampleValue reads an element straight out
//     of the payload buffer and is valid only while that buffer is.
//
// It adds no wire field and changes no octet: docs/telemetry.md already
// specifies every byte here. This is library 2.4.0 territory.
//
// OUT of scope, on purpose:
//   * JSON_UTF8 / CSV_UTF8 bodies -- parsing them needs allocation; body()
//     hands back the raw bytes and the caller parses.
//   * "unknown enum value" (docs/telemetry.md section 14.3) -- SampleValue
//     hands back the raw integer; the caller holds the EnumEntry list (from
//     btp::messages) and decides. A cross-record semantic, the same line
//     btp::messages draws.
//   * schema storage -- the caller keeps the FieldSpec table (fixed capacity,
//     see docs/telemetry.md section 3.1), the same way the dongle's
//     ManifestCache keeps catalog records.
//   * SampleWriter is PACKED_LE only. TLV_LE entries go in field_id order
//     while a caller writes in schema order, which needs buffering; decode
//     (SampleReader) handles both, since that is where interop lives.

#include "btp/codec.hpp"      // ByteView
#include "btp/messages.hpp"   // MessageError, FieldRecord, kField* flag bits

#include <cstddef>
#include <cstdint>

namespace btp {

// ---------------------------------------------------------------------------
// Wire types (docs/telemetry.md section 13)
// ---------------------------------------------------------------------------
// docs/telemetry.md section 13 names the types; the octet values here are the
// ones every existing consumer already uses (they follow the order of that
// table) and are pinned by this library.

enum class WireType : std::uint8_t {
    Uint8 = 0x01U,
    Uint16 = 0x02U,
    Uint32 = 0x03U,
    Uint64 = 0x04U,
    Int8 = 0x05U,
    Int16 = 0x06U,
    Int32 = 0x07U,
    Int64 = 0x08U,
    Float32 = 0x09U,
    Float64 = 0x0AU,
    Bool = 0x0BU,
    Enum8 = 0x0CU,
    Enum16 = 0x0DU,
};

// Octet width of one element of `type`; 0 when `type` is not a defined
// WireType.
std::size_t wire_type_width(std::uint8_t type) noexcept;

// The telemetry body encodings (docs/telemetry.md section 8). SampleReader
// walks PackedLe and TlvLe; for the rest, body() returns the raw bytes.
static const std::uint8_t kEncodingInvalid = 0x00U;
static const std::uint8_t kEncodingOpaqueBytes = 0x01U;
static const std::uint8_t kEncodingUtf8 = 0x02U;
static const std::uint8_t kEncodingJsonUtf8 = 0x03U;
static const std::uint8_t kEncodingCsvUtf8 = 0x04U;
static const std::uint8_t kEncodingPackedLe = 0x05U;
static const std::uint8_t kEncodingTlvLe = 0x06U;

// The largest element count a single field may carry (docs/telemetry.md
// section 7.2: a fixed or variable count fits a uint16, so 65535).
static const std::size_t kMaxElementCount = 65535U;

// ---------------------------------------------------------------------------
// Field descriptor -- the minimum the codec needs
// ---------------------------------------------------------------------------
// A lean subset of btp::FieldRecord: the codec never touches name / unit /
// description / enum entries. A producer with a compiled-in schema fills a
// static FieldSpec[]; a consumer converts each FieldRecord it walked out of a
// MANIFEST_DATA with field_spec() and keeps the FieldSpec[] in its own cache.
//
// `order` must be contiguous from zero across the array, in array order.
// `flags` reuses btp::kFieldNullable (0x01) and btp::kFieldVariableCount
// (0x02).

struct FieldSpec {
    std::uint16_t field_id;           // non-zero, unique within the topic
    std::uint16_t order;              // 0, 1, 2, ... in array order
    std::uint8_t type;               // WireType
    std::uint8_t flags;              // kFieldNullable | kFieldVariableCount
    std::uint16_t element_count;      // fixed count (>= 1); 0 with VARIABLE_COUNT
    std::uint16_t max_element_count;  // upper bound when VARIABLE_COUNT
    double scale;                    // finite; default 1.0
    double offset;                   // finite; default 0.0
};

// Narrows a manifest FieldRecord to what the sample codec uses.
FieldSpec field_spec(const FieldRecord& record) noexcept;

// ---------------------------------------------------------------------------
// Decoded value
// ---------------------------------------------------------------------------
// One field of one sample. Holds no copy of the elements: the accessors read
// element `i` straight from the payload buffer on demand, so a SampleValue is
// valid only while that buffer is. count is 1 for a scalar.

struct SampleValue {
    const FieldSpec* field;
    bool is_null;
    std::uint8_t type;    // == field->type, copied for convenience
    std::uint16_t count;

    // raw * scale + offset for a numeric type; the raw value for bool and
    // enum (scale / offset never apply there -- docs/telemetry.md section
    // 7.1). Reads `type`'s width from the payload at element i.
    double f64(std::size_t index) const noexcept;
    // The raw integer, sign-extended for a signed type, no scaling. For a
    // float type this reinterprets the bit pattern as an integer of the same
    // width -- prefer f64() there.
    std::int64_t i64(std::size_t index) const noexcept;
    std::uint64_t u64(std::size_t index) const noexcept;

    // internal -- set by SampleReader::next
    const std::uint8_t* elements_;
    std::uint8_t width_;
    double scale_;
    double offset_;
};

enum class SampleStep : std::uint8_t { Item, End, Error };

// ---------------------------------------------------------------------------
// SampleReader -- decode
// ---------------------------------------------------------------------------
//   btp::SampleReader r(payload, size, fields, field_count, encoding);
//   btp::SampleValue v;
//   while (r.next(&v) == btp::SampleStep::Item) {
//       if (v.is_null) { /* gap */ continue; }
//       switch (static_cast<btp::WireType>(v.type)) {
//           case btp::WireType::Float32: use(v.f64(0)); break;
//           case btp::WireType::Enum8:   label(*v.field, v.i64(0)); break;
//           ...
//       }
//   }
//   if (r.finish() != btp::MessageError::Ok) { /* reject whole sample */ }
//
// next() yields every schema field exactly once, in `order`, for both
// PACKED_LE and TLV_LE (a TLV null field, which is omitted on the wire, still
// comes back as an is_null Item). A structural fault fails the whole sample:
// docs/telemetry.md section 14.4 forbids a partial decode.

class SampleReader {
public:
    using Step = SampleStep;

    SampleReader(const std::uint8_t* payload, std::size_t size,
                 const FieldSpec* fields, std::size_t field_count,
                 std::uint8_t encoding) noexcept;

    // The uint16_le at payload[0..2). Valid once the constructor's arguments
    // were non-null and size >= 2; 0 otherwise (also recorded as an error).
    std::uint16_t schema_version() const noexcept { return schema_version_; }

    Step next(SampleValue* out) noexcept;
    MessageError finish() noexcept;

    // Everything after the 2-octet schema_version -- the whole encoded_body.
    // For OPAQUE_BYTES / UTF8 / JSON_UTF8 / CSV_UTF8 this is all the caller
    // gets from this layer; it parses the text itself.
    MessageError body(ByteView* out) const noexcept;

    MessageError error() const noexcept { return error_; }

private:
    Step next_packed(SampleValue* out) noexcept;
    Step next_tlv(SampleValue* out) noexcept;
    void prime() noexcept;  // first next(): validate the bitmap / whole TLV body

    const std::uint8_t* payload_;
    std::size_t size_;
    const FieldSpec* fields_;
    std::size_t field_count_;
    std::uint8_t encoding_;
    MessageError error_;
    bool primed_;
    std::uint16_t schema_version_;
    std::size_t body_start_;   // 2 (after schema_version)
    std::size_t cursor_;       // PACKED_LE: read position in the body
    std::size_t bitmap_start_;
    std::size_t bitmap_bytes_;
    std::uint16_t nullable_count_;
    std::uint16_t fields_seen_;
    std::uint16_t nullable_seen_;  // how many nullable fields passed so far
};

// ---------------------------------------------------------------------------
// SampleWriter -- encode (PACKED_LE only)
// ---------------------------------------------------------------------------
//   btp::SampleWriter w(out, capacity, fields, field_count);
//   w.begin(schema_version);
//   w.put_f64(left_speed);        // one call per field, in `order`
//   w.put_null();                 // a nullable field with no reading
//   w.put_array_f64(buf, n);      // a variable-count field
//   std::size_t written = 0;
//   if (w.finish(&written) == btp::MessageError::Ok) send(out, written);
//
// put_f64 takes the engineering value and applies the inverse conversion
// ((value - offset) / scale, rounded for an integer type). put_i64 / put_u64
// take the raw wire value with no scaling -- use them for enum and for a
// field whose raw counts you already have. A call for the wrong type, out of
// order, or a missing field at finish() is MessageError::WrongOrder /
// CountMismatch.

class SampleWriter {
public:
    SampleWriter(std::uint8_t* out, std::size_t capacity,
                 const FieldSpec* fields, std::size_t field_count) noexcept;

    MessageError begin(std::uint16_t schema_version) noexcept;

    MessageError put_f64(double engineering_value) noexcept;
    MessageError put_i64(std::int64_t raw) noexcept;
    MessageError put_u64(std::uint64_t raw) noexcept;
    MessageError put_bool(bool value) noexcept;
    MessageError put_null() noexcept;

    MessageError put_array_f64(const double* values, std::size_t count) noexcept;
    MessageError put_array_i64(const std::int64_t* values, std::size_t count) noexcept;
    MessageError put_array_u64(const std::uint64_t* values, std::size_t count) noexcept;

    MessageError finish(std::size_t* written) noexcept;

    std::size_t size() const noexcept { return cursor_; }

private:
    MessageError write_scalar_f64(const FieldSpec& f, double engineering) noexcept;
    MessageError write_scalar_i64(const FieldSpec& f, std::int64_t raw) noexcept;
    MessageError advance_field(const FieldSpec** out) noexcept;  // next non-null field, checks order
    void set_present(const FieldSpec& f) noexcept;                // flip its bitmap bit to 1

    std::uint8_t* out_;
    std::size_t capacity_;
    const FieldSpec* fields_;
    std::size_t field_count_;
    MessageError error_;
    std::uint8_t state_;       // 0 fresh, 1 begun, 2 done
    std::size_t cursor_;
    std::size_t bitmap_start_;
    std::size_t bitmap_bytes_;
    std::uint16_t nullable_count_;
    std::uint16_t fields_written_;
};

}  // namespace btp

#endif  // BTP_TELEMETRY_HPP
