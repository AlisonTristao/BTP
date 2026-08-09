#ifndef BTP_STREAM_HPP
#define BTP_STREAM_HPP

#include "btp/codec.hpp"

#include <cstddef>
#include <cstdint>

namespace btp {

static const std::size_t kSerialMaxCobsBlockSize =
    kSerialMaxFrameSize + kSerialMaxFrameSize / 254U + 1U;
static const std::size_t kSerialMaxPacketSize =
    kSerialMaxCobsBlockSize + 2U;

enum class CobsError : std::uint8_t {
    Ok,
    InvalidArgument,
    BufferTooSmall,
    InvalidEncoding
};

// Returns the worst-case encoded size for an input of input_size bytes.
CobsError cobs_max_encoded_size(std::size_t input_size,
                                std::size_t* size_out) noexcept;

// COBS encode/decode use caller-owned, non-overlapping buffers. Output and
// bytes_written are untouched when validation fails.
CobsError cobs_encode(const std::uint8_t* input,
                      std::size_t input_size,
                      std::uint8_t* output,
                      std::size_t output_capacity,
                      std::size_t* bytes_written) noexcept;

CobsError cobs_decode(const std::uint8_t* input,
                      std::size_t input_size,
                      std::uint8_t* output,
                      std::size_t output_capacity,
                      std::size_t* bytes_written) noexcept;

const char* cobs_error_string(CobsError error) noexcept;

enum class SerialDecodeEvent : std::uint8_t {
    None,
    Frame,
    CobsError,
    FrameError,
    Overflow,
    InvalidConfiguration
};

struct SerialDecodeResult {
    SerialDecodeEvent event;
    Error frame_error;
};

// Incremental decoder for 0x00 || COBS(frame) || 0x00. It deliberately starts
// unsynchronized and discards input until the first delimiter. The caller must
// supply buffers of at least kSerialMaxCobsBlockSize and kSerialMaxFrameSize.
class SerialDecoder {
public:
    SerialDecoder(std::uint8_t* encoded_buffer,
                  std::size_t encoded_capacity,
                  std::uint8_t* decoded_buffer,
                  std::size_t decoded_capacity) noexcept;

    bool valid() const noexcept;
    void reset() noexcept;

    // On Frame, decoded_frame points into the caller-provided decoded buffer
    // and remains valid until a later delimiter completes another candidate.
    SerialDecodeResult push(std::uint8_t byte,
                            DecodedFrame* decoded_frame) noexcept;

private:
    enum class State : std::uint8_t {
        WaitingDelimiter,
        Collecting,
        DiscardingOverflow
    };

    std::uint8_t* encoded_buffer_;
    std::size_t encoded_capacity_;
    std::uint8_t* decoded_buffer_;
    std::size_t decoded_capacity_;
    std::size_t encoded_size_;
    State state_;
};

}  // namespace btp

#endif  // BTP_STREAM_HPP
