#ifndef BTP_CODEC_HPP
#define BTP_CODEC_HPP

#include <cstddef>
#include <cstdint>

namespace btp {

// Keep in step with CMakeLists.txt project(VERSION) and library.json. These
// are the library's own version, which is a different thing from the wire
// version a frame carries at octet 4.
static const std::uint8_t kLibraryVersionMajor = 2U;
static const std::uint8_t kLibraryVersionMinor = 6U;
static const std::uint8_t kLibraryVersionPatch = 0U;
static const std::uint8_t kMinimumProtocolVersion = 1U;
static const std::uint8_t kMaximumProtocolVersion = 2U;

static const std::size_t kV1HeaderSize = 36U;
static const std::size_t kV1CrcSize = 4U;
static const std::size_t kV1MinimumFrameSize = kV1HeaderSize + kV1CrcSize;
static const std::size_t kEspNowMaxFrameSize = 250U;
static const std::size_t kEspNowMaxPayloadSize = 210U;
static const std::size_t kSerialMaxFrameSize = 4096U;
static const std::size_t kSerialMaxPayloadSize = 4056U;
static const std::size_t kUsbHidMaxFrameSize = 62U;
static const std::size_t kUsbHidMaxPayloadSize = 22U;

static const std::uint8_t kV1Version = 1U;
static const std::uint8_t kV2Version = 2U;
static const std::uint16_t kFlagFragmented = 0x0001U;
static const std::uint16_t kFlagEncrypted = 0x0002U;
// CIPHER_ID sub-field, bits 2-3 of flags (docs/frame.md section 3 and docs/encryption.md section 3):
// a 2-bit enum, not two independent boolean flags, so the wire cannot
// represent an ambiguous "both ciphers marked" combination. 0 == AES-128-GCM
// (default), 1 == ChaCha20-Poly1305, 2/3 reserved and rejected by decode().
static const std::uint16_t kCipherIdMask = 0x000CU;
static const std::uint16_t kCipherIdShift = 2U;
static const std::uint16_t kKnownFlagsMask =
    kFlagFragmented | kFlagEncrypted | kCipherIdMask;

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
    Serial,
    UsbHid
};

// Values assigned to the CIPHER_ID sub-field of flags (docs/encryption.md section 3). Only AesGcm (0) and ChaCha20Poly1305 (1) are assigned; the raw
// values 2 and 3 are reserved for future ciphers. cipher_id() below is a
// pure extraction and returns those reserved values cast into this enum
// too -- rejecting them is validate_header()'s job inside decode()/encode(),
// not cipher_id()'s.
enum class CipherId : std::uint8_t {
    AesGcm = 0x00U,
    ChaCha20Poly1305 = 0x01U
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
    InvalidFragmentation,
    EncryptedVersionMismatch,
    InvalidCipherId,
    EncryptedNotAllowedOnTransport
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

// The frame and payload ceilings of a transport profile. A caller sizing a
// receive buffer needs the first; a caller deciding whether to fragment needs
// the second. Both return the Serial values for an unrecognized profile, since
// every entry point validates the profile before asking.
std::size_t max_frame_size(TransportProfile transport) noexcept;
std::size_t max_payload_size(TransportProfile transport) noexcept;

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

// Extracts the 2-bit CIPHER_ID sub-field of flags (bits 2-3, mask
// kCipherIdMask, shift kCipherIdShift; docs/frame.md section 3 and docs/encryption.md section 3),
// already shifted down to a plain 0-3 value cast into CipherId. This is a
// pure, unchecked read: if the sub-field holds a reserved raw value (2 or
// 3), it is returned as-is cast to CipherId rather than rejected here --
// rejecting a reserved or inconsistent CIPHER_ID is validate_header()'s job
// inside encode()/decode(), not this extraction function's.
CipherId cipher_id(std::uint16_t flags) noexcept;

// Writes the 12-octet AEAD nonce of docs/encryption.md section 4:
// source_id (4) || boot_id (4) || sequence (4), each little-endian.
void aead_nonce(const Header& header, std::uint8_t out_nonce[12]) noexcept;

// Serializes the 36-octet header that encode() would write for this header
// and payload_size, selecting version 2 when ENCRYPTED is set exactly like
// encode() does; callers use this to build the AAD
// (docs/encryption.md section 5) before the
// payload is encrypted, since encode() itself expects an already-encrypted
// payload. payload_size is already the wire's uint16_le width, so it always
// fits; this fails only with whatever Error validate_header() would return
// for an invalid header, or Error::InvalidArgument for a null out_header.
Error encode_header(const Header& header,
                    std::uint16_t payload_size,
                    std::uint8_t out_header[36]) noexcept;

const char* error_string(Error error) noexcept;

}  // namespace btp

#endif  // BTP_CODEC_HPP
