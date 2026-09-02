#include "btp/receiver.hpp"

#include <cstring>

namespace btp {
namespace {

// Mirror of the transport check btp::decode() does; used so valid() can report
// a bad profile at boot instead of every submit() silently dropping.
bool transport_recognised(TransportProfile transport) noexcept {
    return transport == TransportProfile::EspNow ||
           transport == TransportProfile::Serial ||
           transport == TransportProfile::UsbHid;
}

}  // namespace

Receiver::Receiver(ReassemblySlot* slots, const ReassemblyStorage* storage,
                   std::size_t slot_count, std::uint64_t timeout_ms,
                   TransportProfile transport) noexcept
    : reassembler_(slots, storage, slot_count, timeout_ms),
      transport_(transport),
      transport_valid_(transport_recognised(transport)),
      stats_() {}

bool Receiver::valid() const noexcept {
    return transport_valid_ && reassembler_.valid();
}

std::size_t Receiver::slot_count() const noexcept {
    return reassembler_.slot_count();
}

std::size_t Receiver::expire(std::uint64_t now_ms) noexcept {
    const std::size_t reclaimed = reassembler_.expire(now_ms);
    stats_.reassembly_timeouts += static_cast<std::uint32_t>(reclaimed);
    return reclaimed;
}

void Receiver::clear() noexcept {
    reassembler_.clear();
    stats_ = Stats();
}

Receiver::Stats Receiver::stats() const noexcept {
    return stats_;
}

ReceiveOutcome Receiver::submit(const std::uint8_t* data, std::size_t size,
                                std::uint64_t now_ms, std::uint8_t* out_payload,
                                std::size_t out_capacity,
                                ReceivedMessage* message_out) noexcept {
    // Sweep here, on this context, rather than from a timer on another task --
    // this is what lets the loss be counted without a second context ever
    // touching the slot table. btp::Reassembler::push() expires stale slots
    // itself further down anyway; doing it explicitly first is only for the
    // count.
    expire(now_ms);

    if (data == nullptr || size == 0U || out_payload == nullptr ||
        message_out == nullptr) {
        ++stats_.invalid_argument;
        return ReceiveOutcome::InvalidArgument;
    }

    DecodedFrame decoded{};
    const Error error = btp::decode(data, size, transport_, &decoded);
    if (error != Error::Ok) {
        // A frame rejected by CRC is never also a decode error: STATUS reports
        // the two apart because they mean different things (radio corruption
        // versus a peer speaking the wrong dialect).
        if (error == Error::CrcMismatch) {
            ++stats_.dropped_crc;
            return ReceiveOutcome::DroppedCrc;
        }
        ++stats_.dropped_decode;
        return ReceiveOutcome::DroppedDecode;
    }

    return ingest(decoded, now_ms, out_payload, out_capacity, message_out);
}

ReceiveOutcome Receiver::submit(const DecodedFrame& frame, std::uint64_t now_ms,
                                std::uint8_t* out_payload,
                                std::size_t out_capacity,
                                ReceivedMessage* message_out) noexcept {
    expire(now_ms);

    if (out_payload == nullptr || message_out == nullptr ||
        (frame.payload.data == nullptr && frame.payload.size != 0U)) {
        ++stats_.invalid_argument;
        return ReceiveOutcome::InvalidArgument;
    }

    return ingest(frame, now_ms, out_payload, out_capacity, message_out);
}

ReceiveOutcome Receiver::ingest(const DecodedFrame& decoded,
                                std::uint64_t now_ms, std::uint8_t* out_payload,
                                std::size_t out_capacity,
                                ReceivedMessage* message_out) noexcept {
    if ((decoded.header.flags & kFlagFragmented) == 0U) {
        // btp::decode()'s header validation already required fragment_index 0
        // and fragment_count 1 here, so this datagram IS the whole logical
        // message and no slot is involved. Its payload still points into the
        // caller's transient input, so copy it out now for a uniform contract.
        if (decoded.payload.size > out_capacity) {
            ++stats_.invalid_argument;
            return ReceiveOutcome::InvalidArgument;
        }
        if (decoded.payload.size != 0U) {
            std::memcpy(out_payload, decoded.payload.data, decoded.payload.size);
        }
        message_out->header = decoded.header;
        message_out->payload = {out_payload, decoded.payload.size};
        message_out->reassembled = false;
        ++stats_.completed;
        return ReceiveOutcome::Complete;
    }

    ReassembledMessage completed{};
    const ReassemblyEvent event = reassembler_.push(decoded, now_ms, &completed);
    switch (event) {
        case ReassemblyEvent::Accepted:
            ++stats_.fragments_accepted;
            return ReceiveOutcome::FragmentAccepted;
        case ReassemblyEvent::Duplicate:
            ++stats_.duplicate_fragments;
            return ReceiveOutcome::DuplicateFragment;
        case ReassemblyEvent::Complete:
            if (completed.payload.size > out_capacity) {
                // Release the slot even though the caller's buffer is too
                // small -- the message is lost (a caller bug), but a leaked
                // slot would take the pool down with it.
                reassembler_.release(completed.slot_index);
                ++stats_.invalid_argument;
                return ReceiveOutcome::InvalidArgument;
            }
            if (completed.payload.size != 0U) {
                std::memcpy(out_payload, completed.payload.data,
                            completed.payload.size);
            }
            message_out->header = completed.header;
            message_out->payload = {out_payload, completed.payload.size};
            message_out->reassembled = true;
            // Release immediately. What travels onward is the copy above, not
            // the slot, so a busy handler downstream never holds a slot
            // another sender needs -- the failure the small pool is built to
            // hit.
            reassembler_.release(completed.slot_index);
            ++stats_.completed;
            return ReceiveOutcome::Complete;
        case ReassemblyEvent::InvalidFragment:
        case ReassemblyEvent::Conflict:
        case ReassemblyEvent::MessageTooLarge:
        case ReassemblyEvent::NoSlot:
            ++stats_.dropped_reassembly;
            return ReceiveOutcome::DroppedReassembly;
        case ReassemblyEvent::InvalidArgument:
            ++stats_.invalid_argument;
            return ReceiveOutcome::InvalidArgument;
    }

    // Unreachable while the enum above is exhaustive; counted rather than
    // asserted so a future BTP event can never silently become "complete".
    ++stats_.invalid_argument;
    return ReceiveOutcome::InvalidArgument;
}

const char* receive_outcome_string(ReceiveOutcome outcome) noexcept {
    switch (outcome) {
        case ReceiveOutcome::Complete: return "complete";
        case ReceiveOutcome::FragmentAccepted: return "fragment accepted";
        case ReceiveOutcome::DuplicateFragment: return "duplicate fragment";
        case ReceiveOutcome::DroppedCrc: return "dropped: crc mismatch";
        case ReceiveOutcome::DroppedDecode: return "dropped: decode error";
        case ReceiveOutcome::DroppedReassembly: return "dropped: reassembly";
        case ReceiveOutcome::InvalidArgument: return "invalid argument";
    }
    return "unknown";
}

}  // namespace btp
