#ifndef BTP_RECEIVER_HPP
#define BTP_RECEIVER_HPP

// The receive side of a BTP endpoint -- the decode + CRC + reassembly stage
// every consumer runs before it can look at a logical message. It is the
// mirror of btp::Endpoint (docs/library.md chapter 14): where the endpoint
// turns one logical message into wire frames, the receiver turns a stream of
// wire datagrams back into whole logical messages.
//
// btp::decode() validates one frame and btp::Reassembler puts fragments back
// together, but the wiring between them is the same everywhere and easy to get
// subtly wrong: sweep expired partials first so the loss can be counted,
// branch on FRAGMENTED, feed only fragments to the reassembler, copy a
// completed payload out and release its slot *immediately* so a slow handler
// downstream cannot starve a four-slot pool. btp::Receiver is that wiring
// written once, with the same guarantees as the rest of the library:
//
//   * no internal allocation -- the caller owns the reassembly slot array and
//     one byte region per slot (exactly btp::Reassembler's storage), plus the
//     output buffer submit() copies a completed message into;
//   * noexcept -- outcomes are returned, never thrown;
//   * no I/O, no global state;
//   * a clock, but not its own -- submit()/expire() take now_ms, the same way
//     btp::Reassembler does, so the timeout needs no time source of its own.
//
// It adds no wire field. This is library 2.8.0 territory.
//
// NOT internally synchronised. Exactly one context calls submit() (the RX
// path); stats() returns a by-value snapshot whose counters are plain 32-bit
// words -- a reader on another thread sees a relaxed view of monotonic
// counters, which is all a STATUS report needs. A caller that runs RX on more
// than one thread wraps submit() in its own critical section, the same way
// btp::Reassembler expects a single consumer.
//
// OUT of scope, on purpose (docs/library.md chapter 11 still applies):
//   * ROUTING -- deciding who handles a completed message, and whether an
//     unrecognised one is relayed (a hub) or dropped (an endpoint), is the
//     integration's one switch on top of this.
//   * OPENING an encrypted payload -- aead_open() runs after reassembly, on
//     the whole logical payload; btp::Receiver hands back the sealed bytes.
//   * link FRAMING -- COBS / HID report de-padding / ESP-NOW datagram
//     boundaries are btp::stream's or the transport's. The datagram submit()
//     wants one already-bounded frame candidate; the DecodedFrame submit()
//     takes btp::SerialDecoder's output directly.

#include "btp/codec.hpp"          // decode, Header, DecodedFrame, ByteView, TransportLimits
#include "btp/fragmentation.hpp"  // Reassembler and its storage types

#include <cstddef>
#include <cstdint>

namespace btp {

// ---------------------------------------------------------------------------
// The outcome of one submit()
// ---------------------------------------------------------------------------

enum class ReceiveOutcome : std::uint8_t {
    // *message_out is a complete logical message. Its payload has been copied
    // into the caller's output buffer and the reassembly slot (if any) is
    // already released.
    Complete,

    // One fragment of a still-incomplete message was stored. Nothing to
    // deliver yet.
    FragmentAccepted,

    // A byte-identical retransmission of a fragment already held. Absorbed;
    // nothing corrupted, nothing to deliver.
    DuplicateFragment,

    // btp::decode() rejected the frame's CRC-32 -- radio corruption. Counted
    // apart from DroppedDecode because the two mean different things.
    DroppedCrc,

    // btp::decode() rejected the frame for any other reason (bad magic,
    // unsupported version, a length that does not add up, a peer speaking the
    // wrong dialect).
    DroppedDecode,

    // btp::Reassembler rejected the fragment: Conflict (same identity, a
    // fragment that disagrees), InvalidFragment, MessageTooLarge, or NoSlot
    // (every slot busy with another sender).
    DroppedReassembly,

    // A null pointer, a zero-length datagram, or an output buffer smaller than
    // a slot's storage region.
    InvalidArgument,
};

const char* receive_outcome_string(ReceiveOutcome outcome) noexcept;

// ---------------------------------------------------------------------------
// A complete logical message
// ---------------------------------------------------------------------------

struct ReceivedMessage {
    // Normalised by btp::Reassembler on completion: FRAGMENTED cleared,
    // fragment_index 0, fragment_count 1. A consumer cannot tell -- and must
    // not care -- whether this arrived in one frame or in twelve.
    Header header;

    // Points into the output buffer the caller passed to submit(); a copy, so
    // it is safe to queue, and valid until the next submit() overwrites that
    // buffer. Copied even for a one-frame message, so the contract is uniform.
    ByteView payload;

