#include "btp/telemetry.hpp"

#include "messages_detail.hpp"

#include <cstring>

namespace btp {
namespace {

using detail::Reader;
using detail::Writer;

// A double / float is non-finite iff every exponent bit is set. Same trick as
// src/messages.cpp -- avoids <cmath> on the embedded target
// (docs/telemetry.md section 14.1: a non-finite value invalidates the sample).
bool is_finite_f64(double value) noexcept {
    std::uint64_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL;
}
bool is_finite_f32(float value) noexcept {
    std::uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7F800000U) != 0x7F800000U;
}

bool is_signed_type(std::uint8_t type) noexcept {
    switch (static_cast<WireType>(type)) {
        case WireType::Int8:
        case WireType::Int16:
        case WireType::Int32:
        case WireType::Int64:
            return true;
        default:
            return false;
    }
}

bool is_float_type(std::uint8_t type) noexcept {
    return type == static_cast<std::uint8_t>(WireType::Float32) ||
           type == static_cast<std::uint8_t>(WireType::Float64);
}

bool is_scaled_numeric(std::uint8_t type) noexcept {
    // Everything except bool and the two enum types takes scale / offset
    // (docs/telemetry.md section 7.1).
    switch (static_cast<WireType>(type)) {
        case WireType::Bool:
        case WireType::Enum8:
        case WireType::Enum16:
            return false;
        default:
            return wire_type_width(type) != 0U;
    }
}

std::uint64_t read_le(const std::uint8_t* p, std::size_t width) noexcept {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < width; ++index) {
        value |= static_cast<std::uint64_t>(p[index]) << (index * 8U);
    }
    return value;
}

