#ifndef BTP_CODEC_HPP
#define BTP_CODEC_HPP

#include <cstddef>
#include <cstdint>

namespace btp {

static const std::uint8_t kLibraryVersionMajor = 0U;
static const std::uint8_t kLibraryVersionMinor = 1U;
static const std::uint8_t kLibraryVersionPatch = 0U;
static const std::uint8_t kMinimumProtocolVersion = 1U;
static const std::uint8_t kMaximumProtocolVersion = 1U;

static const std::size_t kV1HeaderSize = 36U;
static const std::size_t kV1CrcSize = 4U;
static const std::size_t kV1MinimumFrameSize = kV1HeaderSize + kV1CrcSize;
static const std::size_t kEspNowMaxFrameSize = 250U;
static const std::size_t kEspNowMaxPayloadSize = 210U;
static const std::size_t kSerialMaxFrameSize = 4096U;
static const std::size_t kSerialMaxPayloadSize = 4056U;

static const std::uint8_t kV1Version = 1U;
static const std::uint16_t kFlagFragmented = 0x0001U;
static const std::uint16_t kKnownFlagsMask = kFlagFragmented;

enum class MessageType : std::uint8_t {
    Invalid = 0x00U,
    Telemetry = 0x01U,
    Log = 0x02U,
    Command = 0x03U,
    Terminal = 0x04U,
    Control = 0x05U
};

enum class TransportProfile : std::uint8_t {
    EspNow,
    Serial
};

enum class Error : std::uint8_t {
    Ok,
    InvalidArgument,
    BufferTooSmall,
    FrameTooShort,
    FrameTooLarge,
    PayloadTooLarge,
    InvalidMagic,
    UnsupportedVersion,
    InvalidHeaderSize,
    SizeMismatch,
    CrcMismatch,
    InvalidType,
    InvalidFlags,
    InvalidSourceId,
    InvalidBootId,
    InvalidFragmentation
};

struct ByteView {
    const std::uint8_t* data;
    std::size_t size;
};

struct Header {
    MessageType type;
    std::uint16_t flags;
    std::uint32_t source_id;
    std::uint32_t boot_id;
    std::uint32_t sequence;
    std::uint64_t timestamp_us;
    std::uint16_t object_id;
    std::uint8_t fragment_index;
    std::uint8_t fragment_count;
};

struct Frame {
    Header header;
    ByteView payload;
};

struct DecodedFrame {
    Header header;
    ByteView payload;
    std::uint32_t crc32;
};

// Returns the exact wire size without writing. The transport limit is applied.
Error encoded_size(std::size_t payload_size,
                   TransportProfile transport,
                   std::size_t* size_out) noexcept;

// Encodes into caller-owned memory. No output byte is written unless all
// arguments, header invariants, limits and output capacity are valid.
Error encode(const Frame& frame,
             TransportProfile transport,
             std::uint8_t* output,
             std::size_t output_capacity,
             std::size_t* bytes_written) noexcept;

// Decodes without allocation. On success, payload points inside input and is
// valid only as long as input remains valid. decoded is untouched on failure.
Error decode(const std::uint8_t* input,
             std::size_t input_size,
             TransportProfile transport,
             DecodedFrame* decoded) noexcept;

// CRC-32/ISO-HDLC (CRC-32/IEEE), returned as a numeric host value.
std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept;

const char* error_string(Error error) noexcept;

}  // namespace btp

#endif  // BTP_CODEC_HPP
