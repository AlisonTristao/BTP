#include "btp/node.hpp"

#include <cstring>

namespace btp {

Node::Node(NodeConfig& cfg, ReassemblySlot* slots,
           const ReassemblyStorage* storage, std::size_t slot_count,
           std::uint64_t reassembly_timeout_ms, std::uint8_t* rx_buffer,
           std::size_t rx_capacity, std::uint8_t* seal_scratch,
           std::size_t seal_scratch_cap, std::uint8_t* open_buffer,
           std::size_t open_capacity, std::uint8_t* scratch_buffer,
           std::size_t scratch_capacity) noexcept
    : cfg_(cfg),
      rx_buffer_(rx_buffer),
      rx_capacity_(rx_capacity),
      seal_scratch_(seal_scratch),
      seal_scratch_cap_(seal_scratch_cap),
      open_buffer_(open_buffer),
      open_capacity_(open_capacity),
      scratch_buffer_(scratch_buffer),
      scratch_capacity_(scratch_capacity),
      endpoint_(),
      receiver_(slots, storage, slot_count, reassembly_timeout_ms,
                cfg.transport),
      session_(Hello{}, 0U),
      session_on_(false),
      last_session_event_(SessionEvent::None),
      session_path_dropped_crc_(0U),
      session_path_dropped_decode_(0U),
      initiator_(),
      last_initiator_event_(InitiatorEvent::None),
      subscriptions_(nullptr),
      subscription_client_(nullptr),
      last_subscription_event_(SubscriptionEvent::None),
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
      learn_catalog_(nullptr),
      on_sample_(nullptr),
      on_sample_ctx_(nullptr),
      serve_catalog_(nullptr),
      serve_role_(0U),
      serve_uuid_(),
      serve_name_(nullptr),
      publish_slots_(nullptr),
      publish_slot_capacity_(0U),
      publish_slot_count_(0U) {}

// ---------------------------------------------------------------------------
// NodeConfig bridges -- the only place a virtual call becomes a C-style
// function pointer for btp::Endpoint / btp::Receiver. `ctx` is always the
// NodeConfig this Node (or, for command_thunk, a StaticNode<>'s own
// cfg_ reached via its own enable_commands() call) was built with.
// ---------------------------------------------------------------------------

bool Node::send_thunk(void* ctx, const std::uint8_t* frame,
                      std::size_t frame_size) noexcept {
    return static_cast<NodeConfig*>(ctx)->send(frame, frame_size);
}

bool Node::seal_thunk(void* ctx, const Header& header, std::uint16_t payload_size,
                      const std::uint8_t* plaintext, std::uint8_t* out) noexcept {
    return static_cast<NodeConfig*>(ctx)->seal(header, payload_size, plaintext, out);
}

void Node::terminal_thunk(void* ctx, Node& node, const Header& header,
                          ByteView payload, std::uint64_t now_ms) noexcept {
    static_cast<NodeConfig*>(ctx)->terminal(node, header, payload, now_ms);
}

void Node::command_thunk(void* ctx, std::uint16_t action_id,
                         std::uint16_t action_version, ByteView parameters,
                         NodeActionOutcome* outcome) noexcept {
    static_cast<NodeConfig*>(ctx)->command(action_id, action_version, parameters,
                                           outcome);
}

EndpointSealFn Node::current_seal() const noexcept {
    return cfg_.has_seal() ? &Node::seal_thunk : nullptr;
}

void* Node::current_seal_ctx() const noexcept {
    return cfg_.has_seal() ? &cfg_ : nullptr;
}

bool Node::begin(bool arm_and_announce) noexcept {
    if (!endpoint_.configure(cfg_.source_id, cfg_.boot_id)) return false;
    if (!receiver_.valid()) return false;
    if (session_on_ && !session_.valid()) return false;
    // NodeConfig::send() is always implemented (it is pure virtual) -- a
    // receive-only node's own override just returns false unconditionally,
    // so there is nothing to check here any more.

    if (arm_and_announce) {
        if (session_on_) arm_session();
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
    return endpoint_.configured() && receiver_.valid() &&
           (!session_on_ || session_.valid());
}

std::uint64_t Node::resolve_now(std::uint64_t fallback) const noexcept {
    return cfg_.has_clock() ? cfg_.clock() : fallback;
}

// ---------------------------------------------------------------------------
// Transmit
// ---------------------------------------------------------------------------

bool Node::send_with(MessageType type, std::uint16_t object_id,
                     const std::uint8_t* payload, std::size_t size,
                     std::uint64_t timestamp_us, EndpointSealFn seal,
                     void* seal_ctx) noexcept {
    const LogicalMessage message{type, object_id, timestamp_us,
                                 {payload, size}};
    const bool ok = endpoint_.send_logical(message, cfg_.transport, &Node::send_thunk,
                                          &cfg_, seal_scratch_,
                                          seal_scratch_cap_, seal, seal_ctx);
    if (ok) ++frames_tx_;
    return ok;
}

bool Node::send(MessageType type, std::uint16_t object_id,
                const std::uint8_t* payload, std::size_t size,
                std::uint64_t timestamp_us) noexcept {
    return send_with(type, object_id, payload, size, timestamp_us,
                     current_seal(), current_seal_ctx());
}

void Node::reply_seal_for(const Header& request, EndpointSealFn* out_seal,
                          void** out_seal_ctx) const noexcept {
    *out_seal = nullptr;
    *out_seal_ctx = nullptr;
    cfg_.reply_seal(request, out_seal, out_seal_ctx);
    if (*out_seal == nullptr) {
        *out_seal = current_seal();
        *out_seal_ctx = current_seal_ctx();
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
    last_session_event_ = SessionEvent::None;
    last_initiator_event_ = InitiatorEvent::None;
    if (out == nullptr || datagram == nullptr || size == 0U) {
        return NodeRx::DroppedFrame;
    }

    const bool initiator_live = initiator_.state() != InitiatorState::Idle;
    if (!session_on_ && !initiator_live) {
        return finish(receiver_.submit(datagram, size, now_ms, rx_buffer_,
                                       rx_capacity_, out),
                      out, now_ms);
    }

    // A session (either direction) needs the DecodedFrame btp::Receiver keeps
    // to itself, so the decode happens here; its failures are counted apart
    // (stats()).
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

    return route_decoded(decoded, now_ms, out);
}

NodeRx Node::receive(const DecodedFrame& frame, ReceivedMessage* out) noexcept {
    return receive(frame, resolve_now(0U), out);
}

NodeRx Node::receive(const DecodedFrame& frame, std::uint64_t now_ms,
                     ReceivedMessage* out) noexcept {
    last_session_event_ = SessionEvent::None;
    last_initiator_event_ = InitiatorEvent::None;
    if (out == nullptr) return NodeRx::DroppedFrame;
    return route_decoded(frame, now_ms, out);
}

NodeRx Node::route_decoded(const DecodedFrame& decoded, std::uint64_t now_ms,
                           ReceivedMessage* out) noexcept {
    const bool initiator_live = initiator_.state() != InitiatorState::Idle;

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

    if (!session_on_) {
        return finish(
            receiver_.submit(decoded, now_ms, rx_buffer_, rx_capacity_, out),
            out, now_ms);
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
        endpoint_.send_logical(reply_msg, cfg_.transport, &Node::send_thunk, &cfg_,
                               seal_scratch_, seal_scratch_cap_, nullptr, nullptr);
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
        receiver_.submit(decoded, now_ms, rx_buffer_, rx_capacity_, out), out,
        now_ms);
}

NodeRx Node::finish(ReceiveOutcome outcome, ReceivedMessage* out,
                    std::uint64_t now_ms) noexcept {
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
    if (cfg_.has_open() && (out->header.flags & kFlagEncrypted) != 0U) {
        if (out->payload.size < kEndpointAeadTagSize || open_buffer_ == nullptr ||
            out->payload.size - kEndpointAeadTagSize > open_capacity_) {
            return NodeRx::DroppedFrame;
        }
        if (!cfg_.open(out->header,
                       static_cast<std::uint16_t>(out->payload.size),
                       out->payload.data, open_buffer_)) {
            return NodeRx::DroppedFrame;
        }
        out->payload =
            ByteView{open_buffer_, out->payload.size - kEndpointAeadTagSize};
    }

    // ----- discovery the node manages itself -----
    if (serve_catalog_ != nullptr && out->header.type == MessageType::Control &&
        out->header.object_id == object_id::kManifestRequest) {
        serve_manifest(out->header, out->payload);
        return NodeRx::RequestServed;
    }
    if (learn_catalog_ != nullptr &&
        out->header.type == MessageType::Control &&
        out->header.object_id == object_id::kManifestData) {
        return learn_catalog_->ingest(out->payload.data, out->payload.size) ==
                       MessageError::Ok
                   ? NodeRx::CatalogUpdated
                   : NodeRx::DroppedFrame;
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
    if (subscriptions_ != nullptr && out->header.type == MessageType::Control &&
        out->header.object_id == object_id::kSubscribe) {
        serve_subscribe(out->header, out->payload, now_ms);
        return NodeRx::SubscriptionServed;
    }
    if (subscriptions_ != nullptr && out->header.type == MessageType::Control &&
        out->header.object_id == object_id::kUnsubscribe) {
        serve_unsubscribe(out->header, out->payload);
        return NodeRx::SubscriptionServed;
    }
    if (subscription_client_ != nullptr && out->header.type == MessageType::Control &&
        out->header.object_id == object_id::kSubscribeResult) {
        SubscribeResult result = {};
        if (decode_subscribe_result(out->payload.data, out->payload.size, &result) ==
            MessageError::Ok) {
            last_subscription_event_ =
                subscription_client_->on_result(result, now_ms).event;
        }
        return NodeRx::SubscriptionHandled;
    }

    // ----- commands -----
    if (commands_ != nullptr && on_command_ != nullptr &&
        out->header.type == MessageType::Command &&
        out->header.object_id == object_id::kCommandRequest) {
        serve_command(out->header, out->payload);
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
        on_terminal_(on_terminal_ctx_, *this, out->header, out->payload, now_ms);
        return NodeRx::TerminalDelivered;
    }

    return NodeRx::Complete;
}

// ---------------------------------------------------------------------------
// Subscriptions
// ---------------------------------------------------------------------------

void Node::serve_subscribe(const Header& request, ByteView payload,
                           std::uint64_t now_ms) noexcept {
    if (subscriptions_ == nullptr || serve_catalog_ == nullptr) return;
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
    subscriptions_->handle_subscribe(*serve_catalog_, request, req, now_ms, &result);

    std::uint8_t buffer[40];
    std::size_t written = 0U;
    if (encode_subscribe_result(result, buffer, sizeof(buffer), &written) !=
        MessageError::Ok) {
        return;
    }
    EndpointSealFn seal = nullptr;
    void* seal_ctx = nullptr;
    reply_seal_for(request, &seal, &seal_ctx);
    send_with(MessageType::Control, object_id::kSubscribeResult, buffer, written,
             resolve_now(0U) * 1000ULL, seal, seal_ctx);
}

void Node::serve_unsubscribe(const Header& request, ByteView payload) noexcept {
    if (subscriptions_ == nullptr) return;
    Unsubscribe req = {};
    if (decode_unsubscribe(payload.data, payload.size, &req) != MessageError::Ok) {
        return;
    }
    if (req.target_source_id != cfg_.source_id ||
        req.target_boot_id != cfg_.boot_id) {
        return;
    }

    ControlResult result = {};
    subscriptions_->handle_unsubscribe(request, req, &result);

    std::uint8_t buffer[24];
    std::size_t written = 0U;
    if (encode_unsubscribe_result(result, buffer, sizeof(buffer), &written) !=
        MessageError::Ok) {
        return;
    }
    EndpointSealFn seal = nullptr;
    void* seal_ctx = nullptr;
    reply_seal_for(request, &seal, &seal_ctx);
    send_with(MessageType::Control, object_id::kUnsubscribeResult, buffer, written,
             resolve_now(0U) * 1000ULL, seal, seal_ctx);
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

    const LogicalMessage message{MessageType::Control, object_id::kSubscribe,
                                 now_ms * 1000ULL, {buffer, written}};
    // A send failure here is rare (the frame fit at subscribe() time) and not
    // fatal: the slot stays Pending and simply times out via expire(), the
    // same fail-safe as any other lost frame.
    endpoint_.send_logical_reserved(sequence, message, cfg_.transport,
                                   &Node::send_thunk, &cfg_, seal_scratch_,
                                   seal_scratch_cap_, current_seal(),
                                   current_seal_ctx());
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
    const LogicalMessage message{MessageType::Control, object_id::kUnsubscribe,
                                 resolve_now(0U) * 1000ULL, {buffer, written}};
    return endpoint_.send_logical_reserved(sequence, message, cfg_.transport,
                                          &Node::send_thunk, &cfg_, seal_scratch_,
                                          seal_scratch_cap_, current_seal(),
                                          current_seal_ctx());
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
        const LogicalMessage message{MessageType::Control, object_id::kSubscribe,
                                     now_ms * 1000ULL, {buffer, written}};
        endpoint_.send_logical_reserved(sequence, message, cfg_.transport,
                                       &Node::send_thunk, &cfg_, seal_scratch_,
                                       seal_scratch_cap_, current_seal(),
                                       current_seal_ctx());
    }
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void Node::serve_command(const Header& request, ByteView payload) noexcept {
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
            on_command_(on_command_ctx_, req.action_id, req.action_version,
                       req.parameters, &outcome);
            emit_command_result(request, req.action_id, req.action_version, outcome,
                                slot);
            break;
        }
        case DedupVerdict::DuplicateComplete: {
            // A retransmission of an identity already executed: replay the
            // exact stored result, do not run the action again.
            EndpointSealFn seal = nullptr;
            void* seal_ctx = nullptr;
            reply_seal_for(request, &seal, &seal_ctx);
            send_with(MessageType::Command, object_id::kCommandResult, stored.data,
                     stored.size, resolve_now(0U) * 1000ULL, seal, seal_ctx);
            break;
        }
        case DedupVerdict::DuplicateInFlight:
            break;  // still executing -- drop, the peer will retry
        case DedupVerdict::Conflict:
            emit_command_reject(request, req.action_id, req.action_version,
                                ResultStatus::Rejected, ResultError::RequestConflict);
            break;
        case DedupVerdict::Evicted:
        case DedupVerdict::CapacityExhausted:
            emit_command_reject(request, req.action_id, req.action_version,
                                ResultStatus::Busy, ResultError::CapacityExhausted);
            break;
        case DedupVerdict::InvalidArgument:
            break;  // a null pointer or an oversized request -- drop
    }
}

void Node::emit_command_result(const Header& request, std::uint16_t action_id,
                               std::uint16_t action_version,
                               const NodeActionOutcome& outcome,
                               std::size_t slot) noexcept {
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
        return;
    }
    // Remember the result BEFORE sending -- the action ran exactly once
    // whether or not this send succeeds; a later retransmission of the same
    // identity must still find it and replay rather than running again.
    commands_->record_result(slot, scratch_buffer_, written);
    EndpointSealFn seal = nullptr;
    void* seal_ctx = nullptr;
    reply_seal_for(request, &seal, &seal_ctx);
    send_with(MessageType::Command, object_id::kCommandResult, scratch_buffer_, written,
             resolve_now(0U) * 1000ULL, seal, seal_ctx);
}

void Node::emit_command_reject(const Header& request, std::uint16_t action_id,
                               std::uint16_t action_version, ResultStatus status,
                               ResultError error) noexcept {
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
    reply_seal_for(request, &seal, &seal_ctx);
    send_with(MessageType::Command, object_id::kCommandResult, scratch_buffer_, written,
             resolve_now(0U) * 1000ULL, seal, seal_ctx);
}

std::uint32_t Node::command(std::uint32_t peer_source_id, std::uint32_t peer_boot_id,
                            std::uint16_t action_id, std::uint16_t action_version,
                            const std::uint8_t* parameters,
                            std::size_t parameters_size) noexcept {
    if (command_client_ == nullptr || scratch_buffer_ == nullptr) {
        return 0U;
    }
    std::uint32_t sequence = 0U;
    if (!endpoint_.reserve_sequence(&sequence)) return 0U;

    const std::uint64_t now_ms = resolve_now(0U);
    std::size_t written = 0U;
    const std::uint32_t local_id = command_client_->command(
        peer_source_id, peer_boot_id, action_id, action_version, parameters,
        parameters_size, cfg_.source_id, cfg_.boot_id, sequence, now_ms,
        scratch_buffer_, scratch_capacity_, &written);
    if (local_id == 0U) return 0U;

    const LogicalMessage message{MessageType::Command, object_id::kCommandRequest,
                                 now_ms * 1000ULL, {scratch_buffer_, written}};
    // A send failure here is rare and not fatal: the slot stays Pending and
    // simply times out via expire(), the same fail-safe as any lost frame.
    endpoint_.send_logical_reserved(sequence, message, cfg_.transport,
                                   &Node::send_thunk, &cfg_, seal_scratch_,
                                   seal_scratch_cap_, current_seal(),
                                   current_seal_ctx());
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

    const Receiver::Stats rx = receiver_.stats();
    StatusV1 status = {};
    status.uptime_us = now_ms >= status_started_ms_
                           ? (now_ms - status_started_ms_) * 1000ULL
                           : 0ULL;
    status.frames_rx = rx.completed;
    status.frames_tx = frames_tx_;
    status.crc_errors = static_cast<std::uint64_t>(rx.dropped_crc) +
                        session_path_dropped_crc_;
    status.decode_errors = static_cast<std::uint64_t>(rx.dropped_decode) +
                           session_path_dropped_decode_;
    status.reassembly_completed = rx.completed;
    status.reassembly_timeouts = rx.reassembly_timeouts;
    status.reassembly_rejected = rx.dropped_reassembly;
    status.frames_dropped =
        status.crc_errors + status.decode_errors + status.reassembly_rejected;
    status.command_duplicates =
        commands_ != nullptr ? commands_->stats().replayed : 0U;
    status.telemetry_dropped = 0U;  // not tracked separately yet

    std::size_t written = 0U;
    if (encode_status_v1(status, scratch_buffer_, scratch_capacity_, &written) !=
        MessageError::Ok) {
        return;
    }
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
    return send(MessageType::Control, object_id::kManifestRequest, buffer,
                written, resolve_now(0U) * 1000ULL);
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

void Node::emit_manifest(const Header& request, const RequestRef& reply_to,
                         std::uint8_t status, std::uint8_t flags,
                         std::uint16_t error_code, bool with_topics) noexcept {
    if (serve_catalog_ == nullptr || scratch_buffer_ == nullptr) {
        return;
    }

    // Format 2 whenever there is a source_info block to carry on a SUCCESS
    // reply -- a full response AND a NOT_MODIFIED one (commands.md 3.3:
    // source_info is not covered by config_revision, so it still follows
    // source_name in a NOT_MODIFIED response). A REJECTED reply
    // (STALE_TARGET_BOOT / NOT_FOUND) describes no source and stays format 1.
    const bool with_source_info =
        serve_catalog_->has_source_info() &&
        status == static_cast<std::uint8_t>(ResultStatus::Success);

    ManifestHeader header = {};
    header.request = reply_to;
    header.status = status;
    header.flags = flags;
    header.error_code = error_code;
    header.manifest_format_version = with_source_info ? 2U : 1U;
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
    reply_seal_for(request, &seal, &seal_ctx);
    send_with(MessageType::Control, object_id::kManifestData, scratch_buffer_,
             written, resolve_now(0U) * 1000ULL, seal, seal_ctx);
}

void Node::serve_manifest(const Header& request, ByteView payload) noexcept {
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
        emit_manifest(request, reply_to,
                      static_cast<std::uint8_t>(ResultStatus::Rejected),
                      0U,
                      static_cast<std::uint16_t>(ResultError::StaleTargetBoot),
                      /*with_topics=*/false);
        return;
    }
    if (req.known_config_revision != 0U &&
        req.known_config_revision == serve_catalog_->config_revision()) {
        emit_manifest(request, reply_to,
                      static_cast<std::uint8_t>(ResultStatus::Success),
                      kManifestNotModified, 0U, /*with_topics=*/false);
        return;
    }
    emit_manifest(request, reply_to, static_cast<std::uint8_t>(ResultStatus::Success),
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
    emit_manifest(Header{}, RequestRef{}, static_cast<std::uint8_t>(ResultStatus::Success),
                  kManifestCatalogComplete, 0U, /*with_topics=*/true);
    return true;
}

bool Node::publish(std::uint16_t topic_id, NodeFillFn fill, void* ctx,
                   std::uint64_t timestamp_us) noexcept {
    return publish_with(topic_id, fill, ctx, timestamp_us, current_seal(),
                        current_seal_ctx());
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
    return publish_named_with(topic_id, fill, ctx, timestamp_us, current_seal(),
                              current_seal_ctx());
}

bool Node::publish_named_with(std::uint16_t topic_id, NodeNamedFillFn fill,
                              void* ctx, std::uint64_t timestamp_us,
                              EndpointSealFn seal, void* seal_ctx) noexcept {
    if (serve_catalog_ == nullptr || fill == nullptr ||
        scratch_buffer_ == nullptr) {
        return false;
    }
    const CatalogTopic* topic = serve_catalog_->topic(topic_id);
    if (topic == nullptr) return false;

    NamedSampleWriter writer(scratch_buffer_, scratch_capacity_, *topic);
    if (writer.begin(topic->schema_version) != MessageError::Ok) return false;
    fill(ctx, writer);
    std::size_t written = 0U;
    if (writer.finish(&written) != MessageError::Ok) return false;

    return send_with(MessageType::Telemetry, topic_id, scratch_buffer_, written,
                     timestamp_us, seal, seal_ctx);
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
    if (publish_slots_ == nullptr || subscriptions_ == nullptr) return 0U;

    std::size_t published = 0U;
    for (std::size_t i = 0U; i < publish_slot_count_; ++i) {
        const PublishRegistration& reg = publish_slots_[i];
        if (!subscriptions_->due(reg.topic_id, now_ms)) continue;
        // Skipped, not counted as an error, when reg.topic_id is not (or no
        // longer) in the served catalogue -- same "unknown topic -> false"
        // rule publish_named() itself already has; there is no separate
        // "was it even a real topic" outcome for a caller to want back here.
        if (publish_named(reg.topic_id, reg.fill, reg.ctx, now_ms * 1000ULL)) {
            subscriptions_->note_published(reg.topic_id, now_ms);
            ++published;
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
                                         &Node::send_thunk, &cfg_,
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
    last_initiator_event_ = initiator_.poll(now_ms).event;
    if (subscriptions_ != nullptr) subscriptions_->expire(now_ms);
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
    }
    return "unknown";
}

}  // namespace btp
