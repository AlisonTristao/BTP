#include "btp/endpoint.hpp"

#include "btp/fragmentation.hpp"

#include <cstring>
#include <limits>

namespace btp {
namespace {

// Every frame this class encodes into is bounded by the ESP-NOW ceiling: the
// endpoint's real job is channel-C / channel-B radio traffic, and a logical
// payload larger than one ESP-NOW frame goes through send_logical(), which
// fragments to fit. A single frame that does not fit -- a Serial-sized
// cleartext fragment, say -- fails cleanly on encode()'s own capacity check
// rather than being silently truncated. Serial / USB-HID sized single frames
// are TraceView's concern, not this class's.
constexpr std::size_t kFrameScratchSize = kEspNowMaxFrameSize;

// The sealed copy of one single-frame payload. A whole logical message that
// needs fragmenting seals into the caller's seal_scratch instead (its size is a
// deployment choice); this stack buffer only covers the one-frame case, whose
// sealed payload cannot exceed the ESP-NOW payload ceiling by definition.
constexpr std::size_t kSingleFrameSealScratchSize = kEspNowMaxPayloadSize;

std::uint16_t fragmenting_flags(std::uint8_t fragment_count,
                                bool encrypted) noexcept {
    std::uint16_t flags = 0U;
    if (fragment_count > 1U) flags |= kFlagFragmented;
    if (encrypted) flags |= kFlagEncrypted;
    return flags;
}

}  // namespace

Endpoint::Endpoint() noexcept
    : source_id_(0U), boot_id_(0U), next_sequence_(0U) {}

bool Endpoint::configure(std::uint32_t source_id,
                         std::uint32_t boot_id) noexcept {
    if (source_id == 0U || boot_id == 0U) return false;
    source_id_ = source_id;
    boot_id_ = boot_id;
    next_sequence_.store(1U, std::memory_order_release);
    return true;
}

bool Endpoint::configured() const noexcept {
    return source_id_ != 0U && boot_id_ != 0U;
}

bool Endpoint::reserve_sequence(std::uint32_t* sequence_out) noexcept {
    if (sequence_out == nullptr || !configured()) return false;

    std::uint32_t current = next_sequence_.load(std::memory_order_acquire);
    while (current != 0U) {
        const std::uint32_t next =
            current == std::numeric_limits<std::uint32_t>::max() ? 0U
                                                                 : current + 1U;
        if (next_sequence_.compare_exchange_weak(current, next,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            *sequence_out = current;
            return true;
        }
    }
    return false;
}

bool Endpoint::try_reserve_sequence(std::uint32_t* sequence_out) noexcept {
    if (sequence_out == nullptr || !configured()) return false;

    std::uint32_t current = next_sequence_.load(std::memory_order_acquire);
    if (current == 0U) return false;
    const std::uint32_t next =
        current == std::numeric_limits<std::uint32_t>::max() ? 0U : current + 1U;
    if (!next_sequence_.compare_exchange_strong(current, next,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        return false;
    }
    *sequence_out = current;
    return true;
}

bool Endpoint::encode_fragment(const LogicalMessage& message,
                               TransportProfile transport,
                               std::uint32_t sequence,
                               std::uint8_t fragment_index,
                               std::uint8_t fragment_count, std::uint8_t* output,
                               std::size_t output_capacity,
                               std::size_t* bytes_written, EndpointSealFn seal,
                               void* seal_context) const noexcept {
    if (!configured() || sequence == 0U || fragment_count == 0U ||
        fragment_index >= fragment_count || output == nullptr ||
        bytes_written == nullptr ||
        (message.payload.data == nullptr && message.payload.size != 0U)) {
        return false;
    }
    if (message.payload.size > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }

    const btp::Header header{
        message.type,
        fragmenting_flags(fragment_count, seal != nullptr),
        source_id_,
        boot_id_,
        sequence,
        message.timestamp_us,
        message.object_id,
        fragment_index,
        fragment_count,
    };

    if (seal == nullptr) {
        const btp::Frame frame{header, message.payload};
        return btp::encode(frame, transport, output, output_capacity,
                           bytes_written) == btp::Error::Ok;
    }

    // The AEAD tag covers the whole logical payload, never a slice of one, so a
    // fragment of a larger message cannot seal here.
    if (fragment_count != 1U) return false;
    if (message.payload.size + kEndpointAeadTagSize > kSingleFrameSealScratchSize) {
        return false;
    }

    // Sealed once, in place of the plaintext -- ENCRYPTED is already set on
    // `header` above, since the associated data is computed from the header as
    // given (docs/encryption.md section 5).
    std::uint8_t sealed[kSingleFrameSealScratchSize];
    if (!seal(seal_context, header,
              static_cast<std::uint16_t>(message.payload.size),
              message.payload.data, sealed)) {
        return false;
    }
    const btp::Frame frame{
        header, {sealed, message.payload.size + kEndpointAeadTagSize}};
    return btp::encode(frame, transport, output, output_capacity,
                       bytes_written) == btp::Error::Ok;
}

bool Endpoint::send_fragment(const LogicalMessage& message,
                             TransportProfile transport, std::uint32_t sequence,
                             std::uint8_t fragment_index,
                             std::uint8_t fragment_count, EndpointSendFn send,
                             void* send_context, EndpointSealFn seal,
                             void* seal_context) const noexcept {
    if (send == nullptr) return false;
    std::uint8_t frame[kFrameScratchSize];
    std::size_t frame_size = 0U;
    if (!encode_fragment(message, transport, sequence, fragment_index,
                         fragment_count, frame, sizeof(frame), &frame_size, seal,
                         seal_context)) {
        return false;
    }
    return send(send_context, frame, frame_size);
}

bool Endpoint::send_encoded(const std::uint8_t* frame, std::size_t frame_size,
                            TransportProfile transport, EndpointSendFn send,
                            void* send_context) const noexcept {
    if (frame == nullptr || send == nullptr ||
        frame_size < btp::kV1MinimumFrameSize ||
        frame_size > btp::max_frame_size(transport)) {
        return false;
    }
    return send(send_context, frame, frame_size);
}

bool Endpoint::send_logical(const LogicalMessage& message,
                            TransportProfile transport, EndpointSendFn send,
                            void* send_context, std::uint8_t* seal_scratch,
                            std::size_t seal_scratch_capacity,
                            EndpointSealFn seal, void* seal_context) noexcept {
    std::uint32_t sequence = 0U;
    if (!reserve_sequence(&sequence)) return false;
    return send_logical_impl(sequence, message, transport, send, send_context,
                             seal_scratch, seal_scratch_capacity, seal,
                             seal_context);
}

bool Endpoint::send_logical_reserved(std::uint32_t sequence,
                                     const LogicalMessage& message,
                                     TransportProfile transport,
                                     EndpointSendFn send, void* send_context,
                                     std::uint8_t* seal_scratch,
                                     std::size_t seal_scratch_capacity,
                                     EndpointSealFn seal,
                                     void* seal_context) const noexcept {
    if (sequence == 0U) return false;
    return send_logical_impl(sequence, message, transport, send, send_context,
                             seal_scratch, seal_scratch_capacity, seal,
                             seal_context);
}

bool Endpoint::send_logical_impl(std::uint32_t sequence,
                                 const LogicalMessage& message,
                                 TransportProfile transport, EndpointSendFn send,
                                 void* send_context, std::uint8_t* seal_scratch,
                                 std::size_t seal_scratch_capacity,
                                 EndpointSealFn seal,
                                 void* seal_context) const noexcept {
    if (!configured() || send == nullptr ||
        (message.payload.data == nullptr && message.payload.size != 0U)) {
        return false;
    }

    // ----- cleartext: slice the plaintext directly, one frame at a time -----
    if (seal == nullptr) {
        std::uint8_t count = 0U;
        if (btp::fragment_count(message.payload.size, transport, &count) !=
            btp::Error::Ok) {
            return false;
        }
        const std::size_t limit = btp::max_payload_size(transport);
        for (std::uint8_t index = 0U; index < count; ++index) {
            const std::size_t offset = static_cast<std::size_t>(index) * limit;
            const std::size_t remaining = message.payload.size - offset;
            const std::size_t fragment_size = remaining < limit ? remaining : limit;
            const LogicalMessage fragment{
                message.type, message.object_id, message.timestamp_us,
                {message.payload.data == nullptr ? nullptr
                                                 : message.payload.data + offset,
                 fragment_size}};
            if (!send_fragment(fragment, transport, sequence, index, count, send,
                               send_context)) {
                return false;
            }
        }
        return true;
    }

    // ----- sealed: one seal over the canonical header, then slice the SEALED
    // bytes with make_fragment so every fragment's header matches the one that
    // was actually sealed (docs/encryption.md section 5) -----
    if (message.payload.size > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    const std::size_t sealed_size = message.payload.size + kEndpointAeadTagSize;
    if (seal_scratch == nullptr || seal_scratch_capacity < sealed_size) {
        return false;
    }

    const btp::Header seal_header{
        message.type,
        kFlagEncrypted,
        source_id_,
        boot_id_,
        sequence,
        message.timestamp_us,
        message.object_id,
        0U,
        1U,
    };
    if (!seal(seal_context, seal_header,
              static_cast<std::uint16_t>(message.payload.size),
              message.payload.data, seal_scratch)) {
        return false;
    }

    std::uint8_t count = 0U;
    if (btp::fragment_count(sealed_size, transport, &count) != btp::Error::Ok) {
        return false;
    }

    btp::Header logical_header = seal_header;
    logical_header.flags = fragmenting_flags(count, /*encrypted=*/true);
    logical_header.fragment_count = count;

    for (std::uint8_t index = 0U; index < count; ++index) {
        btp::Frame fragment{};
        if (btp::make_fragment(logical_header, {seal_scratch, sealed_size},
                               transport, index, &fragment) != btp::Error::Ok) {
            return false;
        }
        std::uint8_t frame[kFrameScratchSize];
        std::size_t frame_size = 0U;
        if (btp::encode(fragment, transport, frame, sizeof(frame),
                        &frame_size) != btp::Error::Ok) {
            return false;
        }
        if (!send(send_context, frame, frame_size)) return false;
    }
    return true;
}

}  // namespace btp
