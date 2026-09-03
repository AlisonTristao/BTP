#include "btp/node.hpp"

namespace btp {

Node::Node(const NodeConfig& cfg, ReassemblySlot* slots,
           const ReassemblyStorage* storage, std::size_t slot_count,
           std::uint64_t reassembly_timeout_ms, std::uint8_t* rx_buffer,
           std::size_t rx_capacity, std::uint8_t* seal_scratch,
           std::size_t seal_scratch_cap, std::uint8_t* open_buffer,
           std::size_t open_capacity) noexcept
    : cfg_(cfg),
      rx_buffer_(rx_buffer),
      rx_capacity_(rx_capacity),
      seal_scratch_(seal_scratch),
      seal_scratch_cap_(seal_scratch_cap),
      open_buffer_(open_buffer),
      open_capacity_(open_capacity),
      endpoint_(),
      receiver_(slots, storage, slot_count, reassembly_timeout_ms,
                cfg.transport),
      session_(Hello{}, 0U),
      session_on_(false),
      last_session_event_(SessionEvent::None),
      session_path_dropped_crc_(0U),
      session_path_dropped_decode_(0U) {}

bool Node::begin() noexcept {
    if (!endpoint_.configure(cfg_.source_id, cfg_.boot_id)) return false;
    if (!receiver_.valid()) return false;
    if (session_on_ && !session_.valid()) return false;
    // cfg_.send is not required here: a receive-only node that never sends and
    // never enables a session does not need one. send() / send_with() and a
    // session reply check for it at the point of use.
    return true;
}

bool Node::configured() const noexcept {
    return endpoint_.configured() && receiver_.valid() &&
           (!session_on_ || session_.valid());
}

std::uint64_t Node::resolve_now(std::uint64_t fallback) const noexcept {
    return cfg_.clock != nullptr ? cfg_.clock(cfg_.clock_ctx) : fallback;
}

// ---------------------------------------------------------------------------
// Transmit
// ---------------------------------------------------------------------------

bool Node::send_with(MessageType type, std::uint16_t object_id,
                     const std::uint8_t* payload, std::size_t size,
                     std::uint64_t timestamp_us, EndpointSealFn seal,
                     void* seal_ctx) noexcept {
    if (cfg_.send == nullptr) return false;
    const LogicalMessage message{type, object_id, timestamp_us,
                                 {payload, size}};
    return endpoint_.send_logical(message, cfg_.transport, cfg_.send,
                                  cfg_.send_ctx, seal_scratch_,
                                  seal_scratch_cap_, seal, seal_ctx);
}

bool Node::send(MessageType type, std::uint16_t object_id,
                const std::uint8_t* payload, std::size_t size,
                std::uint64_t timestamp_us) noexcept {
    return send_with(type, object_id, payload, size, timestamp_us, cfg_.seal,
                     cfg_.seal_ctx);
}

// ---------------------------------------------------------------------------
// Receive
// ---------------------------------------------------------------------------

NodeRx Node::receive(const std::uint8_t* datagram, std::size_t size,
                     ReceivedMessage* out) noexcept {
    return receive(datagram, size, resolve_now(0U), out);
}

NodeRx Node::receive(const std::uint8_t* datagram, std::size_t size,
                     std::uint64_t now_ms, ReceivedMessage* out) noexcept {
    last_session_event_ = SessionEvent::None;
    if (out == nullptr || datagram == nullptr || size == 0U) {
        return NodeRx::DroppedFrame;
    }

    if (!session_on_) {
        return finish(receiver_.submit(datagram, size, now_ms, rx_buffer_,
                                       rx_capacity_, out),
                      out);
    }

    // A session needs the DecodedFrame btp::Receiver keeps to itself, so the
    // decode happens here; its failures are counted apart (stats()).
    DecodedFrame decoded{};
    const Error error = btp::decode(datagram, size, cfg_.transport, &decoded);
    if (error != Error::Ok) {
        if (error == Error::CrcMismatch) {
            ++session_path_dropped_crc_;
        } else {
            ++session_path_dropped_decode_;
        }
        return NodeRx::DroppedFrame;
    }

    std::uint8_t reply[kSessionMaxReplySize];
    const SessionOutcome outcome =
        session_.on_frame(decoded, now_ms, reply, sizeof(reply));
    last_session_event_ = outcome.event;
    if (outcome.reply_size != 0U) {
        // btp::Session produces the reply PAYLOAD (HELLO_RESULT /
        // SESSION_CLOSE_RESULT); the node frames it and puts it on the wire,
        // cleartext -- the handshake bootstraps the session before any key.
        const std::uint16_t reply_object =
            outcome.event == SessionEvent::SessionClosed
                ? object_id::kSessionCloseResult
                : object_id::kHelloResult;
        const LogicalMessage reply_msg{
            MessageType::Control, reply_object, resolve_now(0U) * 1000ULL,
            {reply, outcome.reply_size}};
        if (cfg_.send != nullptr) {
            endpoint_.send_logical(reply_msg, cfg_.transport, cfg_.send,
                                   cfg_.send_ctx, seal_scratch_,
                                   seal_scratch_cap_, nullptr, nullptr);
        }
    }
    switch (outcome.event) {
        case SessionEvent::FrameAccepted:
            break;  // a live-session application frame -- route it below
        case SessionEvent::None:
            return NodeRx::Ignored;
        case SessionEvent::HelloAccepted:
        case SessionEvent::HelloRejected:
        case SessionEvent::SessionClosed:
        case SessionEvent::TimedOut:
        case SessionEvent::Abandoned:
            return NodeRx::SessionHandled;
    }

    return finish(
        receiver_.submit(decoded, now_ms, rx_buffer_, rx_capacity_, out), out);
}

NodeRx Node::finish(ReceiveOutcome outcome, ReceivedMessage* out) noexcept {
    switch (outcome) {
        case ReceiveOutcome::Complete:
            break;
        case ReceiveOutcome::FragmentAccepted:
        case ReceiveOutcome::DuplicateFragment:
            return NodeRx::Pending;
        case ReceiveOutcome::DroppedCrc:
        case ReceiveOutcome::DroppedDecode:
        case ReceiveOutcome::DroppedReassembly:
        case ReceiveOutcome::InvalidArgument:
            return NodeRx::DroppedFrame;
    }

    // Complete. Open the sealed payload in place when a key function is set.
    if (cfg_.open != nullptr && (out->header.flags & kFlagEncrypted) != 0U) {
        if (out->payload.size < kEndpointAeadTagSize || open_buffer_ == nullptr ||
            out->payload.size - kEndpointAeadTagSize > open_capacity_) {
            return NodeRx::DroppedFrame;
        }
        if (!cfg_.open(cfg_.open_ctx, out->header,
                       static_cast<std::uint16_t>(out->payload.size),
                       out->payload.data, open_buffer_)) {
            return NodeRx::DroppedFrame;
        }
        out->payload =
            ByteView{open_buffer_, out->payload.size - kEndpointAeadTagSize};
    }
    return NodeRx::Complete;
}

// ---------------------------------------------------------------------------
// Session responder
// ---------------------------------------------------------------------------

void Node::enable_session(const Hello& local,
                          std::uint64_t hello_deadline_ms) noexcept {
    session_ = Session(local, hello_deadline_ms);
    session_on_ = true;
}

void Node::arm_session() noexcept { arm_session(resolve_now(0U)); }

void Node::arm_session(std::uint64_t now_ms) noexcept {
    if (session_on_) session_.arm(now_ms);
}

SessionEvent Node::tick() noexcept { return tick(resolve_now(0U)); }

SessionEvent Node::tick(std::uint64_t now_ms) noexcept {
    receiver_.expire(now_ms);
    if (!session_on_) return SessionEvent::None;
    const SessionOutcome outcome = session_.poll(now_ms);
    last_session_event_ = outcome.event;
    return outcome.event;
}

Node::Stats Node::stats() const noexcept {
    Stats s{};
    s.rx = receiver_.stats();
    s.session_path_dropped_crc = session_path_dropped_crc_;
    s.session_path_dropped_decode = session_path_dropped_decode_;
    return s;
}

const char* node_rx_string(NodeRx rx) noexcept {
    switch (rx) {
        case NodeRx::Complete: return "complete";
        case NodeRx::Pending: return "pending";
        case NodeRx::SessionHandled: return "session handled";
        case NodeRx::Ignored: return "ignored";
        case NodeRx::DroppedFrame: return "dropped frame";
    }
    return "unknown";
}

}  // namespace btp
