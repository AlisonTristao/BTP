#include "btp/codec.hpp"

#include <cstring>

namespace btp {
namespace {

const std::uint8_t kMagic[4] = {0x42U, 0x54U, 0x50U, 0x00U};

void write_u16_le(std::uint8_t* destination, std::uint16_t value) noexcept {
    destination[0] = static_cast<std::uint8_t>(value);
    destination[1] = static_cast<std::uint8_t>(value >> 8U);
}

void write_u32_le(std::uint8_t* destination, std::uint32_t value) noexcept {
    destination[0] = static_cast<std::uint8_t>(value);
    destination[1] = static_cast<std::uint8_t>(value >> 8U);
    destination[2] = static_cast<std::uint8_t>(value >> 16U);
    destination[3] = static_cast<std::uint8_t>(value >> 24U);
}

void write_u64_le(std::uint8_t* destination, std::uint64_t value) noexcept {
    for (std::size_t index = 0U; index < 8U; ++index) {
        destination[index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

std::uint16_t read_u16_le(const std::uint8_t* source) noexcept {
    return static_cast<std::uint16_t>(source[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(source[1]) << 8U);
}

std::uint32_t read_u32_le(const std::uint8_t* source) noexcept {
    return static_cast<std::uint32_t>(source[0]) |
           (static_cast<std::uint32_t>(source[1]) << 8U) |
           (static_cast<std::uint32_t>(source[2]) << 16U) |
           (static_cast<std::uint32_t>(source[3]) << 24U);
}

std::uint64_t read_u64_le(const std::uint8_t* source) noexcept {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(source[index]) << (index * 8U);
    }
    return value;
}

bool valid_transport(TransportProfile transport) noexcept {
    return transport == TransportProfile::EspNow ||
           transport == TransportProfile::Serial ||
           transport == TransportProfile::UsbHid;
}

std::size_t max_payload_size(TransportProfile transport) noexcept {
    switch (transport) {
        case TransportProfile::EspNow: return kEspNowMaxPayloadSize;
        case TransportProfile::UsbHid: return kUsbHidMaxPayloadSize;
        case TransportProfile::Serial: default: return kSerialMaxPayloadSize;
    }
}

std::size_t max_frame_size(TransportProfile transport) noexcept {
    switch (transport) {
        case TransportProfile::EspNow: return kEspNowMaxFrameSize;
        case TransportProfile::UsbHid: return kUsbHidMaxFrameSize;
        case TransportProfile::Serial: default: return kSerialMaxFrameSize;
    }
}

bool valid_type(MessageType type) noexcept {
    const std::uint8_t value = static_cast<std::uint8_t>(type);
    return value >= static_cast<std::uint8_t>(MessageType::Telemetry) &&
           value <= static_cast<std::uint8_t>(MessageType::Control);
}

// Writes the 36-octet header for `header`/`payload_size`, selecting version 2
// automatically when ENCRYPTED is set (BTP_V1.md section 8, §3). Shared by
// encode() and encode_header() so both always produce identical bytes.
void write_header(const Header& header,
                  std::uint16_t payload_size,
                  std::uint8_t out_header[36]) noexcept {
    const bool encrypted = (header.flags & kFlagEncrypted) != 0U;
    out_header[0] = kMagic[0];
    out_header[1] = kMagic[1];
    out_header[2] = kMagic[2];
    out_header[3] = kMagic[3];
    out_header[4] = encrypted ? kV2Version : kV1Version;
    out_header[5] = static_cast<std::uint8_t>(header.type);
    write_u16_le(out_header + 6U, header.flags);
    write_u16_le(out_header + 8U, static_cast<std::uint16_t>(kV1HeaderSize));
    write_u16_le(out_header + 10U, payload_size);
    write_u32_le(out_header + 12U, header.source_id);
    write_u32_le(out_header + 16U, header.boot_id);
    write_u32_le(out_header + 20U, header.sequence);
    write_u64_le(out_header + 24U, header.timestamp_us);
    write_u16_le(out_header + 32U, header.object_id);
    out_header[34] = header.fragment_index;
    out_header[35] = header.fragment_count;
}

// BTP_V1.md section 8.1: with ENCRYPTED clear there is no cipher "in use",
// so CIPHER_ID MUST be 0; with ENCRYPTED set, CIPHER_ID MUST be 0 or 1 (the
// only assigned values) -- 2 and 3 are reserved for future ciphers and MUST
// be rejected, the same principle already applied to reserved flag bits.
// A 16-octet AEAD tag over the UsbHid payload ceiling of 22 octets is 73%
// overhead, against ~7.6% on EspNow and ~0.4% on Serial, so BTP_V1.md section
// 8.7 forbids ENCRYPTED on a UsbHid frame outright. The rule lived only in
// prose until now: an encoder that ignored it produced frames every conforming
// decoder was supposed to refuse, and no decoder actually refused them.
Error validate_encryption_for_transport(std::uint16_t flags,
                                        TransportProfile transport) noexcept {
    if ((flags & kFlagEncrypted) != 0U &&
        transport == TransportProfile::UsbHid) {
        return Error::EncryptedNotAllowedOnTransport;
    }
    return Error::Ok;
}

bool valid_cipher_id_for_flags(std::uint16_t flags) noexcept {
    const std::uint16_t raw_cipher_id =
        static_cast<std::uint16_t>((flags & kCipherIdMask) >> kCipherIdShift);
    if ((flags & kFlagEncrypted) == 0U) {
        return raw_cipher_id == 0U;
    }
    return raw_cipher_id <= static_cast<std::uint16_t>(CipherId::ChaCha20Poly1305);
}

Error validate_header(const Header& header) noexcept {
    if (!valid_type(header.type)) {
        return Error::InvalidType;
    }
    if ((header.flags & static_cast<std::uint16_t>(~kKnownFlagsMask)) != 0U) {
        return Error::InvalidFlags;
    }
    if (!valid_cipher_id_for_flags(header.flags)) {
        return Error::InvalidCipherId;
    }
    if (header.source_id == 0U) {
        return Error::InvalidSourceId;
    }
    if (header.boot_id == 0U) {
        return Error::InvalidBootId;
    }

    const bool fragmented = (header.flags & kFlagFragmented) != 0U;
    if (fragmented) {
        if (header.fragment_count < 2U ||
            header.fragment_index >= header.fragment_count) {
            return Error::InvalidFragmentation;
        }
    } else if (header.fragment_index != 0U || header.fragment_count != 1U) {
        return Error::InvalidFragmentation;
    }
    return Error::Ok;
}

}  // namespace

Error encoded_size(std::size_t payload_size,
                   TransportProfile transport,
                   std::size_t* size_out) noexcept {
    if (size_out == nullptr || !valid_transport(transport)) {
        return Error::InvalidArgument;
    }
    if (payload_size > max_payload_size(transport) || payload_size > 0xFFFFU) {
        return Error::PayloadTooLarge;
    }
    *size_out = kV1MinimumFrameSize + payload_size;
    return Error::Ok;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr && size != 0U) {
        return 0U;
    }

    std::uint32_t accumulator = 0xFFFFFFFFU;
    for (std::size_t index = 0U; index < size; ++index) {
        accumulator ^= static_cast<std::uint32_t>(data[index]);
        for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
            const std::uint32_t mask =
                static_cast<std::uint32_t>(0U - (accumulator & 1U));
            accumulator = (accumulator >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return accumulator ^ 0xFFFFFFFFU;
}

Error encode(const Frame& frame,
             TransportProfile transport,
             std::uint8_t* output,
             std::size_t output_capacity,
             std::size_t* bytes_written) noexcept {
    if (bytes_written == nullptr || !valid_transport(transport)) {
        return Error::InvalidArgument;
    }

    const Error header_error = validate_header(frame.header);
    if (header_error != Error::Ok) {
        return header_error;
    }
    const Error transport_error =
        validate_encryption_for_transport(frame.header.flags, transport);
    if (transport_error != Error::Ok) {
        return transport_error;
    }
    if (frame.payload.data == nullptr && frame.payload.size != 0U) {
        return Error::InvalidArgument;
    }

    std::size_t frame_size = 0U;
    const Error size_error = encoded_size(frame.payload.size, transport, &frame_size);
    if (size_error != Error::Ok) {
        return size_error;
    }
    if (output == nullptr) {
        return Error::InvalidArgument;
    }
    if (output_capacity < frame_size) {
        return Error::BufferTooSmall;
    }

    // Moving first permits a caller to prepend a header in the same buffer;
    // memmove also handles an already-positioned payload without special cases.
    if (frame.payload.size != 0U) {
        std::memmove(output + kV1HeaderSize, frame.payload.data, frame.payload.size);
    }

    write_header(frame.header, static_cast<std::uint16_t>(frame.payload.size),
                output);

    const std::uint32_t checksum =
        crc32(output, kV1HeaderSize + frame.payload.size);
    write_u32_le(output + kV1HeaderSize + frame.payload.size, checksum);
    *bytes_written = frame_size;
    return Error::Ok;
}

Error decode(const std::uint8_t* input,
             std::size_t input_size,
             TransportProfile transport,
             DecodedFrame* decoded) noexcept {
    if (input == nullptr || decoded == nullptr || !valid_transport(transport)) {
        return Error::InvalidArgument;
    }
    if (input_size < kV1MinimumFrameSize) {
        return Error::FrameTooShort;
    }
    if (input_size > max_frame_size(transport)) {
        return Error::FrameTooLarge;
    }
    for (std::size_t index = 0U; index < 4U; ++index) {
        if (input[index] != kMagic[index]) {
            return Error::InvalidMagic;
        }
    }
    const std::uint8_t version = input[4];
    if (version < kMinimumProtocolVersion || version > kMaximumProtocolVersion) {
        return Error::UnsupportedVersion;
    }
    if (read_u16_le(input + 8U) != kV1HeaderSize) {
        return Error::InvalidHeaderSize;
    }

    const std::size_t payload_size = read_u16_le(input + 10U);
    if (payload_size > max_payload_size(transport)) {
        return Error::PayloadTooLarge;
    }
    const std::size_t expected_size = kV1MinimumFrameSize + payload_size;
    if (input_size != expected_size) {
        return Error::SizeMismatch;
    }

    const std::uint32_t expected_crc = read_u32_le(input + kV1HeaderSize + payload_size);
    const std::uint32_t actual_crc = crc32(input, kV1HeaderSize + payload_size);
    if (actual_crc != expected_crc) {
        return Error::CrcMismatch;
    }

    Header header;
    header.type = static_cast<MessageType>(input[5]);
    header.flags = read_u16_le(input + 6U);
    header.source_id = read_u32_le(input + 12U);
    header.boot_id = read_u32_le(input + 16U);
    header.sequence = read_u32_le(input + 20U);
    header.timestamp_us = read_u64_le(input + 24U);
    header.object_id = read_u16_le(input + 32U);
    header.fragment_index = input[34];
    header.fragment_count = input[35];

    // Section 8, §3: ENCRYPTED marked MUST imply version == 0x02. There is no
    // inverse requirement: version == 0x02 with ENCRYPTED clear is valid.
    if ((header.flags & kFlagEncrypted) != 0U && version != kV2Version) {
        return Error::EncryptedVersionMismatch;
    }

    const Error transport_error =
        validate_encryption_for_transport(header.flags, transport);
    if (transport_error != Error::Ok) {
        return transport_error;
    }

    const Error header_error = validate_header(header);
    if (header_error != Error::Ok) {
        return header_error;
    }

    DecodedFrame result;
    result.header = header;
    result.payload.data = input + kV1HeaderSize;
    result.payload.size = payload_size;
    result.crc32 = expected_crc;
    *decoded = result;
    return Error::Ok;
}

CipherId cipher_id(std::uint16_t flags) noexcept {
    return static_cast<CipherId>(
        static_cast<std::uint8_t>((flags & kCipherIdMask) >> kCipherIdShift));
}

void aead_nonce(const Header& header, std::uint8_t out_nonce[12]) noexcept {
    write_u32_le(out_nonce, header.source_id);
    write_u32_le(out_nonce + 4U, header.boot_id);
    write_u32_le(out_nonce + 8U, header.sequence);
}

Error encode_header(const Header& header,
                    std::uint16_t payload_size,
                    std::uint8_t out_header[36]) noexcept {
    if (out_header == nullptr) {
        return Error::InvalidArgument;
    }
    // payload_size is already the wire's uint16_le width, so it is always
    // representable; the same "fits in the field" criterion encoded_size()
    // applies to a std::size_t input can never be violated here by type.
    const Error header_error = validate_header(header);
    if (header_error != Error::Ok) {
        return header_error;
    }
    write_header(header, payload_size, out_header);
    return Error::Ok;
}

const char* error_string(Error error) noexcept {
    switch (error) {
        case Error::Ok: return "ok";
        case Error::InvalidArgument: return "invalid argument";
        case Error::BufferTooSmall: return "output buffer too small";
        case Error::FrameTooShort: return "frame too short";
        case Error::FrameTooLarge: return "frame too large";
        case Error::PayloadTooLarge: return "payload too large for transport";
        case Error::InvalidMagic: return "invalid magic";
        case Error::UnsupportedVersion: return "unsupported version";
        case Error::InvalidHeaderSize: return "invalid header size";
        case Error::SizeMismatch: return "frame size mismatch";
        case Error::CrcMismatch: return "CRC mismatch";
        case Error::InvalidType: return "invalid or reserved message type";
        case Error::InvalidFlags: return "reserved flag is set";
        case Error::InvalidSourceId: return "source ID must be nonzero";
        case Error::InvalidBootId: return "boot ID must be nonzero";
        case Error::InvalidFragmentation: return "invalid fragmentation fields";
        case Error::EncryptedVersionMismatch:
            return "ENCRYPTED flag set without version 2";
        case Error::InvalidCipherId:
            return "CIPHER_ID inconsistent with ENCRYPTED or reserved";
        case Error::EncryptedNotAllowedOnTransport:
            return "ENCRYPTED not allowed on this transport";
    }
    return "unknown error";
}

}  // namespace btp