    // True only when the message came out of the reassembly slot pool.
    // STATUS's reassembly_completed counts reassemblies, not receptions, and
    // the normalised header above cannot say which this was.
    bool reassembled;
};

// ---------------------------------------------------------------------------
// Receiver
// ---------------------------------------------------------------------------
//
//   btp::ReassemblySlot slot_array[4];  // NOT "slots" -- Qt's <QObject> macro eats it
//   std::uint8_t storage_bytes[4][kMaxPayload];
//   btp::ReassemblyStorage storage[4];
//   for (std::size_t i = 0; i < 4; ++i) storage[i] = {storage_bytes[i], kMaxPayload};
//   btp::Receiver receiver(slot_array, storage, 4, 4000, btp::kEspNowTransport);
//
//   std::uint8_t out[kMaxPayload];
//   btp::ReceivedMessage msg{};
//   switch (receiver.submit(datagram, datagram_size, now_ms, out, sizeof(out), &msg)) {
//       case btp::ReceiveOutcome::Complete:
//           route(msg.header, msg.payload);   // the integration's one switch
//           break;
//       default: break;                        // partial / duplicate / dropped
//   }

class Receiver {
public:
    // slots / storage / slot_count / timeout_ms are btp::Reassembler's, forwarded
    // to the Reassembler this object holds. transport is the TransportLimits
    // btp::decode() applies (kEspNowTransport on firmware; kSerialTransport is
    // the widest ceiling of the three presets).
    // slot_array, not slots -- see Reassembler's own constructor comment
    // (btp/fragmentation.hpp) on the Qt <QObject> macro collision.
    Receiver(ReassemblySlot* slot_array, const ReassemblyStorage* storage,
             std::size_t slot_count, std::uint64_t timeout_ms,
             const TransportLimits& transport) noexcept;

    // True when the reassembler wiring is sound and the transport limits are
    // usable (detail::valid_transport()). Check once at boot.
    bool valid() const noexcept;

    std::size_t slot_count() const noexcept;

    // Feed one received datagram -- one already-bounded BTP frame candidate.
    // Sweeps expired partials first (each counted as a reassembly_timeout),
    // then btp::decode()s and, for a fragment, feeds btp::Reassembler. On
    // Complete, the logical payload is copied into `out_payload` and the slot
    // released before returning. `out_capacity` MUST be at least a slot's
    // storage capacity; anything less is InvalidArgument. `out_payload` may
    // alias nothing else in use. Every non-Complete outcome leaves
    // *message_out untouched.
    ReceiveOutcome submit(const std::uint8_t* data, std::size_t size,
                          std::uint64_t now_ms, std::uint8_t* out_payload,
                          std::size_t out_capacity,
                          ReceivedMessage* message_out) noexcept;

    // Same, for a frame btp::decode() (or btp::SerialDecoder) already
    // accepted -- the COBS / serial receive path. Runs only the reassembly
    // stage: dropped_crc and dropped_decode are never touched here because the
    // decoder that produced `frame` already accounted for those.
    ReceiveOutcome submit(const DecodedFrame& frame, std::uint64_t now_ms,
                          std::uint8_t* out_payload, std::size_t out_capacity,
                          ReceivedMessage* message_out) noexcept;

    // Reclaim slots whose last fragment is older than timeout_ms and return
    // how many. Also run at the top of every submit(); exposed for a caller
    // (or a test) that wants to sweep on an idle tick. Each reclaimed slot
    // bumps stats().reassembly_timeouts.
    std::size_t expire(std::uint64_t now_ms) noexcept;

    // Drop every in-flight reassembly and zero the counters -- a fresh
    // connection, or test isolation.
    void clear() noexcept;

    struct Stats {
        std::uint32_t completed;            // Complete outcomes
        std::uint32_t fragments_accepted;   // FragmentAccepted
        std::uint32_t duplicate_fragments;  // DuplicateFragment
        std::uint32_t dropped_crc;          // DroppedCrc
        std::uint32_t dropped_decode;       // DroppedDecode
        std::uint32_t dropped_reassembly;   // DroppedReassembly
        std::uint32_t invalid_argument;     // InvalidArgument
        std::uint32_t reassembly_timeouts;  // partials expired without completing
    };
    Stats stats() const noexcept;

private:
    ReceiveOutcome ingest(const DecodedFrame& decoded, std::uint64_t now_ms,
                          std::uint8_t* out_payload, std::size_t out_capacity,
                          ReceivedMessage* message_out) noexcept;

    Reassembler reassembler_;
    TransportLimits transport_;
    bool transport_valid_;
    Stats stats_;
};

}  // namespace btp

#endif  // BTP_RECEIVER_HPP
