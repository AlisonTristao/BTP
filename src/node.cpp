#include "btp/node.hpp"

#include <cstring>

namespace btp {

namespace {

// NodeConfig::lock() / unlock() for one scope -- see NodeConfig::lock().
class SharedLock {
public:
    explicit SharedLock(NodeConfig& cfg) noexcept : cfg_(cfg) { cfg_.lock(); }
    ~SharedLock() { cfg_.unlock(); }
    SharedLock(const SharedLock&) = delete;
    SharedLock& operator=(const SharedLock&) = delete;

private:
    NodeConfig& cfg_;
};

}  // namespace

NodeLinkSlot::NodeLinkSlot(ReassemblySlot* slot_array,
                           const ReassemblyStorage* storage,
                           std::size_t slot_count,
                           std::uint64_t reassembly_timeout_ms,
                           const TransportLimits& transport,
                           std::uint8_t* rx_buffer, std::size_t rx_capacity,
                           std::uint8_t* open_buffer,
                           std::size_t open_capacity) noexcept
    : cfg_(nullptr),
      receiver_(slot_array, storage, slot_count, reassembly_timeout_ms, transport),
      rx_buffer_(rx_buffer),
      rx_capacity_(rx_capacity),
      open_buffer_(open_buffer),
      open_capacity_(open_capacity),
      last_receive_outcome_(ReceiveOutcome::InvalidArgument),
      session_(Hello{}, 0U),
      session_on_(false),
      last_session_event_(SessionEvent::None),
      session_path_dropped_crc_(0U),
      session_path_dropped_decode_(0U),
      dropped_cleartext_(0U),
      dropped_open_failed_(0U),
      subscriptions_(nullptr),
      epoch_(0U) {}

Node::Node(NodeConfig& cfg, ReassemblySlot* slots,
           const ReassemblyStorage* storage, std::size_t slot_count,
           std::uint64_t reassembly_timeout_ms, std::uint8_t* rx_buffer,
           std::size_t rx_capacity, std::uint8_t* seal_scratch,
           std::size_t seal_scratch_cap, std::uint8_t* open_buffer,
           std::size_t open_capacity, std::uint8_t* scratch_buffer,
           std::size_t scratch_capacity) noexcept
    : cfg_(cfg),
      seal_scratch_(seal_scratch),
      seal_scratch_cap_(seal_scratch_cap),
      scratch_buffer_(scratch_buffer),
      scratch_capacity_(scratch_capacity),
      endpoint_(),
      link0_(slots, storage, slot_count, reassembly_timeout_ms, cfg.transport,
             rx_buffer, rx_capacity, open_buffer, open_capacity),
      initiator_(),
      last_initiator_event_(InitiatorEvent::None),
      subscription_client_(nullptr),
      last_subscription_event_(SubscriptionEvent::None),
      last_subscription_outcome_(),
      commands_(nullptr),
      on_command_(nullptr),
      on_command_ctx_(nullptr),
      command_client_(nullptr),
      last_command_outcome_(),
      on_terminal_(cfg.has_terminal() ? &Node::terminal_thunk : nullptr),
      on_terminal_ctx_(cfg.has_terminal() ? &cfg : nullptr),
      frames_tx_(0U),
      status_period_ms_(0U),
      status_last_ms_(0U),
      status_started_ms_(0U),
      on_status_topics_(nullptr),
      on_status_topics_ctx_(nullptr),
      learn_catalog_(nullptr),
      on_sample_(nullptr),
      on_sample_ctx_(nullptr),
      on_manifest_(nullptr),
      on_manifest_ctx_(nullptr),
      manifest_skip_on_hello_(false),
      last_manifest_outcome_(),
      manifest_pending_(),
      manifest_pending_next_(0U),
      serve_catalog_(nullptr),
      serve_role_(0U),
      serve_uuid_(),
      serve_name_(nullptr),
      publish_slots_(nullptr),
      publish_slot_capacity_(0U),
      publish_slot_count_(0U) {
    link0_.cfg_ = &cfg_;
    links_[0] = &link0_;
    for (std::size_t i = 1U; i < kMaxNodeLinks; ++i) links_[i] = nullptr;
}

// ---------------------------------------------------------------------------
// Links
// ---------------------------------------------------------------------------

NodeLinkSlot* Node::slot(std::uint8_t link) noexcept {
    return link < kMaxNodeLinks ? links_[link] : nullptr;
}

const NodeLinkSlot* Node::slot(std::uint8_t link) const noexcept {
    return link < kMaxNodeLinks ? links_[link] : nullptr;
}

LinkRef Node::ref_of(std::uint8_t link, const NodeLinkSlot& s) noexcept {
    LinkRef ref{};
    ref.link = link;
    ref.epoch = s.epoch_.load(std::memory_order_acquire);
    return ref;
}

bool Node::attach_link(std::uint8_t link, NodeLink& cfg, NodeLinkSlot& s,
                       std::uint64_t reassembly_timeout_ms,
                       SubscriptionTable* subscriptions) noexcept {
    if (link == 0U || link >= kMaxNodeLinks) return false;
    s.cfg_ = &cfg;
    s.subscriptions_ = subscriptions;
    const bool ok = s.receiver_.configure(cfg.transport, reassembly_timeout_ms);
    links_[link] = &s;
    reset_link(link);
    return ok;
}

bool Node::link_attached(std::uint8_t link) const noexcept {
    return slot(link) != nullptr;
}

NodeRx Node::receive_on(std::uint8_t link, const std::uint8_t* datagram,
                        std::size_t size, std::uint64_t now_ms,
                        ReceivedMessage* out) noexcept {
    NodeLinkSlot* s = slot(link);
    if (s == nullptr) return NodeRx::DroppedFrame;
    return receive_datagram(*s, datagram, size, now_ms, out);
}

NodeRx Node::receive_on(std::uint8_t link, const DecodedFrame& frame,
                        std::uint64_t now_ms, ReceivedMessage* out) noexcept {
    NodeLinkSlot* s = slot(link);
    if (s == nullptr) return NodeRx::DroppedFrame;
    return receive_frame(*s, frame, now_ms, out);
}

void Node::enable_session_on(std::uint8_t link, const Hello& local,
                             std::uint64_t hello_deadline_ms) noexcept {
    NodeLinkSlot* s = slot(link);
    if (s == nullptr) return;
    s->session_ = Session(local, hello_deadline_ms);
    s->session_on_ = true;
}

void Node::arm_session_on(std::uint8_t link, std::uint64_t now_ms) noexcept {
    NodeLinkSlot* s = slot(link);
    if (s != nullptr && s->session_on_) s->session_.arm(now_ms);
}

const Session* Node::link_session(std::uint8_t link) const noexcept {
    const NodeLinkSlot* s = slot(link);
    return s != nullptr && s->session_on_ ? &s->session_ : nullptr;
}

SessionEvent Node::link_session_event(std::uint8_t link) const noexcept {
    const NodeLinkSlot* s = slot(link);
    return s != nullptr ? s->last_session_event_ : SessionEvent::None;
}

void Node::reset_link(std::uint8_t link) noexcept {
    NodeLinkSlot* s = slot(link);
    if (s == nullptr) return;
    s->receiver_.clear();
    if (s->session_on_) s->session_.reset();
    if (s->subscriptions_ != nullptr) s->subscriptions_->clear();
    s->last_receive_outcome_ = ReceiveOutcome::InvalidArgument;
    s->last_session_event_ = SessionEvent::None;
    s->session_path_dropped_crc_ = 0U;
    s->session_path_dropped_decode_ = 0U;
    s->dropped_cleartext_ = 0U;
    s->dropped_open_failed_ = 0U;
    s->epoch_.fetch_add(1U, std::memory_order_acq_rel);
}

LinkRef Node::link_ref(std::uint8_t link) const noexcept {
    const NodeLinkSlot* s = slot(link);
    if (s == nullptr) {
        LinkRef none{};
        none.link = link;
        return none;
    }
    return ref_of(link, *s);
}

bool Node::link_current(const LinkRef& ref) const noexcept {
    const NodeLinkSlot* s = slot(ref.link);
    return s != nullptr && s->epoch_.load(std::memory_order_acquire) == ref.epoch;
}

bool Node::send_on(const LinkRef& to, MessageType type, std::uint16_t object_id,
                   const std::uint8_t* payload, std::size_t size,
                   std::uint64_t timestamp_us, EndpointSealFn seal,
                   void* seal_ctx) noexcept {
    NodeLinkSlot* s = slot(to.link);
    if (s == nullptr || !link_current(to)) return false;
    return transmit(*s, type, object_id, payload, size, timestamp_us, seal,
                    seal_ctx);
}

SubscriptionTable* Node::link_subscriptions(std::uint8_t link) noexcept {
    NodeLinkSlot* s = slot(link);
    return s != nullptr ? s->subscriptions_ : nullptr;
}

const SubscriptionTable* Node::link_subscriptions(std::uint8_t link) const noexcept {
    const NodeLinkSlot* s = slot(link);
    return s != nullptr ? s->subscriptions_ : nullptr;
}

std::uint8_t Node::index_of(const NodeLinkSlot& link) const noexcept {
    for (std::size_t i = 0U; i < kMaxNodeLinks; ++i) {
        if (links_[i] == &link) return static_cast<std::uint8_t>(i);
    }
    return 0U;
}

void Node::deliver_terminal(NodeLinkSlot& link, const Header& header,
                            ByteView payload, std::uint64_t now_ms) noexcept {
    if (on_terminal_ == &Node::terminal_thunk && on_terminal_ctx_ == &cfg_) {
        // The NodeConfig handler: tell it which link, so it can answer there.
        cfg_.terminal_on(*this, ref_of(index_of(link), link), header, payload,
                         now_ms);
        return;
    }
    on_terminal_(on_terminal_ctx_, *this, header, payload, now_ms);
}

// ---------------------------------------------------------------------------
// NodeLink / NodeConfig bridges -- the only place a virtual call becomes a
// C-style function pointer for btp::Endpoint / btp::Receiver. `ctx` is the
// NodeLink a frame leaves through (send / seal), or the NodeConfig this Node
// (or, for command_thunk, a StaticNode<>'s own cfg_ reached via its own
// enable_commands() call) was built with (terminal / command).
// ---------------------------------------------------------------------------

bool Node::send_thunk(void* ctx, const std::uint8_t* frame,
                      std::size_t frame_size) noexcept {
    return static_cast<NodeLink*>(ctx)->send(frame, frame_size);
}

bool Node::seal_thunk(void* ctx, const Header& header, std::uint16_t payload_size,
                      const std::uint8_t* plaintext, std::uint8_t* out) noexcept {
    return static_cast<NodeLink*>(ctx)->seal(header, payload_size, plaintext, out);
}

void Node::terminal_thunk(void* ctx, Node& node, const Header& header,
                          ByteView payload, std::uint64_t now_ms) noexcept {
    static_cast<NodeConfig*>(ctx)->terminal(node, header, payload, now_ms);
}

void Node::command_thunk(void* ctx, std::uint16_t action_id,
                         std::uint16_t action_version, ByteView parameters,
                         NodeActionOutcome* outcome,
                         const NodeCommandTicket& ticket) noexcept {
    static_cast<NodeConfig*>(ctx)->command(action_id, action_version, parameters,
                                           outcome, ticket);
}

EndpointSealFn Node::current_seal(const NodeLinkSlot& link) noexcept {
    return link.cfg_->has_seal() ? &Node::seal_thunk : nullptr;
}

void* Node::current_seal_ctx(const NodeLinkSlot& link) noexcept {
    return link.cfg_->has_seal() ? link.cfg_ : nullptr;
}

bool Node::begin(bool arm_and_announce) noexcept {
    if (!endpoint_.configure(cfg_.source_id, cfg_.boot_id)) return false;
    if (!link0_.receiver_.valid()) return false;
    if (link0_.session_on_ && !link0_.session_.valid()) return false;
    // NodeConfig::send() is always implemented (it is pure virtual) -- a
    // receive-only node's own override just returns false unconditionally,
    // so there is nothing to check here any more.

    if (arm_and_announce) {
        if (link0_.session_on_) arm_session();
        if (serve_catalog_ != nullptr) announce_catalog();
    }
    return true;
}

bool Node::begin(const Hello& local_hello,
                 std::uint64_t connect_deadline_ms) noexcept {
    if (!begin(/*arm_and_announce=*/false)) return false;
    return connect(local_hello, connect_deadline_ms);
}

bool Node::configured() const noexcept {
    return endpoint_.configured() && link0_.receiver_.valid() &&
           (!link0_.session_on_ || link0_.session_.valid());
}

std::uint64_t Node::resolve_now(std::uint64_t fallback) const noexcept {
    return cfg_.has_clock() ? cfg_.clock() : fallback;
}

// ---------------------------------------------------------------------------
// Transmit
// ---------------------------------------------------------------------------

bool Node::transmit(NodeLinkSlot& link, MessageType type, std::uint16_t object_id,
                    const std::uint8_t* payload, std::size_t size,
                    std::uint64_t timestamp_us, EndpointSealFn seal,
                    void* seal_ctx) noexcept {
    SharedLock guard(cfg_);
    const LogicalMessage message{type, object_id, timestamp_us,
                                 {payload, size}};
    const bool ok = endpoint_.send_logical(message, link.cfg_->transport,
                                          &Node::send_thunk, link.cfg_,
                                          seal_scratch_, seal_scratch_cap_, seal,
                                          seal_ctx);
    if (ok) ++frames_tx_;
    return ok;
}

bool Node::transmit_reserved(NodeLinkSlot& link, std::uint32_t sequence,
                             MessageType type, std::uint16_t object_id,
                             const std::uint8_t* payload, std::size_t size,
                             std::uint64_t timestamp_us) noexcept {
    SharedLock guard(cfg_);
    const LogicalMessage message{type, object_id, timestamp_us, {payload, size}};
    return endpoint_.send_logical_reserved(sequence, message, link.cfg_->transport,
                                          &Node::send_thunk, link.cfg_,
                                          seal_scratch_, seal_scratch_cap_,
                                          current_seal(link), current_seal_ctx(link));
}

bool Node::send_with(MessageType type, std::uint16_t object_id,
                     const std::uint8_t* payload, std::size_t size,
                     std::uint64_t timestamp_us, EndpointSealFn seal,
                     void* seal_ctx) noexcept {
    return transmit(link0_, type, object_id, payload, size, timestamp_us, seal,
                    seal_ctx);
}

bool Node::send(MessageType type, std::uint16_t object_id,
                const std::uint8_t* payload, std::size_t size,
                std::uint64_t timestamp_us) noexcept {
    return send_with(type, object_id, payload, size, timestamp_us,
                     current_seal(link0_), current_seal_ctx(link0_));
}

void Node::reply_seal_for(NodeLinkSlot& link, const Header& request,
                          EndpointSealFn* out_seal,
                          void** out_seal_ctx) const noexcept {
    *out_seal = nullptr;
    *out_seal_ctx = nullptr;
    link.cfg_->reply_seal(request, out_seal, out_seal_ctx);
    if (*out_seal == nullptr) {
        *out_seal = current_seal(link);
        *out_seal_ctx = current_seal_ctx(link);
    }
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
    return receive_datagram(link0_, datagram, size, now_ms, out);
}

NodeRx Node::receive(const DecodedFrame& frame, ReceivedMessage* out) noexcept {
    return receive(frame, resolve_now(0U), out);
}

NodeRx Node::receive(const DecodedFrame& frame, std::uint64_t now_ms,
                     ReceivedMessage* out) noexcept {
    return receive_frame(link0_, frame, now_ms, out);
}

NodeRx Node::receive_datagram(NodeLinkSlot& link, const std::uint8_t* datagram,
                              std::size_t size, std::uint64_t now_ms,
                              ReceivedMessage* out) noexcept {
    link.last_session_event_ = SessionEvent::None;
    if (&link == &link0_) last_initiator_event_ = InitiatorEvent::None;
    if (out == nullptr || datagram == nullptr || size == 0U) {
        link.last_receive_outcome_ = ReceiveOutcome::InvalidArgument;
        return NodeRx::DroppedFrame;
    }

    // The initiator (connect()) only ever runs on link 0.
    const bool initiator_live =
        &link == &link0_ && initiator_.state() != InitiatorState::Idle;
    if (!link.session_on_ && !initiator_live) {
        return finish(link,
                      link.receiver_.submit(datagram, size, now_ms, link.rx_buffer_,
                                            link.rx_capacity_, out),
                      out, now_ms);
    }

    // A session (either direction) needs the DecodedFrame btp::Receiver keeps
    // to itself, so the decode happens here; its failures are counted apart
    // (stats()).
    DecodedFrame decoded{};
    const Error error = btp::decode(datagram, size, link.cfg_->transport, &decoded);
    if (error != Error::Ok) {
        if (error == Error::CrcMismatch) {
            ++link.session_path_dropped_crc_;
            link.last_receive_outcome_ = ReceiveOutcome::DroppedCrc;
        } else {
            ++link.session_path_dropped_decode_;
            link.last_receive_outcome_ = ReceiveOutcome::DroppedDecode;
        }
        return NodeRx::DroppedFrame;
    }

    return route_decoded(link, decoded, now_ms, out);
}

NodeRx Node::receive_frame(NodeLinkSlot& link, const DecodedFrame& frame,
                           std::uint64_t now_ms, ReceivedMessage* out) noexcept {
    link.last_session_event_ = SessionEvent::None;
    if (&link == &link0_) last_initiator_event_ = InitiatorEvent::None;
    if (out == nullptr) {
        link.last_receive_outcome_ = ReceiveOutcome::InvalidArgument;
        return NodeRx::DroppedFrame;
    }
    return route_decoded(link, frame, now_ms, out);
}

NodeRx Node::route_decoded(NodeLinkSlot& link, const DecodedFrame& decoded,
                           std::uint64_t now_ms, ReceivedMessage* out) noexcept {
    const bool initiator_live =
        &link == &link0_ && initiator_.state() != InitiatorState::Idle;

    if (initiator_live) {
        const InitiatorOutcome io = initiator_.on_frame(decoded, now_ms);
        last_initiator_event_ = io.event;
        switch (io.event) {
            case InitiatorEvent::FrameAccepted:
                break;  // a live-connection application frame -- route it below
            case InitiatorEvent::None:
                break;  // not the awaited HELLO_RESULT -- fall through (a
                        // responder session on the same node, or normal routing)
            case InitiatorEvent::Connected:
            case InitiatorEvent::Rejected:
            case InitiatorEvent::TimedOut:
            case InitiatorEvent::Disconnected:
                return NodeRx::InitiatorHandled;
        }
    }

    if (!link.session_on_) {
        return finish(link,
                      link.receiver_.submit(decoded, now_ms, link.rx_buffer_,
                                            link.rx_capacity_, out),
                      out, now_ms);
    }

    std::uint8_t reply[kSessionMaxReplySize];
    const SessionOutcome outcome =
        link.session_.on_frame(decoded, now_ms, reply, sizeof(reply));
    link.last_session_event_ = outcome.event;
    if (outcome.reply_size != 0U) {
        // btp::Session produces the reply PAYLOAD (HELLO_RESULT /
        // SESSION_CLOSE_RESULT); the node frames it and puts it on the wire,
        // cleartext -- the handshake bootstraps the session before any key.
        // Not counted in frames_tx_ (bootstrap traffic, see enable_status()).
        const std::uint16_t reply_object =
            outcome.event == SessionEvent::SessionClosed
                ? object_id::kSessionCloseResult
                : object_id::kHelloResult;
        const LogicalMessage reply_msg{
            MessageType::Control, reply_object, resolve_now(0U) * 1000ULL,
            {reply, outcome.reply_size}};
        SharedLock guard(cfg_);
        endpoint_.send_logical(reply_msg, link.cfg_->transport, &Node::send_thunk,
                               link.cfg_, seal_scratch_, seal_scratch_cap_, nullptr,
                               nullptr);
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

    return finish(link,
                  link.receiver_.submit(decoded, now_ms, link.rx_buffer_,
                                        link.rx_capacity_, out),
                  out, now_ms);
}

NodeRx Node::finish(NodeLinkSlot& link, ReceiveOutcome outcome,
                    ReceivedMessage* out, std::uint64_t now_ms) noexcept {
    link.last_receive_outcome_ = outcome;
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

    // Complete. With a key (has_open()), open the sealed payload in place --
    // and refuse a cleartext one unless accept_cleartext() lets it through:
    // otherwise a peer could skip the key just by leaving ENCRYPTED clear.
    NodeLink& cfg = *link.cfg_;
    if (cfg.has_open()) {
        if ((out->header.flags & kFlagEncrypted) != 0U) {
            if (out->payload.size < kEndpointAeadTagSize ||
                link.open_buffer_ == nullptr ||
                out->payload.size - kEndpointAeadTagSize > link.open_capacity_ ||
                !cfg.open(out->header,
                          static_cast<std::uint16_t>(out->payload.size),
                          out->payload.data, link.open_buffer_)) {
                ++link.dropped_open_failed_;
                return NodeRx::DroppedFrame;
            }
            out->payload = ByteView{link.open_buffer_,
                                    out->payload.size - kEndpointAeadTagSize};
        } else if (!cfg.accept_cleartext(out->header)) {
            ++link.dropped_cleartext_;
            return NodeRx::DroppedFrame;
        }
    }

    // ----- discovery the node manages itself -----
    if (serve_catalog_ != nullptr && out->header.type == MessageType::Control &&
        out->header.object_id == object_id::kManifestRequest) {
        serve_manifest(link, out->header, out->payload);
        return NodeRx::RequestServed;
    }
    if ((learn_catalog_ != nullptr || on_manifest_ != nullptr) &&
        out->header.type == MessageType::Control &&
        out->header.object_id == object_id::kManifestData) {
        return consume_manifest(link, *out);
    }
    if (learn_catalog_ != nullptr && on_sample_ != nullptr &&
        out->header.type == MessageType::Telemetry) {
        const CatalogTopic* topic = learn_catalog_->topic(out->header.object_id);
        if (topic == nullptr) {
            return NodeRx::Ignored;  // no schema for this topic yet
        }
        SampleReader reader(out->payload.data, out->payload.size, topic->fields,
                            topic->field_count, topic->encoding);
        on_sample_(on_sample_ctx_, *topic, reader);
        return NodeRx::SampleDelivered;
    }

    // ----- subscriptions the node manages itself -----
    if (link.subscriptions_ != nullptr && out->header.type == MessageType::Control &&
        out->header.object_id == object_id::kSubscribe) {
        serve_subscribe(link, out->header, out->payload, now_ms);
        return NodeRx::SubscriptionServed;
    }
    if (link.subscriptions_ != nullptr && out->header.type == MessageType::Control &&
        out->header.object_id == object_id::kUnsubscribe) {
        serve_unsubscribe(link, out->header, out->payload);
        return NodeRx::SubscriptionServed;
    }
    if (subscription_client_ != nullptr && out->header.type == MessageType::Control &&
        out->header.object_id == object_id::kSubscribeResult) {
        SubscribeResult result = {};
        if (decode_subscribe_result(out->payload.data, out->payload.size, &result) ==
            MessageError::Ok) {
            last_subscription_outcome_ = subscription_client_->on_result(result, now_ms);
            last_subscription_event_ = last_subscription_outcome_.event;
        }
        return NodeRx::SubscriptionHandled;
    }

    // ----- commands -----
    if (commands_ != nullptr && on_command_ != nullptr &&
        out->header.type == MessageType::Command &&
        out->header.object_id == object_id::kCommandRequest) {
        serve_command(link, out->header, out->payload);
        return NodeRx::CommandServed;
    }
    if (command_client_ != nullptr && out->header.type == MessageType::Command &&
        out->header.object_id == object_id::kCommandResult) {
        CommandResult result = {};
        if (decode_command_result(out->payload.data, out->payload.size, &result) ==
            MessageError::Ok) {
            last_command_outcome_ = command_client_->on_result(result);
        }
        return NodeRx::CommandHandled;
    }

    // ----- terminal -----
    if (on_terminal_ != nullptr && out->header.type == MessageType::Terminal) {
        deliver_terminal(link, out->header, out->payload, now_ms);
        return NodeRx::TerminalDelivered;
    }

    return NodeRx::Complete;
}

// ---------------------------------------------------------------------------
// Subscriptions
// ---------------------------------------------------------------------------

void Node::serve_subscribe(NodeLinkSlot& link, const Header& request,
                           ByteView payload, std::uint64_t now_ms) noexcept {
    if (link.subscriptions_ == nullptr || serve_catalog_ == nullptr) return;
    Subscribe req = {};
    if (decode_subscribe(payload.data, payload.size, &req) != MessageError::Ok) {
        return;
    }
    // SUBSCRIBE has no broadcast form (target_source_id/boot_id are spec-
    // required non-zero) -- a mismatch means this frame is not addressed to
    // this node at all.
    if (req.target_source_id != cfg_.source_id ||
        req.target_boot_id != cfg_.boot_id) {
        return;
    }

    SubscribeResult result = {};
    link.subscriptions_->handle_subscribe(*serve_catalog_, request, req, now_ms,
                                         &result);

    std::uint8_t buffer[40];
    std::size_t written = 0U;
    if (encode_subscribe_result(result, buffer, sizeof(buffer), &written) !=
        MessageError::Ok) {
        return;
    }
    EndpointSealFn seal = nullptr;
    void* seal_ctx = nullptr;
    reply_seal_for(link, request, &seal, &seal_ctx);
    transmit(link, MessageType::Control, object_id::kSubscribeResult, buffer,
             written, resolve_now(0U) * 1000ULL, seal, seal_ctx);
}

void Node::serve_unsubscribe(NodeLinkSlot& link, const Header& request,
                             ByteView payload) noexcept {
    if (link.subscriptions_ == nullptr) return;
    Unsubscribe req = {};
    if (decode_unsubscribe(payload.data, payload.size, &req) != MessageError::Ok) {
        return;
    }
    if (req.target_source_id != cfg_.source_id ||
        req.target_boot_id != cfg_.boot_id) {
        return;
    }

    ControlResult result = {};
    link.subscriptions_->handle_unsubscribe(request, req, &result);

    std::uint8_t buffer[24];
    std::size_t written = 0U;
    if (encode_unsubscribe_result(result, buffer, sizeof(buffer), &written) !=
        MessageError::Ok) {
        return;
    }
    EndpointSealFn seal = nullptr;
    void* seal_ctx = nullptr;
    reply_seal_for(link, request, &seal, &seal_ctx);
    transmit(link, MessageType::Control, object_id::kUnsubscribeResult, buffer,
             written, resolve_now(0U) * 1000ULL, seal, seal_ctx);
}

std::uint32_t Node::subscribe(std::uint32_t peer_source_id, std::uint32_t peer_boot_id,
                              std::uint16_t topic_id, std::uint32_t rate_millihz,
                              std::uint32_t lease_ms) noexcept {
    if (subscription_client_ == nullptr) return 0U;
    std::uint32_t sequence = 0U;
    if (!endpoint_.reserve_sequence(&sequence)) return 0U;

    const std::uint64_t now_ms = resolve_now(0U);
    std::uint8_t buffer[24];
    std::size_t written = 0U;
    const std::uint32_t local_id = subscription_client_->subscribe(
        peer_source_id, peer_boot_id, topic_id, rate_millihz, lease_ms,
        cfg_.source_id, cfg_.boot_id, sequence, now_ms, buffer, sizeof(buffer),
        &written);
    if (local_id == 0U) return 0U;

    // A send failure here is rare (the frame fit at subscribe() time) and not
    // fatal: the slot stays Pending and simply times out via expire(), the
    // same fail-safe as any other lost frame.
    transmit_reserved(link0_, sequence, MessageType::Control, object_id::kSubscribe,
                      buffer, written, now_ms * 1000ULL);
    return local_id;
}

bool Node::unsubscribe(std::uint32_t local_id) noexcept {
    if (subscription_client_ == nullptr) return false;
    std::uint32_t sequence = 0U;
    if (!endpoint_.reserve_sequence(&sequence)) return false;

    std::uint8_t buffer[16];
    std::size_t written = 0U;
    if (!subscription_client_->unsubscribe(local_id, cfg_.source_id, cfg_.boot_id,
                                           sequence, buffer, sizeof(buffer),
                                           &written)) {
        return false;
    }
    return transmit_reserved(link0_, sequence, MessageType::Control,
                             object_id::kUnsubscribe, buffer, written,
                             resolve_now(0U) * 1000ULL);
}

void Node::drain_subscription_renewals(std::uint64_t now_ms) noexcept {
    if (subscription_client_ == nullptr) return;
    for (;;) {
        const std::uint32_t local_id = subscription_client_->next_renewal_due(now_ms);
        if (local_id == 0U) break;

        std::uint32_t sequence = 0U;
        if (!endpoint_.reserve_sequence(&sequence)) break;
        std::uint8_t buffer[24];
        std::size_t written = 0U;
        if (!subscription_client_->renew(local_id, cfg_.source_id, cfg_.boot_id,
                                         sequence, now_ms, buffer, sizeof(buffer),
                                         &written)) {
            break;  // should not happen -- next_renewal_due() just found it Active
        }
        transmit_reserved(link0_, sequence, MessageType::Control,
                          object_id::kSubscribe, buffer, written, now_ms * 1000ULL);
    }
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void Node::serve_command(NodeLinkSlot& link, const Header& request,
                         ByteView payload) noexcept {
    SharedLock guard(cfg_);  // commands_ and scratch_buffer_ are shared
    if (commands_ == nullptr || on_command_ == nullptr ||
        scratch_buffer_ == nullptr) {
        return;
    }
    CommandRequest req = {};
    if (decode_command_request(payload.data, payload.size, &req) != MessageError::Ok) {
        return;
    }
    // COMMAND_REQUEST has no broadcast form (target_source_id/boot_id are
    // spec-required non-zero) -- a mismatch means this is not for this node.
    if (req.target_source_id != cfg_.source_id ||
        req.target_boot_id != cfg_.boot_id) {
        return;
    }

    const DedupKey key{request.source_id, request.boot_id, request.sequence};
    std::size_t slot = 0U;
    ByteView stored{};
    const DedupVerdict verdict =
        commands_->classify(key, payload.data, payload.size, &slot, &stored);

    switch (verdict) {
        case DedupVerdict::Fresh: {
            NodeActionOutcome outcome = {};
            outcome.status = static_cast<std::uint8_t>(ResultStatus::Success);
            NodeCommandTicket ticket{};
            ticket.request = request;
            ticket.action_id = req.action_id;
            ticket.action_version = req.action_version;
            ticket.slot = slot;
            ticket.armed = true;
            ticket.link = ref_of(index_of(link), link);
            on_command_(on_command_ctx_, req.action_id, req.action_version,
                       req.parameters, &outcome, ticket);
            // pending: the handler kept the ticket for later -- nothing to
            // send now, the slot stays Reserved (a retransmission in the
            // meantime classifies DuplicateInFlight and is dropped).
            if (!outcome.pending) {
                emit_command_result(link, request, req.action_id,
                                    req.action_version, outcome, slot);
            }
            break;
        }
        case DedupVerdict::DuplicateComplete: {
            // A retransmission of an identity already executed: replay the
            // exact stored result, do not run the action again.
            EndpointSealFn seal = nullptr;
            void* seal_ctx = nullptr;
            reply_seal_for(link, request, &seal, &seal_ctx);
            transmit(link, MessageType::Command, object_id::kCommandResult,
                     stored.data, stored.size, resolve_now(0U) * 1000ULL, seal,
                     seal_ctx);
            break;
        }
        case DedupVerdict::DuplicateInFlight:
            break;  // still executing -- drop, the peer will retry
        case DedupVerdict::Conflict:
            emit_command_reject(link, request, req.action_id, req.action_version,
                                ResultStatus::Rejected, ResultError::RequestConflict);
            break;
        case DedupVerdict::Evicted:
        case DedupVerdict::CapacityExhausted:
            emit_command_reject(link, request, req.action_id, req.action_version,
                                ResultStatus::Busy, ResultError::CapacityExhausted);
            break;
        case DedupVerdict::InvalidArgument:
            break;  // a null pointer or an oversized request -- drop
    }
}

bool Node::emit_command_result(NodeLinkSlot& link, const Header& request,
                               std::uint16_t action_id,
                               std::uint16_t action_version,
                               const NodeActionOutcome& outcome,
                               std::size_t slot, bool deliver) noexcept {
    SharedLock guard(cfg_);
    CommandResult result = {};
    result.request.request_source_id = request.source_id;
    result.request.request_boot_id = request.boot_id;
    result.request.reply_to_sequence = request.sequence;
    result.action_id = action_id;
    result.action_version = action_version;
    result.status = outcome.status;
    result.error_code = outcome.error_code;
    if (outcome.message != nullptr) {
        result.message =
            ByteView{reinterpret_cast<const std::uint8_t*>(outcome.message),
                    std::strlen(outcome.message)};
    }
    result.result = ByteView{outcome.result_data, outcome.result_size};

    std::size_t written = 0U;
    if (encode_command_result(result, scratch_buffer_, scratch_capacity_, &written) !=
        MessageError::Ok) {
        return false;
    }
    // Remember the result BEFORE sending -- the action ran exactly once
    // whether or not this send succeeds; a later retransmission of the same
    // identity must still find it and replay rather than running again.
    // Also this call's own guard against a stale/repeated ticket
    // (complete_command()): a slot that is not Reserved any more --
    // already Complete from an earlier call, or long evicted -- refuses
    // here and nothing is sent.
    if (commands_->record_result(slot, scratch_buffer_, written) != MessageError::Ok) {
        return false;
    }
    if (!deliver) return false;
    EndpointSealFn seal = nullptr;
    void* seal_ctx = nullptr;
    reply_seal_for(link, request, &seal, &seal_ctx);
    transmit(link, MessageType::Command, object_id::kCommandResult, scratch_buffer_,
             written, resolve_now(0U) * 1000ULL, seal, seal_ctx);
    return true;
}

bool Node::complete_command(const NodeCommandTicket& ticket,
                            const NodeActionOutcome& outcome) noexcept {
    if (!ticket.valid() || commands_ == nullptr) return false;
    NodeLinkSlot* link = slot(ticket.link.link);
    if (link == nullptr) return false;
    // A reset link still records the result -- a retransmission reaching this
    // node on another link must replay it, not run the action twice -- but
    // the reply is not sent to whoever took that link over.
    return emit_command_result(*link, ticket.request, ticket.action_id,
                               ticket.action_version, outcome, ticket.slot,
                               link_current(ticket.link));
}

void Node::emit_command_reject(NodeLinkSlot& link, const Header& request,
                               std::uint16_t action_id,
                               std::uint16_t action_version, ResultStatus status,
                               ResultError error) noexcept {
    SharedLock guard(cfg_);
    CommandResult result = {};
    result.request.request_source_id = request.source_id;
    result.request.request_boot_id = request.boot_id;
    result.request.reply_to_sequence = request.sequence;
    result.action_id = action_id;
    result.action_version = action_version;
    result.status = static_cast<std::uint8_t>(status);
    result.error_code = static_cast<std::uint16_t>(error);

    std::size_t written = 0U;
    if (encode_command_result(result, scratch_buffer_, scratch_capacity_, &written) !=
        MessageError::Ok) {
        return;
    }
    EndpointSealFn seal = nullptr;
    void* seal_ctx = nullptr;
    reply_seal_for(link, request, &seal, &seal_ctx);
    transmit(link, MessageType::Command, object_id::kCommandResult, scratch_buffer_,
             written, resolve_now(0U) * 1000ULL, seal, seal_ctx);
}

std::uint32_t Node::command(std::uint32_t peer_source_id, std::uint32_t peer_boot_id,
                            std::uint16_t action_id, std::uint16_t action_version,
                            const std::uint8_t* parameters,
                            std::size_t parameters_size) noexcept {
    if (command_client_ == nullptr || scratch_buffer_ == nullptr) {
        return 0U;
    }
    SharedLock guard(cfg_);
    std::uint32_t sequence = 0U;
    if (!endpoint_.reserve_sequence(&sequence)) return 0U;

    const std::uint64_t now_ms = resolve_now(0U);
    std::size_t written = 0U;
    const std::uint32_t local_id = command_client_->command(
        peer_source_id, peer_boot_id, action_id, action_version, parameters,
        parameters_size, cfg_.source_id, cfg_.boot_id, sequence, now_ms,
        scratch_buffer_, scratch_capacity_, &written);
    if (local_id == 0U) return 0U;

    // A send failure here is rare and not fatal: the slot stays Pending and
    // simply times out via expire(), the same fail-safe as any lost frame.
    transmit_reserved(link0_, sequence, MessageType::Command,
                      object_id::kCommandRequest, scratch_buffer_, written,
                      now_ms * 1000ULL);
    return local_id;
}

// ---------------------------------------------------------------------------
// STATUS
// ---------------------------------------------------------------------------

void Node::enable_status(std::uint32_t period_ms) noexcept {
    status_period_ms_ = period_ms;
    status_started_ms_ = resolve_now(0U);
    status_last_ms_ = status_started_ms_;
}

void Node::emit_status(std::uint64_t now_ms) noexcept {
    if (scratch_buffer_ == nullptr) return;
    SharedLock guard(cfg_);

    const Receiver::Stats rx = link0_.receiver_.stats();
    StatusV1 status = {};
    status.uptime_us = now_ms >= status_started_ms_
                           ? (now_ms - status_started_ms_) * 1000ULL
                           : 0ULL;
    status.frames_rx = rx.completed;
    status.frames_tx = frames_tx_;
    status.crc_errors = static_cast<std::uint64_t>(rx.dropped_crc) +
                        link0_.session_path_dropped_crc_;
    status.decode_errors = static_cast<std::uint64_t>(rx.dropped_decode) +
                           link0_.session_path_dropped_decode_;
    status.reassembly_completed = rx.completed;
    status.reassembly_timeouts = rx.reassembly_timeouts;
    status.reassembly_rejected = rx.dropped_reassembly;
    status.frames_dropped = status.crc_errors + status.decode_errors +
                            status.reassembly_rejected +
                            link0_.dropped_cleartext_ + link0_.dropped_open_failed_;
    status.command_duplicates =
        commands_ != nullptr ? commands_->stats().replayed : 0U;
    status.telemetry_dropped = 0U;  // not tracked separately yet

    // v2 per-topic block (2.41.0): only when a callback is attached AND both
    // the served catalog and the subscription table are up -- any one of the
    // three missing falls back to plain v1 below, same as nothing being
    // subscribed right now does.
    TopicStatusRecord topics[kMaxStatusTopics];
    std::size_t topic_count = 0U;
    const SubscriptionTable* subscriptions = link0_.subscriptions_;
    if (on_status_topics_ != nullptr && serve_catalog_ != nullptr &&
        subscriptions != nullptr) {
        const std::size_t catalog_topics = serve_catalog_->topic_count();
        for (std::size_t i = 0U;
             i < catalog_topics && topic_count < kMaxStatusTopics; ++i) {
            const CatalogTopic* topic = serve_catalog_->topic_at(i);
            if (topic == nullptr) continue;
            const std::size_t subs = subscriptions->subscriber_count(topic->topic_id);
            if (subs == 0U) continue;

            TopicStatusRecord& record = topics[topic_count];
            record.source_id = cfg_.source_id;
            record.topic_id = topic->topic_id;
            record.subscriber_count = static_cast<std::uint16_t>(subs);
            record.effective_rate_millihz =
                subscriptions->aggregate_rate_millihz(topic->topic_id);
            record.bytes_total = 0U;
            record.samples_dropped_total = 0U;
            on_status_topics_(on_status_topics_ctx_, *this, topic->topic_id,
                              &record.bytes_total, &record.samples_dropped_total);
            ++topic_count;
        }
    }

    std::size_t written = 0U;
    const MessageError err =
        topic_count > 0U
            ? encode_status_v2(status, topics, topic_count, scratch_buffer_,
                               scratch_capacity_, &written)
            : encode_status_v1(status, scratch_buffer_, scratch_capacity_, &written);
    if (err != MessageError::Ok) return;
    send(MessageType::Control, object_id::kStatus, scratch_buffer_, written,
        now_ms * 1000ULL);
}

// ---------------------------------------------------------------------------
// Discovery: consumer side
// ---------------------------------------------------------------------------

void Node::learn_catalog(Catalog* catalog) noexcept { learn_catalog_ = catalog; }

void Node::on_sample(NodeSampleFn callback, void* ctx) noexcept {
    on_sample_ = callback;
    on_sample_ctx_ = ctx;
}

bool Node::request_manifest(std::uint32_t target_source_id,
                            std::uint32_t target_boot_id,
                            std::uint32_t known_config_revision) noexcept {
    return send_manifest_request(target_source_id, target_boot_id,
                                 known_config_revision);
}

bool Node::send_manifest_request(std::uint32_t target_source_id,
                                 std::uint32_t target_boot_id,
                                 std::uint32_t known_config_revision) noexcept {
    ManifestRequest request = {};
    request.target_source_id = target_source_id;
    request.target_boot_id = target_boot_id;
    request.known_config_revision = known_config_revision;

    std::uint8_t buffer[16];
    std::size_t written = 0U;
    if (encode_manifest_request(request, buffer, sizeof(buffer), &written) !=
        MessageError::Ok) {
        return false;
    }

    // Reserved first, so the answer's RequestRef (reply_to_sequence) can be
    // matched back to this target -- see take_manifest_pending().
    SharedLock guard(cfg_);
    std::uint32_t sequence = 0U;
    if (!endpoint_.reserve_sequence(&sequence)) return false;
    ManifestPending& slot = manifest_pending_[manifest_pending_next_];
    manifest_pending_next_ = (manifest_pending_next_ + 1U) % kNodeManifestPendingSlots;
    slot.sequence = sequence;
    slot.target_source_id = target_source_id;
    slot.used = true;
    const bool sealed = link0_.cfg_->seal_manifest_request(target_source_id);
    const LogicalMessage message{MessageType::Control, object_id::kManifestRequest,
                                 resolve_now(0U) * 1000ULL, {buffer, written}};
    const bool ok = endpoint_.send_logical_reserved(
        sequence, message, link0_.cfg_->transport, &Node::send_thunk, link0_.cfg_,
        seal_scratch_, seal_scratch_cap_, sealed ? current_seal(link0_) : nullptr,
        sealed ? current_seal_ctx(link0_) : nullptr);
    if (ok) ++frames_tx_;
    return ok;
}

bool Node::take_manifest_pending(const RequestRef& ref,
                                 std::uint32_t* target_source_id) noexcept {
    if (ref.request_source_id != cfg_.source_id || ref.request_boot_id != cfg_.boot_id) {
        return false;  // unsolicited (all zero), or answering somebody else
    }
    for (std::size_t i = 0U; i < kNodeManifestPendingSlots; ++i) {
        ManifestPending& slot = manifest_pending_[i];
        if (slot.used && slot.sequence == ref.reply_to_sequence) {
            *target_source_id = slot.target_source_id;
            slot.used = false;
            return true;
        }
    }
    return false;
}

bool Node::load_cached_manifest(std::uint32_t source_id, ByteView* payload,
                                ManifestHeader* header) noexcept {
    if (source_id == 0U || !cfg_.has_manifest_cache()) return false;
    ByteView stored{};
    if (!cfg_.manifest_load(source_id, &stored)) return false;

    // Trust nothing that came back from storage: it must be a complete
    // SUCCESS describing this very source, framed exactly (raw_records()
    // requires the payload consumed to the last octet).
    bool ok = stored.data != nullptr && stored.size != 0U;
    ManifestHeader h = {};
    if (ok) {
        ManifestReader reader(stored.data, stored.size);
        ByteView info{};
        ByteView topics{};
        ByteView actions{};
        ok = reader.header(&h) == MessageError::Ok &&
             h.status == static_cast<std::uint8_t>(ResultStatus::Success) &&
             (h.flags & kManifestNotModified) == 0U &&
             h.described_source_id == source_id &&
             reader.raw_source_info(&info) == MessageError::Ok &&
             reader.raw_records(&topics, &actions) == MessageError::Ok;
    }
    if (!ok) {
        cfg_.manifest_evict(source_id);
        return false;
    }
    *payload = stored;
    *header = h;
    return true;
}

bool Node::request_manifest_cached(std::uint32_t target_source_id,
                                   std::uint32_t target_boot_id) noexcept {
    ByteView cached{};
    ManifestHeader cached_header = {};
    const bool have = target_source_id != 0U &&
                      load_cached_manifest(target_source_id, &cached, &cached_header);

    // Skip-on-hello (opt-in): the peer we are connected to already told us
    // its revision in HELLO_RESULT, and the cache holds exactly that one --
    // answer from the cache without a round trip. Only the boot changes
    // between sessions without the revision changing, and HELLO_RESULT
    // carried that too.
    if (have && manifest_skip_on_hello_ && initiator_.connected() &&
        target_source_id == initiator_.peer_source_id() &&
        cached_header.config_revision == initiator_.peer_config_revision() &&
        std::memcmp(cached_header.source_uuid, initiator_.peer_uuid(), 16U) == 0) {
        NodeManifest manifest = {};
        manifest.header = cached_header;
        manifest.header.described_boot_id = initiator_.peer_boot_id();
        manifest.payload = cached;
        manifest.topics_payload = cached;
        manifest.from_cache = true;
        manifest.source_info_stale = true;
        manifest.link = ref_of(0U, link0_);
        deliver_manifest(manifest);
        return true;
    }

    return send_manifest_request(target_source_id, target_boot_id,
                                 have ? cached_header.config_revision : 0U);
}

void Node::deliver_manifest(const NodeManifest& manifest) noexcept {
    if (learn_catalog_ != nullptr) {
        // topics_payload is complete (a full answer, or the cached one) or a
        // NOT_MODIFIED with no cache behind it, which ingest() already reads
        // as "keep what I have".
        learn_catalog_->ingest(manifest.topics_payload.data, manifest.topics_payload.size);
    }
    if (on_manifest_ != nullptr) {
        on_manifest_(on_manifest_ctx_, *this, manifest);
    }
}

NodeRx Node::consume_manifest(NodeLinkSlot& link, const ReceivedMessage& msg) noexcept {
    ManifestReader reader(msg.payload.data, msg.payload.size);
    ManifestHeader header = {};
    if (reader.header(&header) != MessageError::Ok) {
        return NodeRx::DroppedFrame;
    }

    std::uint32_t target = 0U;
    const bool correlated = take_manifest_pending(header.request, &target);

    if (header.status != static_cast<std::uint8_t>(ResultStatus::Success)) {
        // Describes no source (commands.md 3.2) -- nothing to learn, and in
        // particular nothing to wipe a learn catalogue with.
        last_manifest_outcome_.target_source_id = target;
        last_manifest_outcome_.status = header.status;
        last_manifest_outcome_.error_code = header.error_code;
        last_manifest_outcome_.correlated = correlated;
        return NodeRx::ManifestRejected;
    }

    const std::uint32_t source = header.described_source_id;
    const bool not_modified = (header.flags & kManifestNotModified) != 0U;
    ByteView topics = msg.payload;
    bool from_cache = false;

    if (cfg_.has_manifest_cache() && source != 0U) {
        if (not_modified) {
            ByteView cached{};
            ManifestHeader cached_header = {};
            const bool have = load_cached_manifest(source, &cached, &cached_header);
            const bool usable =
                have && cached_header.config_revision == header.config_revision &&
                std::memcmp(cached_header.source_uuid, header.source_uuid, 16U) == 0;
            if (!usable) {
                // The peer says "you already have revision N", but what is
                // stored is not N (it changed underneath, or was never ours),
                // or it belongs to another device with the same source_id.
                // Drop it and fetch the real thing.
                if (have) cfg_.manifest_evict(source);
                send_manifest_request(source, 0U, 0U);
                return NodeRx::Ignored;
            }
            topics = cached;
            from_cache = true;
        } else {
            cfg_.manifest_store(source, header, msg.payload);
        }
    }

    NodeManifest manifest = {};
    manifest.header = header;
    manifest.payload = msg.payload;
    manifest.topics_payload = topics;
    manifest.from_cache = from_cache;
    manifest.source_info_stale = false;
    manifest.link = ref_of(index_of(link), link);

    if (on_manifest_ == nullptr) {
        // learn_catalog() alone: same NodeRx it always returned.
        return learn_catalog_->ingest(topics.data, topics.size) == MessageError::Ok
                   ? NodeRx::CatalogUpdated
                   : NodeRx::DroppedFrame;
    }
    deliver_manifest(manifest);
    return NodeRx::ManifestHandled;
}

// ---------------------------------------------------------------------------
// Discovery: producer side
// ---------------------------------------------------------------------------

void Node::serve_catalog(Catalog* catalog, std::uint8_t role,
                         const std::uint8_t* source_uuid,
                         const char* source_name) noexcept {
    serve_catalog_ = catalog;
    serve_role_ = role;
    serve_name_ = source_name;
    if (source_uuid != nullptr) {
        std::memcpy(serve_uuid_, source_uuid, sizeof(serve_uuid_));
    } else {
        std::memset(serve_uuid_, 0, sizeof(serve_uuid_));
    }
}

void Node::emit_manifest(NodeLinkSlot& link, const Header& request,
                         const RequestRef& reply_to, std::uint8_t status,
                         std::uint8_t flags, std::uint16_t error_code,
                         bool with_topics) noexcept {
    if (serve_catalog_ == nullptr || scratch_buffer_ == nullptr) {
        return;
    }
    SharedLock guard(cfg_);

    // Format 2 whenever there is a source_info block to carry on a SUCCESS
    // reply -- a full response AND a NOT_MODIFIED one (commands.md 3.3:
    // source_info is not covered by config_revision, so it still follows
    // source_name in a NOT_MODIFIED response). Format 3 the same way, when
    // any field carries a min_value/max_value (it implies format 2's
    // source_info block too -- the formats are cumulative). A REJECTED reply
    // (STALE_TARGET_BOOT / NOT_FOUND) describes no source and stays format 1.
    const bool success = status == static_cast<std::uint8_t>(ResultStatus::Success);
    const bool with_source_info = serve_catalog_->has_source_info() && success;
    const bool with_field_ranges = serve_catalog_->has_field_ranges() && success;

    ManifestHeader header = {};
    header.request = reply_to;
    header.status = status;
    header.flags = flags;
    header.error_code = error_code;
    header.manifest_format_version =
        with_field_ranges ? 3U : (with_source_info ? 2U : 1U);
    header.config_revision = serve_catalog_->config_revision();
    std::memcpy(header.source_uuid, serve_uuid_, sizeof(header.source_uuid));
    header.described_source_id = cfg_.source_id;
    header.described_boot_id = cfg_.boot_id;
    header.source_role = serve_role_ != 0U
                             ? serve_role_
                             : static_cast<std::uint8_t>(Role::Producer);
    header.source_flags = kSourceOnline;
    header.catalog_count = 1U;
    header.topic_count =
        with_topics
            ? static_cast<std::uint16_t>(serve_catalog_->topic_count())
            : 0U;
    if (serve_name_ != nullptr) {
        header.source_name =
            ByteView{reinterpret_cast<const std::uint8_t*>(serve_name_),
                     std::strlen(serve_name_)};
    }

    ManifestWriter writer(scratch_buffer_, scratch_capacity_);
    if (writer.begin(header) != MessageError::Ok) return;
    if (with_source_info &&
        serve_catalog_->write_source_info(&writer) != MessageError::Ok) {
        return;
    }
    if (with_topics && serve_catalog_->write_topics(&writer) != MessageError::Ok) {
        return;
    }
    std::size_t written = 0U;
    if (writer.finish(&written) != MessageError::Ok) return;

    EndpointSealFn seal = nullptr;
    void* seal_ctx = nullptr;
    reply_seal_for(link, request, &seal, &seal_ctx);
    transmit(link, MessageType::Control, object_id::kManifestData, scratch_buffer_,
             written, resolve_now(0U) * 1000ULL, seal, seal_ctx);
}

void Node::serve_manifest(NodeLinkSlot& link, const Header& request,
                          ByteView payload) noexcept {
    if (serve_catalog_ == nullptr) return;

    ManifestRequest req = {};
    if (decode_manifest_request(payload.data, payload.size, &req) !=
        MessageError::Ok) {
        return;
    }
    // Targeted at another source? ignore (0 == the full-catalog request).
    if (req.target_source_id != 0U && req.target_source_id != cfg_.source_id) {
        return;
    }

    RequestRef reply_to = {};
    reply_to.request_source_id = request.source_id;
    reply_to.request_boot_id = request.boot_id;
    reply_to.reply_to_sequence = request.sequence;

    if (req.target_boot_id != 0U && req.target_boot_id != cfg_.boot_id) {
        emit_manifest(link, request, reply_to,
                      static_cast<std::uint8_t>(ResultStatus::Rejected),
                      0U,
                      static_cast<std::uint16_t>(ResultError::StaleTargetBoot),
                      /*with_topics=*/false);
        return;
    }
    if (req.known_config_revision != 0U &&
        req.known_config_revision == serve_catalog_->config_revision()) {
        emit_manifest(link, request, reply_to,
                      static_cast<std::uint8_t>(ResultStatus::Success),
                      kManifestNotModified, 0U, /*with_topics=*/false);
        return;
    }
    emit_manifest(link, request, reply_to,
                  static_cast<std::uint8_t>(ResultStatus::Success),
                  kManifestCatalogComplete, 0U, /*with_topics=*/true);
}

bool Node::announce_catalog() noexcept {
    if (serve_catalog_ == nullptr || scratch_buffer_ == nullptr) {
        return false;
    }
    // Unsolicited: no request to reply to, so nothing to classify by either
    // -- reply_seal_for() falls through to cfg.seal/cfg.seal_ctx for a
    // default-constructed Header exactly as it would for any other source_id
    // a reply_seal callback does not recognise.
    emit_manifest(link0_, Header{}, RequestRef{},
                  static_cast<std::uint8_t>(ResultStatus::Success),
                  kManifestCatalogComplete, 0U, /*with_topics=*/true);
    return true;
}

bool Node::publish(std::uint16_t topic_id, NodeFillFn fill, void* ctx,
                   std::uint64_t timestamp_us) noexcept {
    return publish_with(topic_id, fill, ctx, timestamp_us, current_seal(link0_),
                        current_seal_ctx(link0_));
}

bool Node::publish_with(std::uint16_t topic_id, NodeFillFn fill, void* ctx,
                        std::uint64_t timestamp_us, EndpointSealFn seal,
                        void* seal_ctx) noexcept {
    if (serve_catalog_ == nullptr || fill == nullptr ||
        scratch_buffer_ == nullptr) {
        return false;
    }
    const CatalogTopic* topic = serve_catalog_->topic(topic_id);
    if (topic == nullptr) return false;

    SharedLock guard(cfg_);
    SampleWriter writer(scratch_buffer_, scratch_capacity_, topic->fields,
                        topic->field_count);
    if (writer.begin(topic->schema_version) != MessageError::Ok) return false;
    fill(ctx, writer);
    std::size_t written = 0U;
    if (writer.finish(&written) != MessageError::Ok) return false;

    return send_with(MessageType::Telemetry, topic_id, scratch_buffer_, written,
                     timestamp_us, seal, seal_ctx);
}

bool Node::publish_named(std::uint16_t topic_id, NodeNamedFillFn fill,
                         void* ctx, std::uint64_t timestamp_us) noexcept {
    return publish_named_with(topic_id, fill, ctx, timestamp_us,
                              current_seal(link0_), current_seal_ctx(link0_));
}

bool Node::publish_named_with(std::uint16_t topic_id, NodeNamedFillFn fill,
                              void* ctx, std::uint64_t timestamp_us,
                              EndpointSealFn seal, void* seal_ctx) noexcept {
    return publish_named_on(link0_, topic_id, fill, ctx, timestamp_us, seal,
                            seal_ctx);
}

bool Node::publish_named_on(NodeLinkSlot& link, std::uint16_t topic_id,
                            NodeNamedFillFn fill, void* ctx,
                            std::uint64_t timestamp_us, EndpointSealFn seal,
                            void* seal_ctx) noexcept {
    if (serve_catalog_ == nullptr || fill == nullptr ||
        scratch_buffer_ == nullptr) {
        return false;
    }
    const CatalogTopic* topic = serve_catalog_->topic(topic_id);
    if (topic == nullptr) return false;

    SharedLock guard(cfg_);
    NamedSampleWriter writer(scratch_buffer_, scratch_capacity_, *topic);
    if (writer.begin(topic->schema_version) != MessageError::Ok) return false;
    fill(ctx, writer);
    std::size_t written = 0U;
    if (writer.finish(&written) != MessageError::Ok) return false;

    return transmit(link, MessageType::Telemetry, topic_id, scratch_buffer_,
                    written, timestamp_us, seal, seal_ctx);
}

// ---------------------------------------------------------------------------
// Publish-on-subscribe
// ---------------------------------------------------------------------------

void Node::enable_publish_registry(PublishRegistration* slots,
                                   std::size_t slot_count) noexcept {
    publish_slots_ = slots;
    publish_slot_capacity_ = slot_count;
    publish_slot_count_ = 0U;
}

bool Node::on_publish(std::uint16_t topic_id, NodeNamedFillFn fill,
                      void* ctx) noexcept {
    if (publish_slots_ == nullptr || fill == nullptr ||
        publish_slot_count_ >= publish_slot_capacity_) {
        return false;
    }
    PublishRegistration& slot = publish_slots_[publish_slot_count_];
    slot.topic_id = topic_id;
    slot.fill = fill;
    slot.ctx = ctx;
    ++publish_slot_count_;
    return true;
}

std::size_t Node::publish_subscribed_topics(std::uint64_t now_ms) noexcept {
    if (publish_slots_ == nullptr) return 0U;

    // Every link with its own subscription table publishes to it at its own
    // cadence -- a topic subscribed on two links goes out on both.
    std::size_t published = 0U;
    for (std::size_t l = 0U; l < kMaxNodeLinks; ++l) {
        NodeLinkSlot* link = links_[l];
        if (link == nullptr || link->subscriptions_ == nullptr) continue;
        SubscriptionTable& subscriptions = *link->subscriptions_;
        for (std::size_t i = 0U; i < publish_slot_count_; ++i) {
            const PublishRegistration& reg = publish_slots_[i];
            if (!subscriptions.due(reg.topic_id, now_ms)) continue;
            // Skipped, not counted as an error, when reg.topic_id is not (or
            // no longer) in the served catalogue -- same "unknown topic ->
            // false" rule publish_named() itself already has; there is no
            // separate "was it even a real topic" outcome for a caller to
            // want back here.
            if (publish_named_on(*link, reg.topic_id, reg.fill, reg.ctx,
                                 now_ms * 1000ULL, current_seal(*link),
                                 current_seal_ctx(*link))) {
                subscriptions.note_published(reg.topic_id, now_ms);
                ++published;
            }
        }
    }
    return published;
}

NodeRx Node::routine(const std::uint8_t* datagram, std::size_t size,
                     std::uint64_t now_ms, ReceivedMessage* out) noexcept {
    const NodeRx rx =
        size != 0U ? receive(datagram, size, now_ms, out) : NodeRx::NoDatagram;
    publish_subscribed_topics(now_ms);
    tick(now_ms);
    return rx;
}

NodeRx Node::routine(const std::uint8_t* datagram, std::size_t size,
                     std::uint64_t now_ms) noexcept {
    ReceivedMessage discard{};
    return routine(datagram, size, now_ms, &discard);
}

void Node::routine(std::uint64_t now_ms) noexcept {
    publish_subscribed_topics(now_ms);
    tick(now_ms);
}

// ---------------------------------------------------------------------------
// Session initiator
// ---------------------------------------------------------------------------

bool Node::connect(const Hello& local, std::uint64_t deadline_ms) noexcept {
    return connect(local, resolve_now(0U), deadline_ms);
}

bool Node::connect(const Hello& local, std::uint64_t now_ms,
                   std::uint64_t deadline_ms) noexcept {
    std::uint32_t sequence = 0U;
    if (!endpoint_.reserve_sequence(&sequence)) return false;

    std::uint8_t hello[kSessionMaxHelloSize];
    std::size_t written = 0U;
    if (!initiator_.connect(local, cfg_.source_id, cfg_.boot_id, sequence,
                            now_ms, deadline_ms, hello, sizeof(hello),
                            &written)) {
        return false;
    }

    // HELLO is cleartext -- the handshake bootstraps the session before any
    // key, same as HELLO_RESULT (see receive() above).
    const LogicalMessage message{MessageType::Control, object_id::kHello,
                                 now_ms * 1000ULL, {hello, written}};
    if (!endpoint_.send_logical_reserved(sequence, message, cfg_.transport,
                                         &Node::send_thunk, link0_.cfg_,
                                         seal_scratch_, seal_scratch_cap_,
                                         nullptr, nullptr)) {
        initiator_.reset();  // the HELLO never left -- undo the AwaitingResult
        return false;
    }
    return true;
}

bool Node::connected() const noexcept { return initiator_.connected(); }

const EffectiveLimits& Node::effective_limits() const noexcept {
    return initiator_.effective_limits();
}

std::uint32_t Node::connected_peer_source_id() const noexcept {
    return initiator_.peer_source_id();
}

std::uint32_t Node::connected_peer_boot_id() const noexcept {
    return initiator_.peer_boot_id();
}

std::uint32_t Node::connected_peer_config_revision() const noexcept {
    return initiator_.peer_config_revision();
}

const std::uint8_t* Node::connected_peer_uuid() const noexcept {
    return initiator_.peer_uuid();
}

bool Node::disconnect(std::uint8_t reason,
                      std::uint32_t drain_timeout_ms) noexcept {
    return disconnect(resolve_now(0U), reason, drain_timeout_ms);
}

bool Node::disconnect(std::uint64_t now_ms, std::uint8_t reason,
                      std::uint32_t drain_timeout_ms) noexcept {
    std::uint8_t buffer[kSessionMaxHelloSize];
    std::size_t written = 0U;
    if (!initiator_.disconnect(now_ms, reason, drain_timeout_ms, buffer,
                               sizeof(buffer), &written)) {
        return false;
    }
    return send(MessageType::Control, object_id::kSessionClose, buffer,
               written, now_ms * 1000ULL);
}

// ---------------------------------------------------------------------------
// Session responder
// ---------------------------------------------------------------------------

void Node::enable_session(const Hello& local,
                          std::uint64_t hello_deadline_ms) noexcept {
    link0_.session_ = Session(local, hello_deadline_ms);
    link0_.session_on_ = true;
}

void Node::arm_session() noexcept { arm_session(resolve_now(0U)); }

void Node::arm_session(std::uint64_t now_ms) noexcept {
    if (link0_.session_on_) link0_.session_.arm(now_ms);
}

SessionEvent Node::tick() noexcept { return tick(resolve_now(0U)); }

SessionEvent Node::tick(std::uint64_t now_ms) noexcept {
    for (std::size_t l = 0U; l < kMaxNodeLinks; ++l) {
        NodeLinkSlot* link = links_[l];
        if (link == nullptr) continue;
        link->receiver_.expire(now_ms);
        if (link->subscriptions_ != nullptr) link->subscriptions_->expire(now_ms);
        if (l != 0U && link->session_on_) {
            link->last_session_event_ = link->session_.poll(now_ms).event;
        }
    }
    last_initiator_event_ = initiator_.poll(now_ms).event;
    if (subscription_client_ != nullptr) {
        subscription_client_->expire(now_ms);
        drain_subscription_renewals(now_ms);
    }
    if (command_client_ != nullptr) {
        for (;;) {
            const CommandOutcome outcome = command_client_->expire(now_ms);
            if (outcome.event == CommandEvent::None) break;
            last_command_outcome_ = outcome;  // the last one wins if several timed out
        }
    }
    if (status_period_ms_ != 0U && now_ms - status_last_ms_ >= status_period_ms_) {
        emit_status(now_ms);
        status_last_ms_ = now_ms;
    }
    if (!link0_.session_on_) return SessionEvent::None;
    const SessionOutcome outcome = link0_.session_.poll(now_ms);
    link0_.last_session_event_ = outcome.event;
    return outcome.event;
}

Node::Stats Node::stats() const noexcept { return link_stats(0U); }

Node::Stats Node::link_stats(std::uint8_t link) const noexcept {
    Stats s{};
    const NodeLinkSlot* l = slot(link);
    if (l == nullptr) return s;
    s.rx = l->receiver_.stats();
    s.session_path_dropped_crc = l->session_path_dropped_crc_;
    s.session_path_dropped_decode = l->session_path_dropped_decode_;
    s.dropped_cleartext = l->dropped_cleartext_;
    s.dropped_open_failed = l->dropped_open_failed_;
    return s;
}

const char* node_rx_string(NodeRx rx) noexcept {
    switch (rx) {
        case NodeRx::Complete: return "complete";
        case NodeRx::Pending: return "pending";
        case NodeRx::SessionHandled: return "session handled";
        case NodeRx::InitiatorHandled: return "initiator handled";
        case NodeRx::SubscriptionServed: return "subscription served";
        case NodeRx::SubscriptionHandled: return "subscription handled";
        case NodeRx::CommandServed: return "command served";
        case NodeRx::CommandHandled: return "command handled";
        case NodeRx::CatalogUpdated: return "catalog updated";
        case NodeRx::SampleDelivered: return "sample delivered";
        case NodeRx::TerminalDelivered: return "terminal delivered";
        case NodeRx::RequestServed: return "request served";
        case NodeRx::Ignored: return "ignored";
        case NodeRx::DroppedFrame: return "dropped frame";
        case NodeRx::NoDatagram: return "no datagram";
        case NodeRx::ManifestHandled: return "manifest handled";
        case NodeRx::ManifestRejected: return "manifest rejected";
    }
    return "unknown";
}

}  // namespace btp
