#ifndef BTP_ENDPOINT_HPP
#define BTP_ENDPOINT_HPP

// The transmit side of a BTP endpoint -- the local identity, the outgoing
// sequence counter and the seal -> fragment -> encode pipeline that every
// producer runs to put a logical message on the wire.
//
// btp::codec encodes one frame. btp::fragmentation slices a logical payload
// into frames and puts them back together. Neither remembers who *this* node
// is, hands out the next sequence number, or knows the exact order the steps
// go in when a message is both encrypted and fragmented:
//
//   1. build the CANONICAL logical header -- FRAGMENTED cleared, fragment_index
//      0, fragment_count 1, ENCRYPTED already set (docs/encryption.md section 5);
//   2. seal the WHOLE logical payload once against that header;
//   3. compute fragment_count from the SEALED size (plaintext + 16), never the
//      plaintext size -- a message that fits one frame unsealed can need two
//      once the tag is added, and hand-slicing the plaintext would lose the
//      tail;
//   4. make_fragment / encode each wire frame from the sealed bytes.
//
// Getting step 3 wrong is an off-by-one that silently truncates an encrypted
// message. btp::Endpoint is that pipeline written once, with the same
// guarantees as the rest of the library:
//
//   * no internal allocation -- identity and a sequence counter are the only
//     state; the seal scratch buffer for the fragmented-and-encrypted path is
//     the caller's (its size is a deployment choice: a dongle cannot afford the
//     buffer a desktop can);
//   * noexcept -- every method returns bool ("did the whole logical message go
//     out"), never throws;
//   * no I/O -- an encoded frame is handed to a caller EndpointSendFn; the
//     endpoint never touches a socket, a radio or a file;
//   * no clock, no global state.
//
// It adds no wire field. This is library 2.7.0 territory.
//
// OUT of scope, on purpose (docs/library.md chapter 11 still applies):
//   * ADDRESSING and ROUTING -- which MAC / socket / peer a frame goes to, and
//     any per-peer identity table, are the integration's (model.md section 5:
//     BTP defines no routing identifiers). The EndpointSendFn hides all of it.
//   * DELIVERY confirmation -- whether a frame was acknowledged. A caller that
//     needs it accumulates the per-frame result in its own send context.
//   * the RECEIVE path -- btp::decode() + btp::Reassembler already cover it, and
//     the routing decision on top diverges by role (a hub relays by default, an
//     endpoint does not), so there is no shared receiver here.
//   * link FRAMING -- COBS, HID report padding, ESP-NOW datagram boundaries are
//     btp::stream's / the transport's, applied to the bytes the send callback
//     receives.
//   * the SESSION state machine -- HELLO negotiation, the inactivity watchdog
//     and the console<->protocol transition are the integration's
//     (docs/session-and-terminal.md); btp::DedupCache is the one stateful piece
//     the library runs, and it is receive-side.

