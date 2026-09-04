#include "btp/stream.hpp"

#include <limits>

namespace btp {
namespace {

SerialDecodeResult serial_result(SerialDecodeEvent event,
                                 Error error = Error::Ok) noexcept {
    SerialDecodeResult result = {event, error};
    return result;
}

CobsError decoded_size(const std::uint8_t* input,
                       std::size_t input_size,
                       std::size_t* size_out) noexcept {
    if (input == nullptr || size_out == nullptr || input_size == 0U) {
        return CobsError::InvalidArgument;
    }
    for (std::size_t index = 0U; index < input_size; ++index) {
        if (input[index] == 0U) {
            return CobsError::InvalidEncoding;
        }
    }

    std::size_t read_index = 0U;
    std::size_t decoded = 0U;
    while (read_index < input_size) {
        const std::uint8_t code = input[read_index++];
        if (code == 0U) {
            return CobsError::InvalidEncoding;
        }
        const std::size_t copied = static_cast<std::size_t>(code - 1U);
        if (copied > input_size - read_index) {
            return CobsError::InvalidEncoding;
        }
        if (decoded > std::numeric_limits<std::size_t>::max() - copied) {
            return CobsError::InvalidEncoding;
        }
        decoded += copied;
        read_index += copied;
        if (code != 0xFFU && read_index < input_size) {
            if (decoded == std::numeric_limits<std::size_t>::max()) {
                return CobsError::InvalidEncoding;
            }
            ++decoded;
        }
    }
    *size_out = decoded;
    return CobsError::Ok;
}

}  // namespace

CobsError cobs_max_encoded_size(std::size_t input_size,
                                std::size_t* size_out) noexcept {
    if (size_out == nullptr) {
        return CobsError::InvalidArgument;
    }
    const std::size_t overhead = input_size / 254U + 1U;
    if (input_size > std::numeric_limits<std::size_t>::max() - overhead) {
        return CobsError::InvalidArgument;
    }
    *size_out = input_size + overhead;
    return CobsError::Ok;
}

CobsError cobs_encode(const std::uint8_t* input,
                      std::size_t input_size,
                      std::uint8_t* output,
                      std::size_t output_capacity,
                      std::size_t* bytes_written) noexcept {
    if ((input == nullptr && input_size != 0U) || output == nullptr ||
        bytes_written == nullptr) {
        return CobsError::InvalidArgument;
    }

    // Compute the exact size emitted by this canonical encoder before writing.
    std::size_t required = 1U;
    std::uint16_t code = 1U;
    for (std::size_t index = 0U; index < input_size; ++index) {
        if (required == std::numeric_limits<std::size_t>::max()) {
            return CobsError::InvalidArgument;
        }
        ++required;
        if (input[index] == 0U) {
            code = 1U;
        } else if (++code == 0xFFU) {
            code = 1U;
            if (required == std::numeric_limits<std::size_t>::max()) {
                return CobsError::InvalidArgument;
            }
            ++required;
        }
    }
    if (output_capacity < required) {
        return CobsError::BufferTooSmall;
    }

    std::size_t read_index = 0U;
    std::size_t write_index = 1U;
    std::size_t code_index = 0U;
    std::uint8_t block_code = 1U;
    while (read_index < input_size) {
        if (input[read_index] == 0U) {
            output[code_index] = block_code;
            code_index = write_index++;
            block_code = 1U;
            ++read_index;
        } else {
            output[write_index++] = input[read_index++];
            ++block_code;
            if (block_code == 0xFFU) {
                output[code_index] = block_code;
                code_index = write_index++;
                block_code = 1U;
            }
        }
    }
    output[code_index] = block_code;
    *bytes_written = write_index;
    return CobsError::Ok;
}

CobsError cobs_decode(const std::uint8_t* input,
                      std::size_t input_size,
                      std::uint8_t* output,
                      std::size_t output_capacity,
                      std::size_t* bytes_written) noexcept {
    if (output == nullptr || bytes_written == nullptr) {
        return CobsError::InvalidArgument;
    }
    std::size_t required = 0U;
    const CobsError validation = decoded_size(input, input_size, &required);
    if (validation != CobsError::Ok) {
        return validation;
    }
    if (output_capacity < required) {
        return CobsError::BufferTooSmall;
    }

    std::size_t read_index = 0U;
    std::size_t write_index = 0U;
    while (read_index < input_size) {
        const std::uint8_t code = input[read_index++];
        for (std::uint8_t index = 1U; index < code; ++index) {
            output[write_index++] = input[read_index++];
        }
        if (code != 0xFFU && read_index < input_size) {
            output[write_index++] = 0U;
        }
    }
    *bytes_written = write_index;
    return CobsError::Ok;
}

const char* cobs_error_string(CobsError error) noexcept {
    switch (error) {
        case CobsError::Ok: return "ok";
        case CobsError::InvalidArgument: return "invalid argument";
        case CobsError::BufferTooSmall: return "output buffer too small";
        case CobsError::InvalidEncoding: return "invalid COBS encoding";
    }
    return "unknown COBS error";
}

SerialDecoder::SerialDecoder(std::uint8_t* encoded_buffer,
                             std::size_t encoded_capacity,
                             std::uint8_t* decoded_buffer,
                             std::size_t decoded_capacity) noexcept
    : encoded_buffer_(encoded_buffer),
      encoded_capacity_(encoded_capacity),
      decoded_buffer_(decoded_buffer),
      decoded_capacity_(decoded_capacity),
      encoded_size_(0U),
      state_(State::WaitingDelimiter) {}

bool SerialDecoder::valid() const noexcept {
    return encoded_buffer_ != nullptr && decoded_buffer_ != nullptr &&
           encoded_capacity_ >= kSerialMaxCobsBlockSize &&
           decoded_capacity_ >= kSerialMaxFrameSize;
}

void SerialDecoder::reset() noexcept {
    encoded_size_ = 0U;
    state_ = State::WaitingDelimiter;
}

SerialDecodeResult SerialDecoder::push(std::uint8_t byte,
                                       DecodedFrame* decoded_frame) noexcept {
    if (!valid() || decoded_frame == nullptr) {
        return serial_result(SerialDecodeEvent::InvalidConfiguration,
                             Error::InvalidArgument);
    }

    if (state_ == State::WaitingDelimiter) {
        if (byte == 0U) {
            state_ = State::Collecting;
        }
        return serial_result(SerialDecodeEvent::None);
    }

    if (state_ == State::DiscardingOverflow) {
        if (byte == 0U) {
            encoded_size_ = 0U;
            state_ = State::Collecting;
        }
        return serial_result(SerialDecodeEvent::None);
    }

    if (byte != 0U) {
        if (encoded_size_ == kSerialMaxCobsBlockSize ||
            encoded_size_ == encoded_capacity_) {
            encoded_size_ = 0U;
            state_ = State::DiscardingOverflow;
            return serial_result(SerialDecodeEvent::Overflow,
                                 Error::FrameTooLarge);
        }
        encoded_buffer_[encoded_size_++] = byte;
        return serial_result(SerialDecodeEvent::None);
    }

    if (encoded_size_ == 0U) {
        return serial_result(SerialDecodeEvent::None);
    }

    std::size_t decoded_size_value = 0U;
    const CobsError cobs_result = cobs_decode(
        encoded_buffer_, encoded_size_, decoded_buffer_, decoded_capacity_,
        &decoded_size_value);
    encoded_size_ = 0U;
    if (cobs_result != CobsError::Ok ||
        decoded_size_value > kSerialMaxFrameSize) {
        return serial_result(SerialDecodeEvent::CobsError,
                             Error::FrameTooLarge);
    }

    const Error frame_result = decode(decoded_buffer_, decoded_size_value,
                                      kSerialTransport, decoded_frame);
    if (frame_result != Error::Ok) {
        return serial_result(SerialDecodeEvent::FrameError, frame_result);
    }
    return serial_result(SerialDecodeEvent::Frame);
}

}  // namespace btp