void write_le(std::uint8_t* p, std::uint64_t value, std::size_t width) noexcept {
    for (std::size_t index = 0U; index < width; ++index) {
        p[index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

std::int64_t sign_extend(std::uint64_t raw, std::size_t width) noexcept {
    if (width >= 8U) {
        return static_cast<std::int64_t>(raw);
    }
    const std::uint64_t sign_bit = static_cast<std::uint64_t>(1) << (width * 8U - 1U);
    if ((raw & sign_bit) != 0U) {
        raw |= ~((static_cast<std::uint64_t>(1) << (width * 8U)) - 1U);
    }
    return static_cast<std::int64_t>(raw);
}

// Round half away from zero. Valid for |x| < 2^63, which every wire integer is.
std::int64_t round_to_i64(double x) noexcept {
    if (x >= 0.0) {
        return static_cast<std::int64_t>(x + 0.5);
    }
    return -static_cast<std::int64_t>(-x + 0.5);
}

// True when `raw` fits `type` (signed or unsigned range).
bool fits_type(std::int64_t raw, std::uint8_t type) noexcept {
    const std::size_t width = wire_type_width(type);
    if (width >= 8U) {
        return true;
    }
    if (is_signed_type(type)) {
        const std::int64_t limit = static_cast<std::int64_t>(1) << (width * 8U - 1U);
        return raw >= -limit && raw < limit;
    }
    if (raw < 0) {
        return false;
    }
    const std::uint64_t limit = static_cast<std::uint64_t>(1) << (width * 8U);
    return static_cast<std::uint64_t>(raw) < limit;
}

// Manifest reader / writer states. Kept minimal.
enum : std::uint8_t { kSwFresh = 0U, kSwBegun = 1U, kSwDone = 2U };

}  // namespace

// ---------------------------------------------------------------------------
// wire_type_width / field_spec
// ---------------------------------------------------------------------------

std::size_t wire_type_width(std::uint8_t type) noexcept {
    switch (static_cast<WireType>(type)) {
        case WireType::Uint8:
        case WireType::Int8:
        case WireType::Bool:
        case WireType::Enum8:
            return 1U;
        case WireType::Uint16:
        case WireType::Int16:
        case WireType::Enum16:
            return 2U;
        case WireType::Uint32:
        case WireType::Int32:
        case WireType::Float32:
            return 4U;
        case WireType::Uint64:
        case WireType::Int64:
        case WireType::Float64:
            return 8U;
    }
    return 0U;
}

FieldSpec field_spec(const FieldRecord& record) noexcept {
    FieldSpec spec = {};
    spec.field_id = record.field_id;
    spec.order = record.order;
    spec.type = record.type;
    spec.flags = record.flags;
    spec.element_count = record.element_count;
    spec.max_element_count = record.max_element_count;
    spec.scale = record.scale;
    spec.offset = record.offset;
    return spec;
}

// ---------------------------------------------------------------------------
// SampleValue accessors
// ---------------------------------------------------------------------------

double SampleValue::f64(std::size_t index) const noexcept {
    if (elements_ == nullptr) {
        return 0.0;
    }
    const std::uint8_t* p = elements_ + index * width_;
    if (type == static_cast<std::uint8_t>(WireType::Float32)) {
        const std::uint32_t bits = static_cast<std::uint32_t>(read_le(p, 4U));
        float value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return static_cast<double>(value) * scale_ + offset_;
    }
    if (type == static_cast<std::uint8_t>(WireType::Float64)) {
        const std::uint64_t bits = read_le(p, 8U);
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value * scale_ + offset_;
    }
    const std::uint64_t raw = read_le(p, width_);
    const double numeric = is_signed_type(type)
                               ? static_cast<double>(sign_extend(raw, width_))
                               : static_cast<double>(raw);
    if (!is_scaled_numeric(type)) {
        return numeric;  // bool / enum: raw value, no conversion
    }
    return numeric * scale_ + offset_;
}

std::int64_t SampleValue::i64(std::size_t index) const noexcept {
    if (elements_ == nullptr) {
        return 0;
    }
    const std::uint8_t* p = elements_ + index * width_;
    const std::uint64_t raw = read_le(p, width_);
    if (is_signed_type(type)) {
        return sign_extend(raw, width_);
    }
    return static_cast<std::int64_t>(raw);
}

std::uint64_t SampleValue::u64(std::size_t index) const noexcept {
    if (elements_ == nullptr) {
        return 0U;
    }
    return read_le(elements_ + index * width_, width_);
}

// ---------------------------------------------------------------------------
// SampleReader
// ---------------------------------------------------------------------------

SampleReader::SampleReader(const std::uint8_t* payload, std::size_t size,
                           const FieldSpec* fields, std::size_t field_count,
                           std::uint8_t encoding, SampleLayout layout) noexcept
    : payload_(payload),
      size_(size),
      fields_(fields),
      field_count_(field_count),
      encoding_(encoding),
      error_(MessageError::Ok),
      primed_(false),
      schema_version_(0U),
      body_start_(layout == SampleLayout::BodyOnly ? 0U : 2U),
      cursor_(0U),
      bitmap_start_(0U),
      bitmap_bytes_(0U),
      nullable_count_(0U),
      fields_seen_(0U),
      nullable_seen_(0U) {
    if (payload == nullptr || (fields == nullptr && field_count != 0U)) {
        error_ = MessageError::InvalidArgument;
        return;
    }
    if (field_count > kMaxFieldsPerSide) {
        error_ = MessageError::CountTooLarge;
        return;
    }
    if (size < body_start_) {
        error_ = MessageError::PayloadTooShort;
        return;
    }
    if (body_start_ == 2U) {
        schema_version_ = static_cast<std::uint16_t>(payload[0]) |
                          static_cast<std::uint16_t>(static_cast<std::uint16_t>(payload[1]) << 8U);
    }
    for (std::size_t index = 0U; index < field_count; ++index) {
        if (fields[index].order != index) {
            error_ = MessageError::InvalidArgument;  // caller must pass fields in `order`
            return;
        }
        if ((fields[index].flags & kFieldNullable) != 0U) {
            ++nullable_count_;
        }
        if (wire_type_width(fields[index].type) == 0U) {
            error_ = MessageError::InvalidValue;  // schema names an undefined wire type
            return;
        }
    }
    cursor_ = body_start_;
}

void SampleReader::prime() noexcept {
    primed_ = true;
    if (error_ != MessageError::Ok) {
        return;
    }
    if (encoding_ == kEncodingPackedLe) {
        bitmap_bytes_ = (nullable_count_ + 7U) / 8U;
        bitmap_start_ = body_start_;
        if ((body_start_ + bitmap_bytes_) > size_) {
            error_ = MessageError::PayloadTooShort;
            return;
        }
        // Unused bits in the final bitmap octet must be zero
        // (docs/telemetry.md section 10.1).
        if (bitmap_bytes_ != 0U) {
            const std::uint16_t used = nullable_count_ % 8U;
            if (used != 0U) {
                const std::uint8_t last = payload_[bitmap_start_ + bitmap_bytes_ - 1U];
                const std::uint8_t mask = static_cast<std::uint8_t>(0xFFU << used);
                if ((last & mask) != 0U) {
                    error_ = MessageError::ReservedNotZero;
                    return;
                }
            }
        }
        cursor_ = body_start_ + bitmap_bytes_;
        return;
    }
    if (encoding_ == kEncodingTlvLe) {
        // One structural pass over the whole body: entries ascending by
        // field_id, no repeat, value_size within bounds, no unknown field_id,
        // exact consumption. Presence of every non-nullable field is checked
        // after.
        std::size_t pos = body_start_;
        std::uint32_t previous_id = 0U;
        bool have_previous = false;
        while (pos < size_) {
            if ((pos + 4U) > size_) {
                error_ = MessageError::PayloadTooShort;
                return;
            }
            const std::uint16_t field_id = static_cast<std::uint16_t>(read_le(payload_ + pos, 2U));
            const std::uint16_t value_size =
                static_cast<std::uint16_t>(read_le(payload_ + pos + 2U, 2U));
            if (have_previous && field_id <= previous_id) {
                error_ = MessageError::NotAscending;  // also catches a repeat
                return;
            }
            previous_id = field_id;
            have_previous = true;
            if ((pos + 4U + value_size) > size_) {
                error_ = MessageError::LengthOverflow;
                return;
            }
            const FieldSpec* match = nullptr;
            for (std::size_t index = 0U; index < field_count_; ++index) {
                if (fields_[index].field_id == field_id) {
                    match = &fields_[index];
                    break;
                }
            }
            if (match == nullptr) {
                error_ = MessageError::InvalidValue;  // unknown field -> reject (section 11)
                return;
            }
            pos += 4U + value_size;
        }
        if (pos != size_) {
            error_ = MessageError::TrailingBytes;
            return;
        }
        for (std::size_t index = 0U; index < field_count_; ++index) {
            if ((fields_[index].flags & kFieldNullable) != 0U) {
                continue;
            }
            bool present = false;
            std::size_t scan = body_start_;
            while (scan < size_) {
                const std::uint16_t id = static_cast<std::uint16_t>(read_le(payload_ + scan, 2U));
                const std::uint16_t vs =
                    static_cast<std::uint16_t>(read_le(payload_ + scan + 2U, 2U));
                if (id == fields_[index].field_id) {
                    present = true;
                    break;
                }
                scan += 4U + vs;
            }
            if (!present) {
                error_ = MessageError::CountMismatch;  // missing non-nullable field
                return;
            }
        }
        return;
    }
    // A text / opaque encoding: nothing to walk. next() returns End; the
    // caller uses body().
}

SampleReader::Step SampleReader::next(SampleValue* out) noexcept {
    if (!primed_) {
        prime();
    }
    if (error_ != MessageError::Ok) {
        return Step::Error;
    }
    if (out == nullptr) {
        error_ = MessageError::InvalidArgument;
        return Step::Error;
    }
    if (encoding_ == kEncodingPackedLe) {
        return next_packed(out);
    }
    if (encoding_ == kEncodingTlvLe) {
        return next_tlv(out);
    }
    return Step::End;
}

namespace {

// Reads one run of `count` elements of `type` starting at payload+pos, bound
// by `size`. Validates width, byte count, bool domain and float finiteness.
// On success advances `pos` and fills the SampleValue element view.
MessageError read_elements(const std::uint8_t* payload, std::size_t size,
                           std::size_t* pos, std::uint8_t type,
                           std::uint16_t count, SampleValue* out) noexcept {
    const std::size_t width = wire_type_width(type);
    if (width == 0U) {
        return MessageError::InvalidValue;
    }
    const std::size_t need = width * static_cast<std::size_t>(count);
    if ((*pos + need) > size) {
        return MessageError::PayloadTooShort;
    }
    const std::uint8_t* base = payload + *pos;
    for (std::uint16_t element = 0U; element < count; ++element) {
        const std::uint8_t* p = base + static_cast<std::size_t>(element) * width;
        if (type == static_cast<std::uint8_t>(WireType::Bool)) {
            if (p[0] > 1U) {
                return MessageError::InvalidValue;
            }
        } else if (type == static_cast<std::uint8_t>(WireType::Float32)) {
            std::uint32_t bits = static_cast<std::uint32_t>(read_le(p, 4U));
            float value = 0.0F;
            std::memcpy(&value, &bits, sizeof(value));
            if (!is_finite_f32(value)) {
                return MessageError::InvalidValue;
            }
        } else if (type == static_cast<std::uint8_t>(WireType::Float64)) {
            std::uint64_t bits = read_le(p, 8U);
            double value = 0.0;
            std::memcpy(&value, &bits, sizeof(value));
            if (!is_finite_f64(value)) {
                return MessageError::InvalidValue;
            }
        }
    }
    out->elements_ = base;
    out->width_ = static_cast<std::uint8_t>(width);
    out->count = count;
    *pos += need;
    return MessageError::Ok;
}

void fill_null(SampleValue* out, const FieldSpec& field) noexcept {
    out->field = &field;
    out->is_null = true;
    out->type = field.type;
    out->count = 0U;
    out->elements_ = nullptr;
    out->width_ = 0U;
    out->scale_ = field.scale;
    out->offset_ = field.offset;
}

}  // namespace

SampleReader::Step SampleReader::next_packed(SampleValue* out) noexcept {
    if (fields_seen_ == field_count_) {
        return Step::End;
    }
    const FieldSpec& field = fields_[fields_seen_];
    const bool nullable = (field.flags & kFieldNullable) != 0U;
    bool is_null = false;
    if (nullable) {
        const std::uint16_t bit = nullable_seen_++;
        const std::uint8_t octet = payload_[bitmap_start_ + (bit / 8U)];
        is_null = ((octet >> (bit % 8U)) & 0x01U) == 0U;
    }
    if (is_null) {
        fill_null(out, field);
        ++fields_seen_;
        return Step::Item;
    }

    std::uint16_t count = field.element_count;
    if ((field.flags & kFieldVariableCount) != 0U) {
        if ((cursor_ + 2U) > size_) {
            error_ = MessageError::PayloadTooShort;
            return Step::Error;
        }
        count = static_cast<std::uint16_t>(read_le(payload_ + cursor_, 2U));
        cursor_ += 2U;
        if (count > field.max_element_count) {
            error_ = MessageError::CountTooLarge;
            return Step::Error;
        }
    } else if (count == 0U) {
        error_ = MessageError::InvalidArgument;  // malformed schema: fixed field, count 0
        return Step::Error;
    }

    out->field = &field;
    out->is_null = false;
    out->type = field.type;
    out->scale_ = field.scale;
    out->offset_ = field.offset;
    const MessageError rc = read_elements(payload_, size_, &cursor_, field.type, count, out);
    if (rc != MessageError::Ok) {
        error_ = rc;
        return Step::Error;
    }
    ++fields_seen_;
    return Step::Item;
}

SampleReader::Step SampleReader::next_tlv(SampleValue* out) noexcept {
    if (fields_seen_ == field_count_) {
        return Step::End;
    }
    const FieldSpec& field = fields_[fields_seen_++];

    // Find this field's entry in the (already validated) TLV body.
    std::size_t pos = body_start_;
    const std::uint8_t* value = nullptr;
    std::uint16_t value_size = 0U;
    while (pos < size_) {
        const std::uint16_t id = static_cast<std::uint16_t>(read_le(payload_ + pos, 2U));
        const std::uint16_t vs = static_cast<std::uint16_t>(read_le(payload_ + pos + 2U, 2U));
        if (id == field.field_id) {
            value = payload_ + pos + 4U;
            value_size = vs;
            break;
        }
        pos += 4U + vs;
    }
    if (value == nullptr) {
        fill_null(out, field);  // omitted -> null (prime validated it is nullable)
        return Step::Item;
    }

    std::uint16_t count = field.element_count;
    std::size_t value_pos = 0U;
    if ((field.flags & kFieldVariableCount) != 0U) {
        if (value_size < 2U) {
            error_ = MessageError::PayloadTooShort;
            return Step::Error;
        }
        count = static_cast<std::uint16_t>(read_le(value, 2U));
        value_pos = 2U;
        if (count > field.max_element_count) {
            error_ = MessageError::CountTooLarge;
            return Step::Error;
        }
    } else if (count == 0U) {
        error_ = MessageError::InvalidArgument;
        return Step::Error;
    }

    out->field = &field;
    out->is_null = false;
    out->type = field.type;
    out->scale_ = field.scale;
    out->offset_ = field.offset;
    std::size_t rel = value_pos;
    const MessageError rc = read_elements(value, value_size, &rel, field.type, count, out);
    if (rc != MessageError::Ok) {
        error_ = rc;
        return Step::Error;
    }
    if (rel != value_size) {
        error_ = MessageError::TrailingBytes;  // value_size did not match the element run
        return Step::Error;
    }
    return Step::Item;
}

MessageError SampleReader::finish() noexcept {
    if (!primed_) {
        prime();
    }
    if (error_ != MessageError::Ok) {
        return error_;
    }
    SampleValue scratch = {};
    while (fields_seen_ < field_count_) {
        if (next(&scratch) == Step::Error) {
            return error_;
        }
    }
    if (encoding_ == kEncodingPackedLe && cursor_ != size_) {
        error_ = MessageError::TrailingBytes;
        return error_;
    }
    return MessageError::Ok;
}

MessageError SampleReader::body(ByteView* out) const noexcept {
    if (out == nullptr) {
        return MessageError::InvalidArgument;
    }
    if (payload_ == nullptr || size_ < 2U) {
        return MessageError::PayloadTooShort;
    }
    *out = ByteView{payload_ + body_start_, size_ - body_start_};
    return MessageError::Ok;
}

// ---------------------------------------------------------------------------
// SampleWriter (PACKED_LE only)
// ---------------------------------------------------------------------------

SampleWriter::SampleWriter(std::uint8_t* out, std::size_t capacity,
                           const FieldSpec* fields, std::size_t field_count) noexcept
    : out_(out),
      capacity_(capacity),
      fields_(fields),
      field_count_(field_count),
      error_(MessageError::Ok),
      state_(kSwFresh),
      cursor_(0U),
      bitmap_start_(0U),
      bitmap_bytes_(0U),
      nullable_count_(0U),
      fields_written_(0U) {
    if (out == nullptr || (fields == nullptr && field_count != 0U)) {
        error_ = MessageError::InvalidArgument;
        return;
    }
    if (field_count > kMaxFieldsPerSide) {
        error_ = MessageError::CountTooLarge;
        return;
    }
    for (std::size_t index = 0U; index < field_count; ++index) {
        if (fields[index].order != index) {
            error_ = MessageError::InvalidArgument;
            return;
        }
        if (wire_type_width(fields[index].type) == 0U) {
            error_ = MessageError::InvalidValue;
            return;
        }
        if (!is_finite_f64(fields[index].scale) || !is_finite_f64(fields[index].offset)) {
            error_ = MessageError::InvalidValue;
        }
        if ((fields[index].flags & kFieldNullable) != 0U) {
            ++nullable_count_;
        }
    }
}

MessageError SampleWriter::begin(std::uint16_t schema_version, SampleLayout layout) noexcept {
    if (error_ != MessageError::Ok) {
        return error_;
    }
    if (state_ != kSwFresh) {
        error_ = MessageError::WrongOrder;
        return error_;
    }
    const std::size_t prefix = (layout == SampleLayout::BodyOnly) ? 0U : 2U;
    bitmap_bytes_ = (nullable_count_ + 7U) / 8U;
    if ((prefix + bitmap_bytes_) > capacity_) {
        error_ = MessageError::BufferTooSmall;
        return error_;
    }
    if (prefix == 2U) {
        out_[0] = static_cast<std::uint8_t>(schema_version);
        out_[1] = static_cast<std::uint8_t>(schema_version >> 8U);
    }
    bitmap_start_ = prefix;
    for (std::size_t index = 0U; index < bitmap_bytes_; ++index) {
        out_[bitmap_start_ + index] = 0U;
    }
    cursor_ = prefix + bitmap_bytes_;
    state_ = kSwBegun;
    return MessageError::Ok;
}

MessageError SampleWriter::advance_field(const FieldSpec** out) noexcept {
    if (error_ != MessageError::Ok) {
        return error_;
    }
    if (state_ != kSwBegun) {
        error_ = MessageError::WrongOrder;
        return error_;
    }
    if (fields_written_ >= field_count_) {
        error_ = MessageError::CountMismatch;  // more put_* calls than fields
        return error_;
    }
    *out = &fields_[fields_written_];
    return MessageError::Ok;
}

void SampleWriter::set_present(const FieldSpec& field) noexcept {
    if ((field.flags & kFieldNullable) == 0U) {
        return;
    }
    std::uint16_t bit = 0U;
    for (std::size_t index = 0U; index < field.order; ++index) {
        if ((fields_[index].flags & kFieldNullable) != 0U) {
            ++bit;
        }
    }
    out_[bitmap_start_ + (bit / 8U)] |= static_cast<std::uint8_t>(1U << (bit % 8U));
}

MessageError SampleWriter::write_scalar_i64(const FieldSpec& field, std::int64_t raw) noexcept {
    const std::size_t width = wire_type_width(field.type);
    if (!fits_type(raw, field.type)) {
        error_ = MessageError::InvalidValue;
        return error_;
    }
    if ((cursor_ + width) > capacity_) {
        error_ = MessageError::BufferTooSmall;
        return error_;
    }
    write_le(out_ + cursor_, static_cast<std::uint64_t>(raw), width);
    cursor_ += width;
    return MessageError::Ok;
}

MessageError SampleWriter::write_scalar_f64(const FieldSpec& field, double engineering) noexcept {
    const std::uint8_t type = field.type;
    if (type == static_cast<std::uint8_t>(WireType::Float32)) {
        const float value = static_cast<float>((engineering - field.offset) / field.scale);
        if (!is_finite_f32(value)) {
            error_ = MessageError::InvalidValue;
            return error_;
        }
        if ((cursor_ + 4U) > capacity_) {
            error_ = MessageError::BufferTooSmall;
            return error_;
        }
        std::uint32_t bits = 0U;
        std::memcpy(&bits, &value, sizeof(bits));
        write_le(out_ + cursor_, bits, 4U);
        cursor_ += 4U;
        return MessageError::Ok;
    }
    if (type == static_cast<std::uint8_t>(WireType::Float64)) {
        const double value = (engineering - field.offset) / field.scale;
        if (!is_finite_f64(value)) {
            error_ = MessageError::InvalidValue;
            return error_;
        }
        if ((cursor_ + 8U) > capacity_) {
            error_ = MessageError::BufferTooSmall;
            return error_;
        }
        std::uint64_t bits = 0U;
        std::memcpy(&bits, &value, sizeof(bits));
        write_le(out_ + cursor_, bits, 8U);
        cursor_ += 8U;
        return MessageError::Ok;
    }
    // integer with an engineering scale
    const double raw = (engineering - field.offset) / field.scale;
    if (!is_finite_f64(raw)) {
        error_ = MessageError::InvalidValue;
        return error_;
    }
    return write_scalar_i64(field, round_to_i64(raw));
}

MessageError SampleWriter::put_f64(double engineering_value) noexcept {
    const FieldSpec* field = nullptr;
    if (advance_field(&field) != MessageError::Ok) {
        return error_;
    }
    if (field->type == static_cast<std::uint8_t>(WireType::Bool) ||
        field->type == static_cast<std::uint8_t>(WireType::Enum8) ||
        field->type == static_cast<std::uint8_t>(WireType::Enum16)) {
        error_ = MessageError::InvalidValue;  // use put_bool / put_i64
        return error_;
    }
    if (field->element_count != 1U || (field->flags & kFieldVariableCount) != 0U) {
        error_ = MessageError::WrongOrder;  // use put_array_f64
        return error_;
    }
    if (write_scalar_f64(*field, engineering_value) != MessageError::Ok) {
        return error_;
    }
    set_present(*field);
    ++fields_written_;
    return MessageError::Ok;
}

MessageError SampleWriter::put_i64(std::int64_t raw) noexcept {
    const FieldSpec* field = nullptr;
    if (advance_field(&field) != MessageError::Ok) {
        return error_;
    }
    if (is_float_type(field->type) ||
        field->type == static_cast<std::uint8_t>(WireType::Bool)) {
        error_ = MessageError::InvalidValue;
        return error_;
    }
    if (field->element_count != 1U || (field->flags & kFieldVariableCount) != 0U) {
        error_ = MessageError::WrongOrder;
        return error_;
    }
    if (write_scalar_i64(*field, raw) != MessageError::Ok) {
        return error_;
    }
    set_present(*field);
    ++fields_written_;
    return MessageError::Ok;
}

MessageError SampleWriter::put_u64(std::uint64_t raw) noexcept {
    if (raw >= (static_cast<std::uint64_t>(1) << 63U)) {
        // Only an unsigned 64-bit field can hold this; write it directly.
        const FieldSpec* field = nullptr;
        if (advance_field(&field) != MessageError::Ok) {
            return error_;
        }
        if (field->type != static_cast<std::uint8_t>(WireType::Uint64) ||
            field->element_count != 1U) {
            error_ = MessageError::InvalidValue;
            return error_;
        }
        if ((cursor_ + 8U) > capacity_) {
            error_ = MessageError::BufferTooSmall;
            return error_;
        }
        write_le(out_ + cursor_, raw, 8U);
        cursor_ += 8U;
        set_present(*field);
        ++fields_written_;
        return MessageError::Ok;
    }
    return put_i64(static_cast<std::int64_t>(raw));
}

MessageError SampleWriter::put_bool(bool value) noexcept {
    const FieldSpec* field = nullptr;
    if (advance_field(&field) != MessageError::Ok) {
        return error_;
    }
    if (field->type != static_cast<std::uint8_t>(WireType::Bool) ||
        field->element_count != 1U) {
        error_ = MessageError::InvalidValue;
        return error_;
    }
    if ((cursor_ + 1U) > capacity_) {
        error_ = MessageError::BufferTooSmall;
        return error_;
    }
    out_[cursor_++] = value ? 1U : 0U;
    set_present(*field);
    ++fields_written_;
    return MessageError::Ok;
}

MessageError SampleWriter::put_null() noexcept {
    const FieldSpec* field = nullptr;
    if (advance_field(&field) != MessageError::Ok) {
        return error_;
    }
    if ((field->flags & kFieldNullable) == 0U) {
        error_ = MessageError::WrongOrder;  // this field cannot be null
        return error_;
    }
    // leave the presence bit at 0, write no bytes
    ++fields_written_;
    return MessageError::Ok;
}

namespace {

MessageError check_array_count(const FieldSpec& field, std::size_t count,
                               bool* variable_out) noexcept {
    if ((field.flags & kFieldVariableCount) != 0U) {
        if (count > field.max_element_count) {
            return MessageError::CountTooLarge;
        }
        *variable_out = true;
        return MessageError::Ok;
    }
    if (count != field.element_count) {
        return MessageError::CountMismatch;
    }
    *variable_out = false;
    return MessageError::Ok;
}

}  // namespace

MessageError SampleWriter::put_array_f64(const double* values, std::size_t count) noexcept {
    const FieldSpec* field = nullptr;
    if (advance_field(&field) != MessageError::Ok) {
        return error_;
    }
    if (values == nullptr && count != 0U) {
        error_ = MessageError::InvalidArgument;
        return error_;
    }
    if (!is_scaled_numeric(field->type)) {
        error_ = MessageError::InvalidValue;  // bool / enum arrays: use put_array_i64
        return error_;
    }
    bool variable = false;
    const MessageError rc = check_array_count(*field, count, &variable);
    if (rc != MessageError::Ok) {
        error_ = rc;
        return error_;
    }
    if (variable) {
        if ((cursor_ + 2U) > capacity_) {
            error_ = MessageError::BufferTooSmall;
            return error_;
        }
        write_le(out_ + cursor_, static_cast<std::uint64_t>(count), 2U);
        cursor_ += 2U;
    }
    for (std::size_t index = 0U; index < count; ++index) {
        if (write_scalar_f64(*field, values[index]) != MessageError::Ok) {
            return error_;
        }
    }
    set_present(*field);
    ++fields_written_;
    return MessageError::Ok;
}

MessageError SampleWriter::put_array_i64(const std::int64_t* values, std::size_t count) noexcept {
    const FieldSpec* field = nullptr;
    if (advance_field(&field) != MessageError::Ok) {
        return error_;
    }
    if (values == nullptr && count != 0U) {
        error_ = MessageError::InvalidArgument;
        return error_;
    }
    if (is_float_type(field->type) ||
        field->type == static_cast<std::uint8_t>(WireType::Bool)) {
        error_ = MessageError::InvalidValue;
        return error_;
    }
    bool variable = false;
    const MessageError rc = check_array_count(*field, count, &variable);
    if (rc != MessageError::Ok) {
        error_ = rc;
        return error_;
    }
    if (variable) {
        if ((cursor_ + 2U) > capacity_) {
            error_ = MessageError::BufferTooSmall;
            return error_;
        }
        write_le(out_ + cursor_, static_cast<std::uint64_t>(count), 2U);
        cursor_ += 2U;
    }
    for (std::size_t index = 0U; index < count; ++index) {
        if (write_scalar_i64(*field, values[index]) != MessageError::Ok) {
            return error_;
        }
    }
    set_present(*field);
    ++fields_written_;
    return MessageError::Ok;
}

MessageError SampleWriter::put_array_u64(const std::uint64_t* values, std::size_t count) noexcept {
    const FieldSpec* field = nullptr;
    if (advance_field(&field) != MessageError::Ok) {
        return error_;
    }
    if (values == nullptr && count != 0U) {
        error_ = MessageError::InvalidArgument;
        return error_;
    }
    if (field->type != static_cast<std::uint8_t>(WireType::Uint64)) {
        error_ = MessageError::InvalidValue;
        return error_;
    }
    bool variable = false;
    const MessageError rc = check_array_count(*field, count, &variable);
    if (rc != MessageError::Ok) {
        error_ = rc;
        return error_;
    }
    if (variable) {
        if ((cursor_ + 2U) > capacity_) {
            error_ = MessageError::BufferTooSmall;
            return error_;
        }
        write_le(out_ + cursor_, static_cast<std::uint64_t>(count), 2U);
        cursor_ += 2U;
    }
    if ((cursor_ + 8U * count) > capacity_) {
        error_ = MessageError::BufferTooSmall;
        return error_;
    }
    for (std::size_t index = 0U; index < count; ++index) {
        write_le(out_ + cursor_, values[index], 8U);
        cursor_ += 8U;
    }
    set_present(*field);
    ++fields_written_;
    return MessageError::Ok;
}

MessageError SampleWriter::finish(std::size_t* written) noexcept {
    if (written == nullptr) {
        return MessageError::InvalidArgument;
    }
    if (error_ != MessageError::Ok) {
        return error_;
    }
    if (state_ != kSwBegun) {
        error_ = MessageError::WrongOrder;
        return error_;
    }
    if (fields_written_ != field_count_) {
        error_ = MessageError::CountMismatch;
        return error_;
    }
    *written = cursor_;
    state_ = kSwDone;
    return MessageError::Ok;
}

}  // namespace btp
