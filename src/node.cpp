#include "btp/node.hpp"

#include <cstring>

namespace btp {

Node::Node(const NodeConfig& cfg, ReassemblySlot* slots,
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
      learn_catalog_(nullptr),
      on_sample_(nullptr),
      on_sample_ctx_(nullptr),
      serve_catalog_(nullptr),
      serve_role_(0U),
      serve_uuid_(),
      serve_name_(nullptr) {}

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
    last_initiator_event_ = InitiatorEvent::None;
    if (out == nullptr || datagram == nullptr || size == 0U) {
        return NodeRx::DroppedFrame;
    }

    const bool initiator_live = initiator_.state() != InitiatorState::Idle;
    if (!session_on_ && !initiator_live) {
        return finish(receiver_.submit(datagram, size, now_ms, rx_buffer_,
                                       rx_capacity_, out),
                      out);
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
            out);
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

    return NodeRx::Complete;
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

void Node::emit_manifest(const RequestRef& reply_to, std::uint8_t status,
                         std::uint8_t flags, std::uint16_t error_code,
                         bool with_topics) noexcept {
    if (serve_catalog_ == nullptr || cfg_.send == nullptr ||
        scratch_buffer_ == nullptr) {
        return;
    }

    ManifestHeader header = {};
    header.request = reply_to;
    header.status = status;
    header.flags = flags;
    header.error_code = error_code;
    header.manifest_format_version = 1U;
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
    if (with_topics && serve_catalog_->write_topics(&writer) != MessageError::Ok) {
        return;
    }
    std::size_t written = 0U;
    if (writer.finish(&written) != MessageError::Ok) return;

    send(MessageType::Control, object_id::kManifestData, scratch_buffer_,
         written, resolve_now(0U) * 1000ULL);
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
        emit_manifest(reply_to,
                      static_cast<std::uint8_t>(ResultStatus::Rejected),
                      0U,
                      static_cast<std::uint16_t>(ResultError::StaleTargetBoot),
                      /*with_topics=*/false);
        return;
    }
    if (req.known_config_revision != 0U &&
        req.known_config_revision == serve_catalog_->config_revision()) {
        emit_manifest(reply_to,
                      static_cast<std::uint8_t>(ResultStatus::Success),
                      kManifestNotModified, 0U, /*with_topics=*/false);
        return;
    }
    emit_manifest(reply_to, static_cast<std::uint8_t>(ResultStatus::Success),
                  kManifestCatalogComplete, 0U, /*with_topics=*/true);
}

bool Node::announce_catalog() noexcept {
    if (serve_catalog_ == nullptr || cfg_.send == nullptr ||
        scratch_buffer_ == nullptr) {
        return false;
    }
    emit_manifest(RequestRef{}, static_cast<std::uint8_t>(ResultStatus::Success),
                  kManifestCatalogComplete, 0U, /*with_topics=*/true);
    return true;
}

bool Node::publish(std::uint16_t topic_id, NodeFillFn fill, void* ctx,
                   std::uint64_t timestamp_us) noexcept {
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

    return send(MessageType::Telemetry, topic_id, scratch_buffer_, written,
                timestamp_us);
}

bool Node::publish_named(std::uint16_t topic_id, NodeNamedFillFn fill,
                         void* ctx, std::uint64_t timestamp_us) noexcept {
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

    return send(MessageType::Telemetry, topic_id, scratch_buffer_, written,
                timestamp_us);
}

// ---------------------------------------------------------------------------
// Session initiator
// ---------------------------------------------------------------------------

bool Node::connect(const Hello& local, std::uint64_t deadline_ms) noexcept {
    return connect(local, resolve_now(0U), deadline_ms);
}

bool Node::connect(const Hello& local, std::uint64_t now_ms,
                   std::uint64_t deadline_ms) noexcept {
    if (cfg_.send == nullptr) return false;
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
                                         cfg_.send, cfg_.send_ctx,
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
    if (cfg_.send == nullptr) return false;
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
        case NodeRx::CatalogUpdated: return "catalog updated";
        case NodeRx::SampleDelivered: return "sample delivered";
        case NodeRx::RequestServed: return "request served";
        case NodeRx::Ignored: return "ignored";
        case NodeRx::DroppedFrame: return "dropped frame";
    }
    return "unknown";
}

}  // namespace btp
