#ifndef BTP_MESSAGES_DETAIL_HPP
#define BTP_MESSAGES_DETAIL_HPP

// Internal cursor for btp::messages. NOT under include/, not installed, no
// compatibility promise -- same status as detail.hpp.
//
// One Reader and one Writer, both bounds-checked on every access and both
// "sticky" on error: once an access would run past the buffer the cursor
// stops moving and every later call is a no-op, so a decoder can do a run of
// reads and check the error once at the end instead of after each field.
//
// This is the single place the "validate every declared length before
// consuming the data" rule (docs/commands.md section 7) lives. Every consumer
// used to re-implement it; an off-by-one here was an off-by-one in three
// repositories.
//
// Endianness: BTP is little-endian on the wire and every target (ESP32, x86
// desktops) is a little-endian host, the same assumption src/codec.cpp makes.
// f64 is the IEEE-754 bit pattern in little-endian order (docs/telemetry.md
// section 13).

#include "btp/codec.hpp"     // ByteView
#include "btp/messages.hpp"  // MessageError, kMaxUtf8Text

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace btp {
namespace detail {

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) noexcept
        : data_(data), size_(size), cursor_(0U), error_(MessageError::Ok) {}

    bool ok() const noexcept { return error_ == MessageError::Ok; }
    MessageError error() const noexcept { return error_; }
    std::size_t consumed() const noexcept { return cursor_; }
    std::size_t remaining() const noexcept {
        return ok() ? (size_ - cursor_) : 0U;
    }

    // Records `e` unless a failure is already recorded (first error wins).
    // Returns false so a caller can `return reader.fail(...)`.
    bool fail(MessageError e) noexcept {
        if (error_ == MessageError::Ok) {
            error_ = e;
        }
        return false;
    }

    std::uint8_t u8() noexcept {
        if (!take(1U, MessageError::PayloadTooShort)) {
            return 0U;
        }
        return data_[cursor_ - 1U];
    }

    std::uint16_t u16() noexcept {
        if (!take(2U, MessageError::PayloadTooShort)) {
            return 0U;
        }
        const std::uint8_t* p = data_ + cursor_ - 2U;
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(p[0]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8U));
    }

    std::uint32_t u32() noexcept {
        if (!take(4U, MessageError::PayloadTooShort)) {
            return 0U;
        }
        const std::uint8_t* p = data_ + cursor_ - 4U;
        return static_cast<std::uint32_t>(p[0]) |
               (static_cast<std::uint32_t>(p[1]) << 8U) |
               (static_cast<std::uint32_t>(p[2]) << 16U) |
               (static_cast<std::uint32_t>(p[3]) << 24U);
    }

    std::uint64_t u64() noexcept {
        if (!take(8U, MessageError::PayloadTooShort)) {
            return 0U;
        }
        const std::uint8_t* p = data_ + cursor_ - 8U;
        std::uint64_t value = 0U;
        for (std::size_t index = 0U; index < 8U; ++index) {
            value |= static_cast<std::uint64_t>(p[index]) << (index * 8U);
        }
        return value;
    }

    double f64() noexcept {
        const std::uint64_t bits = u64();
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // A fixed-size run of octets (peer_uuid, source_uuid). Overflow is
    // PayloadTooShort -- the fixed portion of the payload is truncated.
    ByteView raw(std::size_t count) noexcept {
        if (!take(count, MessageError::PayloadTooShort)) {
            return ByteView{nullptr, 0U};
        }
        return ByteView{data_ + cursor_ - count, count};
    }

    // utf8_u16: uint16_le length, then that many octets (docs/commands.md
    // section 1.1). `limit` is the smallest applicable section-6 cap; a length
    // above it is CountTooLarge, a length that fits the cap but not the
    // remaining buffer is LengthOverflow.
    ByteView utf8_u16(std::size_t limit = kMaxUtf8Text) noexcept {
        const std::uint16_t length = u16();
        if (!ok()) {
            return ByteView{nullptr, 0U};
        }
        if (length > limit) {
            fail(MessageError::CountTooLarge);
            return ByteView{nullptr, 0U};
        }
        if (!take(length, MessageError::LengthOverflow)) {
            return ByteView{nullptr, 0U};
        }
        return ByteView{data_ + cursor_ - length, length};
    }

    // bytes_u32: uint32_le length, then that many octets (section 1.2).
    ByteView bytes_u32(std::size_t limit) noexcept {
        const std::uint32_t length = u32();
        if (!ok()) {
            return ByteView{nullptr, 0U};
        }
        if (length > limit) {
            fail(MessageError::CountTooLarge);
            return ByteView{nullptr, 0U};
        }
        if (!take(length, MessageError::LengthOverflow)) {
            return ByteView{nullptr, 0U};
        }
        return ByteView{data_ + cursor_ - length, length};
    }

    void skip(std::size_t count) noexcept {
        (void)take(count, MessageError::PayloadTooShort);
    }

    // Reserved octets must be zero (docs/model.md section 9.3).
    void expect_zero(std::size_t count) noexcept {
        for (std::size_t index = 0U; index < count; ++index) {
            const std::uint8_t octet = u8();
            if (ok() && octet != 0U) {
                fail(MessageError::ReservedNotZero);
            }
        }
    }

    // Call at the end of a decode: an octet left over is TrailingBytes
    // (docs/commands.md section 7 step 10).
    MessageError require_exhausted() noexcept {
        if (ok() && cursor_ != size_) {
            fail(MessageError::TrailingBytes);
        }
        return error_;
    }

private:
    bool take(std::size_t count, MessageError on_overflow) noexcept {
        if (error_ != MessageError::Ok) {
            return false;
        }
        if (count > (size_ - cursor_)) {
            error_ = on_overflow;
            return false;
        }
        cursor_ += count;
        return true;
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t cursor_;
    MessageError error_;
};

class Writer {
public:
    Writer(std::uint8_t* out, std::size_t capacity) noexcept
        : out_(out), capacity_(capacity), cursor_(0U), error_(MessageError::Ok) {}

    bool ok() const noexcept { return error_ == MessageError::Ok; }
    MessageError error() const noexcept { return error_; }
    std::size_t written() const noexcept { return cursor_; }

    bool fail(MessageError e) noexcept {
        if (error_ == MessageError::Ok) {
            error_ = e;
        }
        return false;
    }

    void u8(std::uint8_t value) noexcept {
        if (!make_room(1U)) {
            return;
        }
        out_[cursor_ - 1U] = value;
    }

    void u16(std::uint16_t value) noexcept {
        if (!make_room(2U)) {
            return;
        }
        std::uint8_t* p = out_ + cursor_ - 2U;
        p[0] = static_cast<std::uint8_t>(value);
        p[1] = static_cast<std::uint8_t>(value >> 8U);
    }

    void u32(std::uint32_t value) noexcept {
        if (!make_room(4U)) {
            return;
        }
        write_u32_at(cursor_ - 4U, value);
    }

    void u64(std::uint64_t value) noexcept {
        if (!make_room(8U)) {
            return;
        }
        std::uint8_t* p = out_ + cursor_ - 8U;
        for (std::size_t index = 0U; index < 8U; ++index) {
            p[index] = static_cast<std::uint8_t>(value >> (index * 8U));
        }
    }

    void f64(double value) noexcept {
        std::uint64_t bits = 0U;
        std::memcpy(&bits, &value, sizeof(bits));
        u64(bits);
    }

    void raw(const std::uint8_t* data, std::size_t count) noexcept {
        if (!make_room(count)) {
            return;
        }
        if (count != 0U) {
            std::memcpy(out_ + cursor_ - count, data, count);
        }
    }

    void zeros(std::size_t count) noexcept {
        if (!make_room(count)) {
            return;
        }
        if (count != 0U) {
            std::memset(out_ + cursor_ - count, 0, count);
        }
    }

    void utf8_u16(ByteView text, std::size_t limit = kMaxUtf8Text) noexcept {
        if (text.size > limit || text.size > 0xFFFFU) {
            fail(MessageError::InvalidArgument);
            return;
        }
        u16(static_cast<std::uint16_t>(text.size));
        raw(text.data, text.size);
    }

    void bytes_u32(ByteView blob, std::size_t limit) noexcept {
        if (blob.size > limit) {
            fail(MessageError::InvalidArgument);
            return;
        }
        u32(static_cast<std::uint32_t>(blob.size));
        raw(blob.data, blob.size);
    }

    // record_size backpatching (docs/commands.md section 3.5): reserve a
    // uint32_le slot now, fill it once the record content is written.
    std::size_t reserve_u32() noexcept {
        const std::size_t at = cursor_;
        u32(0U);
        return at;
    }

    void patch_u32(std::size_t at, std::uint32_t value) noexcept {
        if (!ok()) {
            return;
        }
        if ((at + 4U) > cursor_) {
            fail(MessageError::InvalidArgument);
            return;
        }
        write_u32_at(at, value);
    }

private:
    bool make_room(std::size_t count) noexcept {
        if (error_ != MessageError::Ok) {
            return false;
        }
        if (count > (capacity_ - cursor_)) {
            error_ = MessageError::BufferTooSmall;
            return false;
        }
        cursor_ += count;
        return true;
    }

    void write_u32_at(std::size_t at, std::uint32_t value) noexcept {
        out_[at] = static_cast<std::uint8_t>(value);
        out_[at + 1U] = static_cast<std::uint8_t>(value >> 8U);
        out_[at + 2U] = static_cast<std::uint8_t>(value >> 16U);
        out_[at + 3U] = static_cast<std::uint8_t>(value >> 24U);
    }

    std::uint8_t* out_;
    std::size_t capacity_;
    std::size_t cursor_;
    MessageError error_;
};

}  // namespace detail
}  // namespace btp

#endif  // BTP_MESSAGES_DETAIL_HPP