#include "btp/codec.hpp"  // Header, Frame, ByteView, TransportLimits, MessageType

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace btp {

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

// Seals ONE whole logical message before it is fragmented. `header` is the
// canonical logical header with ENCRYPTED already set -- the associated data is
// computed from it AS GIVEN (docs/encryption.md section 5), so the flag must be
// set before sealing, not patched in after. `plaintext` / `payload_size` are
// the logical payload; `out` has room for exactly
// `payload_size + kEndpointAeadTagSize` octets. Returns false when there is no
// key or the seal genuinely fails -- the endpoint then sends NOTHING (an
// unsealed frame must never reach the wire as a fallback). nullptr as the seal
// argument means "do not encrypt".
using EndpointSealFn = bool (*)(void* context, const Header& header,
                                std::uint16_t payload_size,
                                const std::uint8_t* plaintext,
                                std::uint8_t* out);

// Hands ONE fully encoded wire frame to the transport. Return false to abort
// the remaining fragments of the current logical message (send_logical then
// returns false too). The endpoint does not retry and keeps no partial state.
using EndpointSendFn = bool (*)(void* context, const std::uint8_t* frame,
                                std::size_t frame_size);

// docs/encryption.md section 2: sealing always grows the payload by exactly
// this many octets, regardless of cipher.
static const std::size_t kEndpointAeadTagSize = 16U;

// ---------------------------------------------------------------------------
// One logical message
// ---------------------------------------------------------------------------
//
// For send_logical / send_logical_reserved, `payload` is the COMPLETE logical
// payload (always plaintext -- the caller never seals) and the endpoint slices
// it into frames. For encode_fragment / send_fragment, `payload` is exactly the
// bytes of the one frame being built and `fragment_index` / `fragment_count`
// are supplied separately.

struct LogicalMessage {
    MessageType type;
    std::uint16_t object_id;
    std::uint64_t timestamp_us;
    ByteView payload;
};

// ---------------------------------------------------------------------------
// Endpoint
// ---------------------------------------------------------------------------
//
//   btp::Endpoint endpoint;
//   endpoint.configure(source_id_from_mac(mac), boot_id);
//
//   // fire-and-forget a logical message, sealed on channel C:
//   std::uint8_t scratch[kMaxLogicalPayload + btp::kEndpointAeadTagSize];
//   btp::LogicalMessage msg{btp::MessageType::Control, kStatusObjectId, now_us,
//                           {payload, payload_size}};
//   endpoint.send_logical(msg, btp::kEspNowTransport,
//                         &my_send, &radio, scratch, sizeof(scratch),
//                         &RadioSeal::seal, &seal_ctx);
//
// CONCURRENCY. reserve_sequence() / try_reserve_sequence() are the only methods
// safe to call from more than one context at once -- the sequence counter is
// atomic because a telemetry task and a command task both draw from it.
// configure() runs once at boot; every send method reads identity without
// locking and expects it not to change under them.

class Endpoint {
public:
    Endpoint() noexcept;

    // Sets this boot's identity. Both must be non-zero (BTP reserves 0 for
    // both fields). Returns false and changes nothing otherwise. Resets the
    // sequence counter to 1. Call once, before any send.
    bool configure(std::uint32_t source_id, std::uint32_t boot_id) noexcept;
    bool configured() const noexcept;

    std::uint32_t source_id() const noexcept { return source_id_; }
    std::uint32_t boot_id() const noexcept { return boot_id_; }

    // Reserves the next outgoing logical-message sequence. Zero is never
    // returned; once the counter reaches it, it stays there and every call
    // fails (the sequence space for this boot is exhausted). The CAS-loop form
    // always succeeds while sequences remain; the try_ form does a single CAS
    // and may fail under contention even when they do -- a hard non-blocking
    // producer should drop or count rather than spin.
    bool reserve_sequence(std::uint32_t* sequence_out) noexcept;
    bool try_reserve_sequence(std::uint32_t* sequence_out) noexcept;

    // Encodes `message` and sends it as one or more wire frames, each handed to
    // `send`. A fresh sequence is reserved. `seal` may be null (cleartext, no
    // scratch needed -- pass {nullptr, 0}); when non-null, `seal_scratch` must
    // hold at least `message.payload.size + kEndpointAeadTagSize` octets (the
    // sealed copy the fragments are cut from). Fails closed: a false from
    // `seal` or `send`, an oversized payload, or an unconfigured endpoint sends
    // nothing further and returns false. Best-effort past the first frame:
    // there is no rollback of frames already sent.
    bool send_logical(const LogicalMessage& message, const TransportLimits& transport,
                      EndpointSendFn send, void* send_context,
                      std::uint8_t* seal_scratch,
                      std::size_t seal_scratch_capacity,
                      EndpointSealFn seal = nullptr,
                      void* seal_context = nullptr) noexcept;

    // Same pipeline with a sequence a non-blocking producer already reserved --
    // keeps a queued large sample in the same order as its one-frame
    // neighbours. const: it does not touch the counter.
    bool send_logical_reserved(std::uint32_t sequence,
                               const LogicalMessage& message,
                               const TransportLimits& transport, EndpointSendFn send,
                               void* send_context, std::uint8_t* seal_scratch,
                               std::size_t seal_scratch_capacity,
                               EndpointSealFn seal = nullptr,
                               void* seal_context = nullptr) const noexcept;

    // Encodes exactly one physical frame into `output`. `message.payload` is
    // the payload of THIS frame (not a whole logical message), and
    // `fragment_index` / `fragment_count` go straight into the header. When
    // `seal` is non-null, `fragment_count` MUST be 1: the AEAD tag covers a
    // whole logical payload, never a slice, so sealing a mid-message fragment
    // is refused rather than silently wrong, and the sealed payload must fit
    // one ESP-NOW frame (`payload.size + kEndpointAeadTagSize <=
    // kEspNowMaxPayloadSize`).
    bool encode_fragment(const LogicalMessage& message,
                         const TransportLimits& transport, std::uint32_t sequence,
                         std::uint8_t fragment_index,
                         std::uint8_t fragment_count, std::uint8_t* output,
                         std::size_t output_capacity, std::size_t* bytes_written,
                         EndpointSealFn seal = nullptr,
                         void* seal_context = nullptr) const noexcept;

    // encode_fragment() then hand the frame to `send`.
    bool send_fragment(const LogicalMessage& message, const TransportLimits& transport,
                       std::uint32_t sequence, std::uint8_t fragment_index,
                       std::uint8_t fragment_count, EndpointSendFn send,
                       void* send_context, EndpointSealFn seal = nullptr,
                       void* seal_context = nullptr) const noexcept;

    // Hands an already-encoded frame straight to `send` with no re-encode and
    // no CRC recomputation -- the octets reach the wire byte for byte. Used on
    // a relay / playback path where the header carries another endpoint's AEAD
    // nonce and the CRC covers its sealed payload. Rejects octets that cannot
    // be a frame at all (shorter than the minimum, longer than the transport's
    // frame ceiling); does not otherwise validate them.
    bool send_encoded(const std::uint8_t* frame, std::size_t frame_size,
                      const TransportLimits& transport, EndpointSendFn send,
                      void* send_context) const noexcept;

private:
    bool send_logical_impl(std::uint32_t sequence, const LogicalMessage& message,
                           const TransportLimits& transport, EndpointSendFn send,
                           void* send_context, std::uint8_t* seal_scratch,
                           std::size_t seal_scratch_capacity, EndpointSealFn seal,
                           void* seal_context) const noexcept;

    std::uint32_t source_id_;
    std::uint32_t boot_id_;
    std::atomic<std::uint32_t> next_sequence_;
};

}  // namespace btp

#endif  // BTP_ENDPOINT_HPP
