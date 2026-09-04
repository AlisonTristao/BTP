// Unit tests for btp::Node -- the facade of docs/library.md chapter 16.
//
// The Node is wiring over btp::Endpoint + btp::Receiver + btp::Session, not a
// new wire layout, so there is no vector tree. Every test drives a Node (often
// a sender / receiver pair) and checks the frames, the reassembled payloads and
// the session replies with the public btp:: APIs.

#include "btp/node.hpp"

#include "btp/catalog.hpp"
#include "btp/codec.hpp"
#include "btp/messages.hpp"
#include "btp/telemetry.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << __FILE__ << ':' << __LINE__                            \
                      << ": check failed: " #condition << '\n';                 \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

using btp::Hello;
using btp::MessageError;
using btp::MessageType;
using btp::NodeConfig;
using btp::NodeRx;
using btp::ReceivedMessage;
using btp::Role;
using btp::kEspNowTransport;
using btp::SessionEvent;

constexpr std::uint32_t kSenderId = 0x00CAFE01U;
constexpr std::uint32_t kSenderBoot = 0x0000B001U;
constexpr std::uint32_t kPeerId = 0x00B0B0FEU;
constexpr std::uint32_t kPeerBoot = 0x0000C0DEU;
// A second requester identity, for reply_seal_for() tests: distinct from
// kPeerId so a picker callback keyed on request_header.source_id can be
// checked against both "the known alt requester" and "everyone else".
constexpr std::uint32_t kAltPeerId = 0x00A17E51U;
constexpr std::uint32_t kAltPeerBoot = 0x0000A17EU;

// Captures every frame a Node hands to its send callback.
struct Sink {
    std::vector<std::vector<std::uint8_t> > frames;
    static bool send(void* ctx, const std::uint8_t* f, std::size_t n) {
        static_cast<Sink*>(ctx)->frames.emplace_back(f, f + n);
        return true;
    }
    void clear() { frames.clear(); }
    std::size_t count() const { return frames.size(); }
};

// A reversible stand-in for AEAD: the node's seal / open wiring is exercised
// without linking btp::aead. XORs the payload and appends a fixed 16-octet
// "tag" (btp::kEndpointAeadTagSize).
bool fake_seal(void*, const btp::Header&, std::uint16_t n,
               const std::uint8_t* plaintext, std::uint8_t* out) {
    for (std::uint16_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::uint8_t>(plaintext[i] ^ 0x5AU);
    }
    for (std::size_t i = 0; i < btp::kEndpointAeadTagSize; ++i) {
        out[n + i] = 0xA5U;
    }
    return true;
}

bool fake_open(void*, const btp::Header&, std::uint16_t sealed_size,
               const std::uint8_t* sealed, std::uint8_t* out_plaintext) {
    if (sealed_size < btp::kEndpointAeadTagSize) return false;
    const std::uint16_t n =
        static_cast<std::uint16_t>(sealed_size - btp::kEndpointAeadTagSize);
    for (std::size_t i = 0; i < btp::kEndpointAeadTagSize; ++i) {
        if (sealed[n + i] != 0xA5U) return false;
    }
    for (std::uint16_t i = 0; i < n; ++i) {
        out_plaintext[i] = static_cast<std::uint8_t>(sealed[i] ^ 0x5AU);
    }
    return true;
}

// A SECOND, distinguishable stand-in key (0x3C / tag 0x5B, vs. fake_seal's
// 0x5A / 0xA5) -- reply_seal_for() tests use the tag byte alone to tell
// which of the two actually sealed a given outgoing frame, the same way a
// real test would distinguish RadioSeal::seal from RadioSeal::seal_e.
bool fake_seal_b(void*, const btp::Header&, std::uint16_t n,
                 const std::uint8_t* plaintext, std::uint8_t* out) {
    for (std::uint16_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::uint8_t>(plaintext[i] ^ 0x3CU);
    }
    for (std::size_t i = 0; i < btp::kEndpointAeadTagSize; ++i) {
        out[n + i] = 0x5BU;
    }
    return true;
}

bool frame_tagged_b(const std::vector<std::uint8_t>& frame) {
    // header | sealed_payload (ciphertext + 16-octet AEAD tag) | crc32 (4
    // octets) -- the last tag octet sits right before the 4-octet CRC
    // trailer, whichever fake_seal wrote it.
    constexpr std::size_t kCrcSize = 4U;
    if (frame.size() < kCrcSize + 1U) return false;
    return frame[frame.size() - kCrcSize - 1U] == 0x5BU;
}

std::vector<std::uint8_t> make_payload(std::size_t n, std::uint8_t seed) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<std::uint8_t>(seed + i);
    }
    return v;
}

Hello make_hello(Role role) {
    Hello h{};
    h.role = static_cast<std::uint8_t>(role);
    h.version_count = 1U;
    h.versions[0] = 1U;
    h.max_logical_payload = 2048U;
    h.max_inflight_reassemblies = 4U;
    h.max_subscriptions = 8U;
    h.max_dedup_entries = 32U;
    h.session_timeout_ms = 30000U;
    for (int i = 0; i < 16; ++i) {
        h.peer_uuid[i] = static_cast<std::uint8_t>(i + 1);
    }
    h.config_revision = 0U;
    return h;
}

// Every axis TestConfig now exposes as a virtual method, forwarded to a
// settable raw function pointer -- keeps the free-function test doubles
// above (Sink::send, fake_seal, fake_open, ...) usable exactly as they were
// against the old POD TestConfig, one class, written once, instead of a
// subclass per test scenario. nullptr means "this axis is off", same
// meaning a null TestConfig field had before TestConfig became abstract.
class TestConfig : public btp::NodeConfig {
public:
    btp::EndpointSendFn send_fn = nullptr;
    void* send_ctx = nullptr;
    std::uint64_t (*clock_fn)(void*) = nullptr;
    void* clock_ctx = nullptr;
    btp::EndpointSealFn seal_fn = nullptr;
    void* seal_ctx = nullptr;
    btp::EndpointSealFn open_fn = nullptr;  // same shape as EndpointSealFn
    void* open_ctx = nullptr;
    btp::NodeTerminalFn terminal_fn = nullptr;
    void* terminal_ctx = nullptr;
    btp::NodeActionFn command_fn = nullptr;
    void* command_ctx = nullptr;
    void (*reply_seal_fn)(void*, const btp::Header&, btp::EndpointSealFn*,
                          void**) = nullptr;
    void* reply_seal_ctx = nullptr;

    bool send(const std::uint8_t* frame, std::size_t n) override {
        return send_fn != nullptr && send_fn(send_ctx, frame, n);
    }
    bool has_clock() const noexcept override { return clock_fn != nullptr; }
    std::uint64_t clock() override { return clock_fn(clock_ctx); }
    bool has_seal() const noexcept override { return seal_fn != nullptr; }
    bool seal(const btp::Header& h, std::uint16_t n, const std::uint8_t* pt,
             std::uint8_t* out) override {
        return seal_fn(seal_ctx, h, n, pt, out);
    }
    bool has_open() const noexcept override { return open_fn != nullptr; }
    bool open(const btp::Header& h, std::uint16_t n, const std::uint8_t* s,
             std::uint8_t* out) override {
        return open_fn(open_ctx, h, n, s, out);
    }
    bool has_terminal() const noexcept override { return terminal_fn != nullptr; }
    void terminal(btp::Node& node, const btp::Header& h, btp::ByteView p,
                  std::uint64_t now_ms) override {
        terminal_fn(terminal_ctx, node, h, p, now_ms);
    }
    bool has_command() const noexcept override { return command_fn != nullptr; }
    void command(std::uint16_t id, std::uint16_t ver, btp::ByteView params,
                btp::NodeActionOutcome* out) override {
        command_fn(command_ctx, id, ver, params, out);
    }
    void reply_seal(const btp::Header& request, btp::EndpointSealFn* out_seal,
                    void** out_ctx) override {
        if (reply_seal_fn != nullptr) {
            reply_seal_fn(reply_seal_ctx, request, out_seal, out_ctx);
        } else {
            *out_seal = nullptr;
            *out_ctx = nullptr;
        }
    }
};

TestConfig base_config(std::uint32_t source_id, std::uint32_t boot_id,
                       Sink* sink) {
    TestConfig cfg{};
    cfg.source_id = source_id;
    cfg.boot_id = boot_id;
    cfg.transport = kEspNowTransport;
    cfg.send_fn = &Sink::send;
    cfg.send_ctx = sink;
    cfg.clock_fn = nullptr;
    cfg.clock_ctx = nullptr;
    cfg.seal_fn = nullptr;
    cfg.seal_ctx = nullptr;
    cfg.open_fn = nullptr;
    cfg.open_ctx = nullptr;
    return cfg;
}

using TestNode = btp::StaticNode<4, 700, 1024>;

// Deliver every frame `sink` collected to `dst`, returning the last outcome.
NodeRx deliver(TestNode& dst, Sink& sink, std::uint64_t now_ms,
               ReceivedMessage* msg) {
    NodeRx last = NodeRx::Pending;
    for (std::size_t i = 0; i < sink.frames.size(); ++i) {
        last = dst.receive(sink.frames[i].data(), sink.frames[i].size(), now_ms,
                           msg);
    }
    return last;
}

// ---------------------------------------------------------------------------

void test_begin() {
    Sink sink;

    TestConfig ok_cfg = base_config(kSenderId, kSenderBoot, &sink);

    TestNode ok(ok_cfg);
    CHECK(ok.begin());
    CHECK(ok.configured());
    CHECK(ok.source_id() == kSenderId);
    CHECK(ok.session() == nullptr);

    TestConfig bad_id = base_config(0U, kSenderBoot, &sink);
    TestNode bad(bad_id);
    CHECK(!bad.begin());

    // A receive-only node may omit `send`: begin() still succeeds, but send()
    // then fails in place.
    TestConfig nosend_cfg = base_config(kSenderId, kSenderBoot, &sink);
    nosend_cfg.send_fn = nullptr;
    TestNode nosend(nosend_cfg);
    CHECK(nosend.begin());
    const std::uint8_t body[4] = {1, 2, 3, 4};
    CHECK(!nosend.send(MessageType::Telemetry, 0x0101U, body, sizeof(body), 0U));
}

// Built with a placeholder config (no identity, no send -- as if TxScheduler
// were not configured yet), then the SAME config object mutated in place
// with the real identity/send once it is known, THEN begin() -- the
// two-phase boot pattern Node's constructor comment describes: because the
// Node only holds a REFERENCE to this TestConfig, it always reads the
// current fields at each call, so there is no separate reconfigure() method
// any more -- just assign the fields, same as any other object. begin()
// before that mutation would fail here (source_id 0); after it,
// begin()/source_id()/send() all reflect the NEW fields.
void test_reconfigure_before_begin() {
    Sink placeholder_sink;
    TestConfig cfg{};
    cfg.transport = kEspNowTransport;
    cfg.send_fn = &Sink::send;
    cfg.send_ctx = &placeholder_sink;
    TestNode node(cfg);
    CHECK(!node.begin());  // source_id/boot_id still 0

    Sink real_sink;
    cfg.source_id = kSenderId;
    cfg.boot_id = kSenderBoot;
    cfg.send_ctx = &real_sink;
    CHECK(node.begin());
    CHECK(node.source_id() == kSenderId);
    CHECK(node.boot_id() == kSenderBoot);

    const std::uint8_t body[4] = {9, 8, 7, 6};
    CHECK(node.send(MessageType::Telemetry, 0x0101U, body, sizeof(body), 0U));
    CHECK(placeholder_sink.count() == 0U);  // never sent through the old ctx
    CHECK(real_sink.count() == 1U);
}

void test_cleartext_roundtrip() {
    Sink tx;
    TestConfig sender_cfg = base_config(kSenderId, kSenderBoot, &tx);
    TestNode sender(sender_cfg);
    Sink rx_out;
    TestConfig receiver_cfg = base_config(kPeerId, kPeerBoot, &rx_out);
    TestNode receiver(receiver_cfg);
    CHECK(sender.begin());
    CHECK(receiver.begin());

    const std::vector<std::uint8_t> payload = make_payload(40, 0x10);
    CHECK(sender.send(MessageType::Telemetry, 0x0101U, payload.data(),
                      payload.size(), 0x1122334455667788ULL));
    CHECK(tx.count() == 1U);

    ReceivedMessage msg{};
    CHECK(deliver(receiver, tx, 0U, &msg) == NodeRx::Complete);
    CHECK(msg.header.type == MessageType::Telemetry);
    CHECK(msg.header.source_id == kSenderId);
    CHECK(msg.header.object_id == 0x0101U);
    CHECK(!msg.reassembled);
    CHECK(msg.payload.size == payload.size());
    CHECK(std::memcmp(msg.payload.data, payload.data(), payload.size()) == 0);
}

void test_fragmented_roundtrip() {
    Sink tx;
    TestConfig sender_cfg = base_config(kSenderId, kSenderBoot, &tx);
    TestNode sender(sender_cfg);
    Sink rx_out;
    TestConfig receiver_cfg = base_config(kPeerId, kPeerBoot, &rx_out);
    TestNode receiver(receiver_cfg);
    sender.begin();
    receiver.begin();

    // 500 octets over the ESP-NOW 210-octet payload ceiling -> 3 frames.
    const std::vector<std::uint8_t> payload = make_payload(500, 0x33);
    CHECK(sender.send(MessageType::Command, 0x0001U, payload.data(),
                      payload.size(), 7ULL));
    CHECK(tx.count() == 3U);

    ReceivedMessage msg{};
    CHECK(receiver.receive(tx.frames[0].data(), tx.frames[0].size(), 0U, &msg) ==
          NodeRx::Pending);
    CHECK(receiver.receive(tx.frames[1].data(), tx.frames[1].size(), 0U, &msg) ==
          NodeRx::Pending);
    CHECK(receiver.receive(tx.frames[2].data(), tx.frames[2].size(), 0U, &msg) ==
          NodeRx::Complete);
    CHECK(msg.reassembled);
    CHECK(msg.payload.size == payload.size());
    CHECK(std::memcmp(msg.payload.data, payload.data(), payload.size()) == 0);
}

void test_sealed_roundtrip() {
    Sink tx;
    TestConfig sender_cfg = base_config(kSenderId, kSenderBoot, &tx);
    sender_cfg.seal_fn = &fake_seal;
    TestNode sender(sender_cfg);

    Sink rx_out;
    TestConfig receiver_cfg = base_config(kPeerId, kPeerBoot, &rx_out);
    receiver_cfg.open_fn = &fake_open;
    TestNode receiver(receiver_cfg);
    sender.begin();
    receiver.begin();

    const std::vector<std::uint8_t> payload = make_payload(48, 0x70);
    CHECK(sender.send(MessageType::Telemetry, 0x0101U, payload.data(),
                      payload.size(), 9ULL));
    CHECK(tx.count() == 1U);

    ReceivedMessage msg{};
    CHECK(deliver(receiver, tx, 0U, &msg) == NodeRx::Complete);
    // fake_open reversed the fake_seal: the caller sees the plaintext back.
    CHECK(msg.payload.size == payload.size());
    CHECK(std::memcmp(msg.payload.data, payload.data(), payload.size()) == 0);

    // A receiver with no open callback gets the sealed bytes verbatim.
    Sink other_out;
    TestConfig raw_cfg = base_config(0x00ABCDEFU, 0x00FEDCBAU, &other_out);
    TestNode raw(raw_cfg);
    raw.begin();
    ReceivedMessage sealed{};
    CHECK(deliver(raw, tx, 0U, &sealed) == NodeRx::Complete);
    CHECK(sealed.payload.size == payload.size() + btp::kEndpointAeadTagSize);
    CHECK(sealed.payload.data[0] == (payload[0] ^ 0x5AU));
}

void test_session_handshake() {
    Sink peer_tx;
    TestConfig peer_cfg = base_config(kPeerId, kPeerBoot, &peer_tx);
    TestNode peer(peer_cfg);
    peer.enable_session(make_hello(Role::Consumer), /*hello_deadline_ms=*/0U);
    CHECK(peer.begin());
    CHECK(peer.session() != nullptr);
    peer.arm_session(100U);

    // The initiator side: a bare endpoint puts a HELLO on the wire.
    const Hello remote = make_hello(Role::Producer);
    std::uint8_t hello_body[64];
    std::size_t hello_n = 0;
    CHECK(btp::encode_hello(remote, hello_body, sizeof(hello_body), &hello_n) ==
          btp::MessageError::Ok);

    Sink initiator_tx;
    btp::Endpoint initiator;
    initiator.configure(kSenderId, kSenderBoot);
    const btp::LogicalMessage hello_msg{MessageType::Control,
                                        btp::object_id::kHello, 1ULL,
                                        {hello_body, hello_n}};
    CHECK(initiator.send_logical(hello_msg, kEspNowTransport,
                                 &Sink::send, &initiator_tx, nullptr, 0U));
    CHECK(initiator_tx.count() == 1U);

    ReceivedMessage msg{};
    const NodeRx rx = peer.receive(initiator_tx.frames[0].data(),
                                   initiator_tx.frames[0].size(), 200U, &msg);
    CHECK(rx == NodeRx::SessionHandled);
    CHECK(peer.session_event() == SessionEvent::HelloAccepted);
    CHECK(peer.session()->active());

    // The peer answered with a HELLO_RESULT through its send callback.
    CHECK(peer_tx.count() == 1U);
    btp::DecodedFrame reply{};
    CHECK(btp::decode(peer_tx.frames[0].data(), peer_tx.frames[0].size(),
                      kEspNowTransport, &reply) == btp::Error::Ok);
    CHECK(reply.header.type == MessageType::Control);
    CHECK(reply.header.object_id == btp::object_id::kHelloResult);
    btp::HelloResult hr{};
    CHECK(btp::decode_hello_result(reply.payload.data, reply.payload.size,
                                   &hr) == btp::MessageError::Ok);
    CHECK(hr.status == static_cast<std::uint8_t>(btp::ResultStatus::Success));

    // Now an application frame from the same identity routes normally and
    // renews the watchdog.
    Sink app_tx;
    TestConfig sender_cfg = base_config(kSenderId, kSenderBoot, &app_tx);
    TestNode sender(sender_cfg);
    sender.begin();
    const std::vector<std::uint8_t> payload = make_payload(24, 0x01);
    sender.send(MessageType::Telemetry, 0x0101U, payload.data(), payload.size(),
                5ULL);
    ReceivedMessage app{};
    CHECK(deliver(peer, app_tx, 300U, &app) == NodeRx::Complete);
    CHECK(app.payload.size == payload.size());

    // That frame renewed the watchdog at now = 300; well within 30 s it is
    // still alive, and past it the watchdog fires once.
    CHECK(peer.tick(300U + 15000U) == SessionEvent::None);
    CHECK(peer.session()->active());
    CHECK(peer.tick(300U + 30000U + 1U) == SessionEvent::TimedOut);
    CHECK(!peer.session()->active());
    CHECK(peer.tick(300U + 40000U) == SessionEvent::None);  // reported once
}

void test_session_ignores_frame_before_hello() {
    Sink peer_tx;
    TestConfig peer_cfg = base_config(kPeerId, kPeerBoot, &peer_tx);
    TestNode peer(peer_cfg);
    peer.enable_session(make_hello(Role::Consumer), 0U);
    peer.begin();
    peer.arm_session(0U);

    Sink app_tx;
    TestConfig sender_cfg = base_config(kSenderId, kSenderBoot, &app_tx);
    TestNode sender(sender_cfg);
    sender.begin();
    const std::vector<std::uint8_t> payload = make_payload(16, 0x02);
    sender.send(MessageType::Telemetry, 0x0101U, payload.data(), payload.size(),
                1ULL);

    ReceivedMessage msg{};
    CHECK(deliver(peer, app_tx, 10U, &msg) == NodeRx::Ignored);
    CHECK(peer_tx.count() == 0U);
    CHECK(!peer.session()->active());
}

void test_stats() {
    Sink rx_out;
    TestConfig receiver_cfg = base_config(kPeerId, kPeerBoot, &rx_out);
    TestNode receiver(receiver_cfg);
    receiver.begin();

    // A too-short buffer is a decode failure.
    const std::uint8_t junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ReceivedMessage msg{};
    CHECK(receiver.receive(junk, sizeof(junk), 0U, &msg) == NodeRx::DroppedFrame);
    CHECK(receiver.stats().rx.dropped_decode >= 1U);

    Sink tx;
    TestConfig sender_cfg = base_config(kSenderId, kSenderBoot, &tx);
    TestNode sender(sender_cfg);
    sender.begin();
    const std::vector<std::uint8_t> payload = make_payload(20, 0x40);
    sender.send(MessageType::Telemetry, 0x0101U, payload.data(), payload.size(),
                1ULL);
    CHECK(deliver(receiver, tx, 0U, &msg) == NodeRx::Complete);
    CHECK(receiver.stats().rx.completed == 1U);
}

// --- discovery: consumer learns a schema from MANIFEST_DATA -----------------

const btp::FieldRecord kDriveFields[] = {
    btp::f32("left_rpm"),
    btp::u16("battery_v", 0.001),
    btp::nullable(btp::i16("temp_c", 0.1)),
};

// Serialise a one-topic catalogue into a MANIFEST_DATA payload.
std::size_t manifest_of(const btp::Catalog& cat, std::uint8_t* out,
                        std::size_t cap) {
    btp::ManifestHeader h = {};
    h.status = static_cast<std::uint8_t>(btp::ResultStatus::Success);
    h.manifest_format_version = 1U;
    h.config_revision = cat.config_revision();
    h.described_source_id = kSenderId;
    h.described_boot_id = kSenderBoot;
    h.source_role = static_cast<std::uint8_t>(Role::Producer);
    h.source_flags = btp::kSourceOnline;
    h.catalog_count = 1U;
    h.topic_count = static_cast<std::uint16_t>(cat.topic_count());
    h.source_name = btp::ByteView{reinterpret_cast<const std::uint8_t*>("robot"),
                                  5U};
    btp::ManifestWriter w(out, cap);
    if (w.begin(h) != btp::MessageError::Ok) return 0U;
    if (cat.write_topics(&w) != btp::MessageError::Ok) return 0U;
    std::size_t n = 0U;
    return w.finish(&n) == btp::MessageError::Ok ? n : 0U;
}

struct SampleCapture {
    int calls;
    double values[8];
    const char* names[8];
    int count;
};

void capture_sample(void* ctx, const btp::CatalogTopic& topic,
                    btp::SampleReader& reader) {
    SampleCapture* c = static_cast<SampleCapture*>(ctx);
    ++c->calls;
    c->count = 0;
    btp::SampleValue v = {};
    while (reader.next(&v) == btp::SampleStep::Item && c->count < 8) {
        c->values[c->count] = v.is_null ? -999.0 : v.f64(0);
        c->names[c->count] = topic.field_names != nullptr
                                 ? topic.field_names[v.field->order]
                                 : "";
        ++c->count;
    }
}

void test_catalog_discovery() {
    btp::StaticCatalog<> producer_cat;
    producer_cat.set_config_revision(9U);
    CHECK(producer_cat.add_topic(0x0101U, 2U, "drive_status", kDriveFields) ==
          btp::MessageError::Ok);

    std::uint8_t manifest[512];
    const std::size_t manifest_n =
        manifest_of(producer_cat, manifest, sizeof(manifest));
    CHECK(manifest_n != 0U);

    // A sender node just relays the raw MANIFEST_DATA / TELEMETRY frames.
    Sink tx;
    TestConfig sender_cfg = base_config(kSenderId, kSenderBoot, &tx);
    TestNode sender(sender_cfg);
    sender.begin();

    Sink rx_out;
    TestConfig consumer_cfg = base_config(kPeerId, kPeerBoot, &rx_out);
    TestNode consumer(consumer_cfg);
    btp::StaticCatalog<> learned;
    consumer.learn_catalog(&learned);
    SampleCapture capture = {};
    consumer.on_sample(&capture_sample, &capture);
    consumer.begin();

    // A TELEMETRY sample arriving before the manifest: no schema yet.
    std::uint8_t body[32];
    btp::SampleWriter w0(body, sizeof(body), producer_cat.topic(0x0101U)->fields,
                         3U);
    w0.begin(2U);
    w0.put_f64(1400.0);
    w0.put_f64(3.71);
    w0.put_null();
    std::size_t body_n = 0U;
    CHECK(w0.finish(&body_n) == btp::MessageError::Ok);

    tx.clear();
    sender.send(MessageType::Telemetry, 0x0101U, body, body_n, 1ULL);
    ReceivedMessage msg{};
    CHECK(deliver(consumer, tx, 0U, &msg) == NodeRx::Ignored);
    CHECK(capture.calls == 0);

    // The manifest: the consumer learns the schema.
    tx.clear();
    sender.send(MessageType::Control, btp::object_id::kManifestData, manifest,
                manifest_n, 2ULL);
    CHECK(deliver(consumer, tx, 0U, &msg) == NodeRx::CatalogUpdated);
    CHECK(learned.config_revision() == 9U);
    const btp::CatalogTopic* t = learned.topic(0x0101U);
    CHECK(t != nullptr);
    CHECK(t->field_count == 3U);
    CHECK(t->fields[1].scale == 0.001);

    // Now the same sample decodes against the learned schema.
    tx.clear();
    sender.send(MessageType::Telemetry, 0x0101U, body, body_n, 3ULL);
    CHECK(deliver(consumer, tx, 0U, &msg) == NodeRx::SampleDelivered);
    CHECK(capture.calls == 1);
    CHECK(capture.count == 3);
    CHECK(capture.values[0] == 1400.0);
    CHECK(capture.values[1] > 3.70 && capture.values[1] < 3.72);  // 3710 * 0.001
    CHECK(capture.values[2] == -999.0);                           // null temp_c
    CHECK(std::strcmp(capture.names[1], "battery_v") == 0);
}

void fill_drive(void* /*ctx*/, btp::SampleWriter& w) {
    w.put_f64(1450.0);
    w.put_f64(3.72);
    w.put_null();
}

void test_catalog_serve_and_publish() {
    const std::uint8_t uuid[16] = {1, 2, 3, 4, 5, 6, 7, 8,
                                   9, 10, 11, 12, 13, 14, 15, 16};

    // Producer: a served catalogue.
    Sink prod_tx;
    TestConfig producer_cfg = base_config(kSenderId, kSenderBoot, &prod_tx);
    TestNode producer(producer_cfg);
    btp::StaticCatalog<> served;
    served.set_config_revision(4U);
    CHECK(served.add_topic(0x0101U, 2U, "drive_status", kDriveFields) ==
          btp::MessageError::Ok);
    producer.serve_catalog(&served, static_cast<std::uint8_t>(Role::Producer),
                           uuid, "example-robot");
    producer.begin();

    // Consumer: a learn catalogue.
    Sink cons_tx;
    TestConfig consumer_cfg = base_config(kPeerId, kPeerBoot, &cons_tx);
    TestNode consumer(consumer_cfg);
    btp::StaticCatalog<> learned;
    consumer.learn_catalog(&learned);
    SampleCapture capture = {};
    consumer.on_sample(&capture_sample, &capture);
    consumer.begin();

    // Consumer asks; producer serves; consumer learns.
    CHECK(consumer.request_manifest(kSenderId, kSenderBoot, 0U));
    ReceivedMessage msg{};
    CHECK(deliver(producer, cons_tx, 0U, &msg) == NodeRx::RequestServed);
    CHECK(prod_tx.count() >= 1U);
    CHECK(deliver(consumer, prod_tx, 0U, &msg) == NodeRx::CatalogUpdated);
    CHECK(learned.topic(0x0101U) != nullptr);
    CHECK(learned.config_revision() == 4U);
    CHECK(std::strcmp(learned.field_name(*learned.topic(0x0101U), 1),
                      "battery_v") == 0);

    // Producer publishes a typed sample; consumer decodes it.
    prod_tx.clear();
    CHECK(producer.publish(0x0101U, &fill_drive, nullptr, 42ULL));
    CHECK(deliver(consumer, prod_tx, 0U, &msg) == NodeRx::SampleDelivered);
    CHECK(capture.calls == 1);
    CHECK(capture.values[0] == 1450.0);
    CHECK(capture.values[2] == -999.0);  // null

    // Publishing an unknown topic fails.
    CHECK(!producer.publish(0x0999U, &fill_drive, nullptr, 1ULL));

    // A second request carrying the current revision gets NOT_MODIFIED, which
    // ingest() treats as "keep what I have".
    cons_tx.clear();
    prod_tx.clear();
    CHECK(consumer.request_manifest(kSenderId, kSenderBoot, 4U));
    CHECK(deliver(producer, cons_tx, 0U, &msg) == NodeRx::RequestServed);
    CHECK(deliver(consumer, prod_tx, 0U, &msg) == NodeRx::CatalogUpdated);
    CHECK(learned.topic_count() == 1U);

    // Unsolicited announce also works.
    prod_tx.clear();
    CHECK(producer.announce_catalog());
    CHECK(prod_tx.count() >= 1U);
    btp::StaticCatalog<> late;
    Sink late_tx;
    TestConfig latecomer_cfg = base_config(0x00A1A1A1U, 0x00B2B2B2U, &late_tx);
    TestNode latecomer(latecomer_cfg);
    latecomer.learn_catalog(&late);
    latecomer.begin();
    CHECK(deliver(latecomer, prod_tx, 0U, &msg) == NodeRx::CatalogUpdated);
    CHECK(late.topic(0x0101U) != nullptr);
}

// --- consumer sends MANIFEST_REQUEST ---------------------------------------

void test_request_manifest() {
    Sink tx;
    TestConfig consumer_cfg = base_config(kPeerId, kPeerBoot, &tx);
    TestNode consumer(consumer_cfg);
    consumer.begin();
    CHECK(consumer.request_manifest(kSenderId, kSenderBoot, /*known_rev=*/0U));
    CHECK(tx.count() == 1U);

    btp::DecodedFrame frame{};
    CHECK(btp::decode(tx.frames[0].data(), tx.frames[0].size(),
                      kEspNowTransport, &frame) == btp::Error::Ok);
    CHECK(frame.header.type == MessageType::Control);
    CHECK(frame.header.object_id == btp::object_id::kManifestRequest);
    btp::ManifestRequest req{};
    CHECK(btp::decode_manifest_request(frame.payload.data, frame.payload.size,
                                       &req) == btp::MessageError::Ok);
    CHECK(req.target_source_id == kSenderId);
    CHECK(req.known_config_revision == 0U);
}

// ===========================================================================
// btp::Node -- the session initiator (connect())
// ===========================================================================

using btp::InitiatorEvent;

void test_connect_handshake_and_effective_limits() {
    Sink init_tx;
    TestConfig initiator_cfg = base_config(kSenderId, kSenderBoot, &init_tx);
    TestNode initiator(initiator_cfg);
    CHECK(initiator.begin());
    CHECK(!initiator.connected());

    CHECK(initiator.connect(make_hello(Role::Consumer), 0U,
                            /*deadline_ms=*/2000U));
    CHECK(init_tx.count() == 1U);

    Sink peer_tx;
    TestConfig peer_cfg = base_config(kPeerId, kPeerBoot, &peer_tx);
    TestNode peer(peer_cfg);
    peer.enable_session(make_hello(Role::Producer), 0U);
    CHECK(peer.begin());
    peer.arm_session(0U);

    ReceivedMessage msg{};
    CHECK(deliver(peer, init_tx, 1U, &msg) == NodeRx::SessionHandled);
    CHECK(peer.session()->active());
    CHECK(peer_tx.count() == 1U);  // the peer's HELLO_RESULT

    CHECK(deliver(initiator, peer_tx, 2U, &msg) == NodeRx::InitiatorHandled);
    CHECK(initiator.initiator_event() == InitiatorEvent::Connected);
    CHECK(initiator.connected());
    CHECK(initiator.connected_peer_source_id() == kPeerId);
    CHECK(initiator.connected_peer_boot_id() == kPeerBoot);
    CHECK(initiator.effective_limits().session_timeout_ms == 30000U);

    // An application frame from the peer now routes normally AND renews the
    // initiator's own watchdog.
    Sink app_tx;
    TestConfig app_cfg = base_config(kPeerId, kPeerBoot, &app_tx);
    TestNode app(app_cfg);
    app.begin();
    const std::vector<std::uint8_t> payload = make_payload(12, 0x07);
    app.send(MessageType::Telemetry, 0x0101U, payload.data(), payload.size(),
            9ULL);
    CHECK(deliver(initiator, app_tx, 500U, &msg) == NodeRx::Complete);
    CHECK(initiator.initiator_event() == InitiatorEvent::FrameAccepted);
    CHECK(initiator.connected());
}

void test_connect_times_out_without_a_reply() {
    Sink tx;
    TestConfig initiator_cfg = base_config(kSenderId, kSenderBoot, &tx);
    TestNode initiator(initiator_cfg);
    initiator.begin();
    CHECK(initiator.connect(make_hello(Role::Consumer), 0U,
                            /*deadline_ms=*/2000U));

    CHECK(initiator.tick(1999U) == SessionEvent::None);  // unrelated return type
    CHECK(initiator.initiator_event() == InitiatorEvent::None);
    CHECK(initiator.tick(2000U) == SessionEvent::None);
    CHECK(initiator.initiator_event() == InitiatorEvent::TimedOut);
    CHECK(!initiator.connected());
}

void test_connect_requires_send() {
    Sink tx;
    TestConfig cfg = base_config(kSenderId, kSenderBoot, &tx);
    cfg.send_fn = nullptr;
    TestNode initiator(cfg);
    initiator.begin();
    CHECK(!initiator.connect(make_hello(Role::Consumer), 0U, 2000U));
}

void test_disconnect_sends_session_close() {
    Sink init_tx;
    TestConfig initiator_cfg = base_config(kSenderId, kSenderBoot, &init_tx);
    TestNode initiator(initiator_cfg);
    initiator.begin();
    CHECK(initiator.connect(make_hello(Role::Consumer), 0U, 2000U));

    Sink peer_tx;
    TestConfig peer_cfg = base_config(kPeerId, kPeerBoot, &peer_tx);
    TestNode peer(peer_cfg);
    peer.enable_session(make_hello(Role::Producer), 0U);
    peer.begin();
    peer.arm_session(0U);
    ReceivedMessage msg{};
    deliver(peer, init_tx, 0U, &msg);
    CHECK(deliver(initiator, peer_tx, 0U, &msg) == NodeRx::InitiatorHandled);
    CHECK(initiator.connected());

    init_tx.clear();
    CHECK(initiator.disconnect(1U, static_cast<std::uint8_t>(1U), 100U));
    CHECK(!initiator.connected());
    CHECK(init_tx.count() == 1U);

    btp::DecodedFrame frame{};
    CHECK(btp::decode(init_tx.frames[0].data(), init_tx.frames[0].size(),
                      kEspNowTransport, &frame) == btp::Error::Ok);
    CHECK(frame.header.object_id == btp::object_id::kSessionClose);
}

// ===========================================================================
// btp::StaticNode -- its own catalogue (no separate btp::StaticCatalog)
// ===========================================================================

void test_static_node_owns_its_catalog() {
    Sink tx;
    TestConfig node_cfg = base_config(kSenderId, kSenderBoot, &tx);
    TestNode node(node_cfg);

    // TopicBuilder, straight off the node -- no local StaticCatalog to declare.
    CHECK(node.topic(0x0101U, 2U, "drive_status")
             .f32("left_rpm")
             .f32("right_rpm")
             .u16("battery_v", 0.001)
             .end() == MessageError::Ok);
    CHECK(node.catalog().topic_count() == 1U);

    node.serve_catalog(static_cast<std::uint8_t>(Role::Producer));
    CHECK(node.begin());
    CHECK(node.announce_catalog());
    CHECK(tx.count() >= 1U);

    btp::DecodedFrame frame{};
    CHECK(btp::decode(tx.frames[0].data(), tx.frames[0].size(),
                      kEspNowTransport, &frame) == btp::Error::Ok);
    CHECK(frame.header.object_id == btp::object_id::kManifestData);

    // The consumer side of the same sugar: learn_catalog() with no argument.
    Sink cons_tx;
    TestConfig consumer_cfg = base_config(kPeerId, kPeerBoot, &cons_tx);
    TestNode consumer(consumer_cfg);
    consumer.learn_catalog();
    consumer.begin();
    ReceivedMessage msg{};
    CHECK(deliver(consumer, tx, 0U, &msg) == NodeRx::CatalogUpdated);
    CHECK(consumer.catalog().topic(0x0101U) != nullptr);
}

// ===========================================================================
// btp::Node::publish_named -- SampleWriter, checked by field name
// ===========================================================================

void fill_drive_by_name(void* /*ctx*/, btp::NamedSampleWriter& w) {
    CHECK(w.put("left_rpm", 1450.0) == MessageError::Ok);
    CHECK(w.put("right_rpm", -1448.5) == MessageError::Ok);
    CHECK(w.put_null("battery_v") == MessageError::Ok);
}

void test_publish_named_round_trips() {
    Sink prod_tx;
    TestConfig producer_cfg = base_config(kSenderId, kSenderBoot, &prod_tx);
    TestNode producer(producer_cfg);
    CHECK(producer.topic(0x0101U, 2U, "drive_status")
             .f32("left_rpm")
             .f32("right_rpm")
             .u16("battery_v", 0.001, "", /*is_nullable=*/true)
             .end() == MessageError::Ok);
    producer.serve_catalog();
    producer.begin();

    CHECK(producer.publish_named(0x0101U, &fill_drive_by_name, nullptr, 3ULL));
    CHECK(prod_tx.count() == 1U);

    Sink cons_tx;
    TestConfig consumer_cfg = base_config(kPeerId, kPeerBoot, &cons_tx);
    TestNode consumer(consumer_cfg);
    consumer.learn_catalog();
    SampleCapture capture = {};
    consumer.on_sample(&capture_sample, &capture);
    consumer.begin();
    CHECK(consumer.request_manifest(kSenderId, kSenderBoot, 0U));
    ReceivedMessage msg{};
    CHECK(deliver(producer, cons_tx, 0U, &msg) == NodeRx::RequestServed);
    CHECK(deliver(consumer, prod_tx, 0U, &msg) == NodeRx::CatalogUpdated);

    prod_tx.clear();
    CHECK(producer.publish_named(0x0101U, &fill_drive_by_name, nullptr, 4ULL));
    CHECK(deliver(consumer, prod_tx, 0U, &msg) == NodeRx::SampleDelivered);
    CHECK(capture.calls == 1);
    CHECK(capture.values[0] == 1450.0);
    CHECK(capture.values[1] == -1448.5);
    CHECK(capture.values[2] == -999.0);  // null

    // Publishing with the wrong topic id fails, same as publish().
    CHECK(!producer.publish_named(0x0999U, &fill_drive_by_name, nullptr, 1ULL));
}

// publish_with()/publish_named_with() take the seal for THIS sample instead
// of cfg.seal -- checked the same way send_with()'s own contract reads: a
// null override forces cleartext regardless of cfg.seal, so the two sends
// below (same fill, same topic) differ by exactly kEndpointAeadTagSize on
// the wire -- proof the override, not cfg.seal, decided each one.
void test_publish_with_overrides_cfg_seal() {
    Sink prod_tx;
    TestConfig cfg = base_config(kSenderId, kSenderBoot, &prod_tx);
    cfg.seal_fn = &fake_seal;  // the node's default -- publish_with() bypasses it
    TestNode producer(cfg);
    CHECK(producer.topic(0x0101U, 2U, "drive_status")
             .f32("left_rpm")
             .f32("right_rpm")
             .u16("battery_v", 0.001, "", /*is_nullable=*/true)
             .end() == MessageError::Ok);
    producer.serve_catalog();
    CHECK(producer.begin());

    CHECK(producer.publish_named(0x0101U, &fill_drive_by_name, nullptr, 3ULL));
    CHECK(prod_tx.count() == 1U);
    const std::size_t sealed_size = prod_tx.frames[0].size();

    prod_tx.clear();
    CHECK(producer.publish_named_with(0x0101U, &fill_drive_by_name, nullptr,
                                      4ULL, /*seal=*/nullptr, nullptr));
    CHECK(prod_tx.count() == 1U);
    const std::size_t cleartext_size = prod_tx.frames[0].size();

    CHECK(sealed_size == cleartext_size + btp::kEndpointAeadTagSize);

    // publish() itself is unaffected -- still cfg.seal, same as before this.
    prod_tx.clear();
    CHECK(producer.publish(
        0x0101U,
        [](void*, btp::SampleWriter& w) {
            w.put_f64(1.0);
            w.put_f64(2.0);
            w.put_null();
        },
        nullptr, 5ULL));
    CHECK(prod_tx.count() == 1U);
    CHECK(prod_tx.frames[0].size() == sealed_size);
}

// ===========================================================================
// btp::Node -- subscriptions (btp::SubscriptionTable / btp::SubscriptionClient)
// ===========================================================================

using btp::ClientSubscription;
using btp::SubscriptionClient;
using btp::SubscriptionEvent;
using btp::SubscriptionRecord;
using btp::SubscriptionTable;

void test_subscribe_grant_publish_cadence_and_unsubscribe() {
    Sink prod_tx;
    TestConfig producer_cfg = base_config(kSenderId, kSenderBoot, &prod_tx);
    TestNode producer(producer_cfg);
    btp::StaticCatalog<> served;
    CHECK(served.add_topic(0x0101U, 2U, "drive_status", kDriveFields) ==
          btp::MessageError::Ok);
    producer.serve_catalog(&served, static_cast<std::uint8_t>(Role::Producer),
                           nullptr, "example-robot");
    SubscriptionRecord table_slots[4];
    SubscriptionTable table(table_slots, 4);
    producer.enable_subscriptions(&table);
    CHECK(producer.begin());

    Sink cons_tx;
    TestConfig consumer_cfg = base_config(kPeerId, kPeerBoot, &cons_tx);
    TestNode consumer(consumer_cfg);
    ClientSubscription client_slots[4];
    SubscriptionClient client(client_slots, 4);
    consumer.enable_subscription_client(&client);
    CHECK(consumer.begin());

    // Consumer subscribes at 10000 mHz (100 ms period), a 1000 ms lease.
    const std::uint32_t local_id =
        consumer.subscribe(kSenderId, kSenderBoot, 0x0101U, 10000U, 1000U);
    CHECK(local_id != 0U);
    CHECK(cons_tx.count() == 1U);

    ReceivedMessage msg{};
    CHECK(deliver(producer, cons_tx, 0U, &msg) == NodeRx::SubscriptionServed);
    CHECK(prod_tx.count() == 1U);  // SUBSCRIBE_RESULT
    CHECK(table.due(0x0101U, 0U));  // due right away, before any publish

    CHECK(consumer.subscription_event() == SubscriptionEvent::None);
    CHECK(deliver(consumer, prod_tx, 0U, &msg) == NodeRx::SubscriptionHandled);
    CHECK(consumer.subscription_event() == SubscriptionEvent::Granted);

    // Producer publishes once due(), then the cadence follows the granted rate.
    CHECK(producer.publish(0x0101U, &fill_drive, nullptr, 5ULL));
    table.note_published(0x0101U, 0U);
    CHECK(!table.due(0x0101U, 99U));
    CHECK(table.due(0x0101U, 100U));

    // Unsubscribe: the consumer sends UNSUBSCRIBE, the producer drops it.
    prod_tx.clear();
    cons_tx.clear();
    CHECK(consumer.unsubscribe(local_id));
    CHECK(cons_tx.count() == 1U);
    CHECK(deliver(producer, cons_tx, 200U, &msg) == NodeRx::SubscriptionServed);
    CHECK(!table.due(0x0101U, 300U));

    // The id is spent -- unsubscribing it again fails.
    CHECK(!consumer.unsubscribe(local_id));
}

// cfg.reply_seal, when set, picks the seal for one automatic reply from the
// ORIGINAL request's header -- the hub case docs/library.md notes as node.hpp's
// escape hatch: kAltPeerId gets fake_seal_b, anyone else (kPeerId here) falls
// back to fake_seal explicitly, the same "known channel vs. default" shape a
// dual-key responder needs. cfg.seal itself is left null, so a frame reaching
// the wire unsealed (the hook not firing at all) would fail_open's tag check
// below instead of silently passing.
void pick_seal_by_alt_requester(void*, const btp::Header& request,
                                btp::EndpointSealFn* out_seal,
                                void** out_seal_ctx) {
    *out_seal = (request.source_id == kAltPeerId) ? &fake_seal_b : &fake_seal;
    *out_seal_ctx = nullptr;
}

void test_reply_seal_hook_picks_seal_per_subscribe_requester() {
    Sink prod_tx;
    TestConfig cfg = base_config(kSenderId, kSenderBoot, &prod_tx);
    cfg.reply_seal_fn = &pick_seal_by_alt_requester;
    TestNode producer(cfg);
    btp::StaticCatalog<> served;
    CHECK(served.add_topic(0x0101U, 2U, "drive_status", kDriveFields) ==
          btp::MessageError::Ok);
    producer.serve_catalog(&served, static_cast<std::uint8_t>(Role::Producer),
                           nullptr, "example-robot");
    SubscriptionRecord table_slots[4];
    SubscriptionTable table(table_slots, 4);
    producer.enable_subscriptions(&table);
    CHECK(producer.begin());

    // kPeerId: not the alt requester -- falls back to fake_seal (the "A" tag).
    Sink cons_tx;
    TestConfig consumer_cfg = base_config(kPeerId, kPeerBoot, &cons_tx);
    TestNode consumer(consumer_cfg);
    ClientSubscription cons_slots[4];
    SubscriptionClient cons_client(cons_slots, 4);
    consumer.enable_subscription_client(&cons_client);
    CHECK(consumer.begin());
    consumer.subscribe(kSenderId, kSenderBoot, 0x0101U, 10000U, 1000U);
    CHECK(cons_tx.count() == 1U);
    ReceivedMessage msg{};
    CHECK(deliver(producer, cons_tx, 0U, &msg) == NodeRx::SubscriptionServed);
    CHECK(prod_tx.count() == 1U);
    CHECK(!frame_tagged_b(prod_tx.frames[0]));

    // kAltPeerId: the reply_seal callback recognises it -- fake_seal_b (the
    // "B" tag), a DIFFERENT key from the one above, for the exact same node.
    prod_tx.clear();
    Sink alt_tx;
    TestConfig alt_consumer_cfg = base_config(kAltPeerId, kAltPeerBoot, &alt_tx);
    TestNode alt_consumer(alt_consumer_cfg);
    ClientSubscription alt_slots[4];
    SubscriptionClient alt_client(alt_slots, 4);
    alt_consumer.enable_subscription_client(&alt_client);
    CHECK(alt_consumer.begin());
    alt_consumer.subscribe(kSenderId, kSenderBoot, 0x0101U, 10000U, 1000U);
    CHECK(alt_tx.count() == 1U);
    CHECK(deliver(producer, alt_tx, 0U, &msg) == NodeRx::SubscriptionServed);
    CHECK(prod_tx.count() == 1U);
    CHECK(frame_tagged_b(prod_tx.frames[0]));
}

// on_publish() + publish_subscribed_topics() together do what the test above
// does by hand (due() then publish_named() then note_published(), per topic)
// -- same producer/consumer setup, but the producer registers its fill once
// and the loop becomes a single call.
void test_publish_subscribed_topics_walks_registered_topics() {
    Sink prod_tx;
    TestConfig producer_cfg = base_config(kSenderId, kSenderBoot, &prod_tx);
    TestNode producer(producer_cfg);
    // Same schema fill_drive_by_name itself expects (left_rpm, right_rpm,
    // battery_v-nullable) -- not kDriveFields, which is a different one.
    btp::StaticCatalog<> served;
    CHECK(served.topic(0x0101U, 2U, "drive_status")
             .f32("left_rpm")
             .f32("right_rpm")
             .u16("battery_v", 0.001, "", /*is_nullable=*/true)
             .end() == btp::MessageError::Ok);
    producer.serve_catalog(&served, static_cast<std::uint8_t>(Role::Producer),
                           nullptr, "example-robot");
    SubscriptionRecord table_slots[4];
    SubscriptionTable table(table_slots, 4);
    producer.enable_subscriptions(&table);
    CHECK(producer.on_publish(0x0101U, &fill_drive_by_name, nullptr));
    CHECK(producer.begin());

    // Nothing subscribed yet -- nothing to walk.
    CHECK(producer.publish_subscribed_topics(0U) == 0U);
    CHECK(prod_tx.count() == 0U);

    Sink cons_tx;
    TestConfig consumer_cfg = base_config(kPeerId, kPeerBoot, &cons_tx);
    TestNode consumer(consumer_cfg);
    ClientSubscription client_slots[4];
    SubscriptionClient client(client_slots, 4);
    consumer.enable_subscription_client(&client);
    CHECK(consumer.begin());

    // 10000 mHz -> 100 ms period, a 1000 ms lease.
    consumer.subscribe(kSenderId, kSenderBoot, 0x0101U, 10000U, 1000U);
    CHECK(cons_tx.count() == 1U);
    ReceivedMessage msg{};
    CHECK(deliver(producer, cons_tx, 0U, &msg) == NodeRx::SubscriptionServed);

    // Due right away -- one call publishes the one registered, due topic.
    CHECK(producer.publish_subscribed_topics(0U) == 1U);
    CHECK(prod_tx.count() == 2U);  // SUBSCRIBE_RESULT, then this TELEMETRY

    // note_published() already ran inside that call -- not due again yet.
    CHECK(producer.publish_subscribed_topics(50U) == 0U);
    CHECK(prod_tx.count() == 2U);

    // ...and due again once the granted period has actually elapsed.
    CHECK(producer.publish_subscribed_topics(100U) == 1U);
    CHECK(prod_tx.count() == 3U);
}

// StaticNode<>::topic()'s `fill` argument on_publish()'s the topic right
// there, in the same statement as its schema -- no separate on_publish()
// call needed. Same scenario as the test above, but through that one call.
void test_topic_with_fill_registers_publish_in_one_call() {
    Sink prod_tx;
    TestConfig producer_cfg = base_config(kSenderId, kSenderBoot, &prod_tx);
    TestNode producer(producer_cfg);
    CHECK(producer.topic(0x0101U, 2U, "drive_status", &fill_drive_by_name)
             .f32("left_rpm")
             .f32("right_rpm")
             .u16("battery_v", 0.001, "", /*is_nullable=*/true)
             .end() == MessageError::Ok);
    producer.serve_catalog(static_cast<std::uint8_t>(Role::Producer));
    SubscriptionRecord table_slots[4];
    SubscriptionTable table(table_slots, 4);
    producer.enable_subscriptions(&table);
    CHECK(producer.begin());

    Sink cons_tx;
    TestConfig consumer_cfg = base_config(kPeerId, kPeerBoot, &cons_tx);
    TestNode consumer(consumer_cfg);
    ClientSubscription client_slots[4];
    SubscriptionClient client(client_slots, 4);
    consumer.enable_subscription_client(&client);
    CHECK(consumer.begin());

    consumer.subscribe(kSenderId, kSenderBoot, 0x0101U, 10000U, 1000U);
    ReceivedMessage msg{};
    CHECK(deliver(producer, cons_tx, 0U, &msg) == NodeRx::SubscriptionServed);

    // publish_subscribed_topics() already finds it -- topic() registered the
    // fill by itself, nothing else called on_publish() for this topic.
    CHECK(producer.publish_subscribed_topics(0U) == 1U);
    CHECK(prod_tx.count() == 2U);  // SUBSCRIBE_RESULT, then this TELEMETRY

    // The old 3-argument form (no fill) still declares a schema with nothing
    // registered for it -- the default does not silently opt every topic in.
    CHECK(producer.topic(0x0102U, 1U, "unfilled").f32("x").end() ==
          MessageError::Ok);
    CHECK(producer.catalog().topic_count() == 2U);
}

// StaticNode<> already has a SubscriptionTable ready -- no separate
// enable_subscriptions() call, the same "owns all of its storage" deal
// on_publish()/publish_subscribed_topics() already get.
void test_static_node_grants_subscriptions_with_no_setup_call() {
    Sink prod_tx;
    TestConfig producer_cfg = base_config(kSenderId, kSenderBoot, &prod_tx);
    TestNode producer(producer_cfg);
    CHECK(producer.topic(0x0101U, 2U, "drive_status", &fill_drive_by_name)
             .f32("left_rpm")
             .f32("right_rpm")
             .u16("battery_v", 0.001, "", /*is_nullable=*/true)
             .end() == MessageError::Ok);
    producer.serve_catalog(static_cast<std::uint8_t>(Role::Producer));
    // No producer.enable_subscriptions(...) here.
    CHECK(producer.begin());
    CHECK(producer.subscriptions() != nullptr);

    Sink cons_tx;
    TestConfig consumer_cfg = base_config(kPeerId, kPeerBoot, &cons_tx);
    TestNode consumer(consumer_cfg);
    // No consumer.enable_subscription_client(...) here either -- StaticNode<>
    // already has one of its own, same as the producer's subscriptions() above.
    CHECK(consumer.begin());

    consumer.subscribe(kSenderId, kSenderBoot, 0x0101U, 10000U, 1000U);
    ReceivedMessage msg{};
    CHECK(deliver(producer, cons_tx, 0U, &msg) == NodeRx::SubscriptionServed);
    CHECK(deliver(consumer, prod_tx, 0U, &msg) == NodeRx::SubscriptionHandled);
    CHECK(consumer.subscription_event() == SubscriptionEvent::Granted);

    CHECK(producer.publish_subscribed_topics(0U) == 1U);
}

// begin(/*arm_and_announce=*/true) arms any enabled session and sends
// announce_catalog() by itself -- begin() alone (the default) does neither,
// same as before this parameter existed.
void test_begin_can_arm_session_and_announce_catalog() {
    Sink tx;
    TestConfig plain_cfg = base_config(kSenderId, kSenderBoot, &tx);
    TestNode plain(plain_cfg);
    plain.enable_session(make_hello(Role::Producer), 0U);
    CHECK(plain.begin());  // default: neither armed nor announced
    CHECK(plain.session()->state() == btp::SessionState::Idle);
    CHECK(tx.count() == 0U);

    Sink tx2;
    TestConfig node_cfg = base_config(kSenderId, kSenderBoot, &tx2);
    TestNode node(node_cfg);
    CHECK(node.topic(0x0101U, 1U, "drive_status").f32("x").end() ==
          MessageError::Ok);
    node.serve_catalog(static_cast<std::uint8_t>(Role::Producer));
    node.enable_session(make_hello(Role::Producer), 0U);

    CHECK(node.begin(/*arm_and_announce=*/true));
    CHECK(node.session()->state() == btp::SessionState::AwaitingHello);
    CHECK(tx2.count() == 1U);  // the announce_catalog() MANIFEST_DATA

    btp::DecodedFrame frame{};
    CHECK(btp::decode(tx2.frames[0].data(), tx2.frames[0].size(),
                      kEspNowTransport, &frame) == btp::Error::Ok);
    CHECK(frame.header.object_id == btp::object_id::kManifestData);
}

// The INITIATOR's analogue: Node::begin(local_hello, deadline_ms) folds
// begin(/*arm_and_announce=*/false) + connect() into one call -- receiver.cpp
// no longer writes begin() then connect() as two statements.
void test_begin_with_hello_connects_out() {
    Sink tx;
    TestConfig initiator_cfg = base_config(kSenderId, kSenderBoot, &tx);
    TestNode initiator(initiator_cfg);
    CHECK(initiator.begin(make_hello(Role::Consumer), /*connect_deadline_ms=*/2000U));
    CHECK(tx.count() == 1U);  // the HELLO connect() sent
    CHECK(!initiator.connected());  // no HELLO_RESULT delivered yet
}

// StaticNode<> bundles enable_subscription_client() too, same "no setup call"
// treatment test_static_node_grants_subscriptions_with_no_setup_call above
// already gives the responder-side table.
void test_static_node_bundles_subscription_client() {
    Sink tx;
    TestConfig node_cfg = base_config(kSenderId, kSenderBoot, &tx);
    TestNode node(node_cfg);
    CHECK(node.begin());
    const std::uint32_t id =
        node.subscribe(kPeerId, kPeerBoot, 0x0101U, /*rate_millihz=*/1000U,
                       /*lease_ms=*/60000U);
    CHECK(id != 0U);
    CHECK(tx.count() == 1U);  // the SUBSCRIBE
}

// learn_catalog(sample, ctx) registers both the learn catalogue AND the
// sample callback in the one call -- example/receiver.cpp's analogue of
// topic(..., fill) on the producer side.
void test_static_node_learn_catalog_with_sample_registers_on_sample() {
    btp::StaticCatalog<> producer_cat;
    CHECK(producer_cat.add_topic(0x0101U, 2U, "drive_status", kDriveFields) ==
          btp::MessageError::Ok);
    std::uint8_t manifest[512];
    const std::size_t manifest_n =
        manifest_of(producer_cat, manifest, sizeof(manifest));
    CHECK(manifest_n != 0U);

    Sink tx;
    TestConfig sender_cfg = base_config(kSenderId, kSenderBoot, &tx);
    TestNode sender(sender_cfg);
    sender.begin();

    Sink rx_out;
    TestConfig consumer_cfg = base_config(kPeerId, kPeerBoot, &rx_out);
    TestNode consumer(consumer_cfg);
    SampleCapture capture = {};
    consumer.learn_catalog(&capture_sample, &capture);  // the one call under test
    CHECK(consumer.learned_catalog() == &consumer.catalog());
    consumer.begin();

    ReceivedMessage msg{};
    sender.send(MessageType::Control, btp::object_id::kManifestData, manifest,
                manifest_n, 1ULL);
    CHECK(deliver(consumer, tx, 0U, &msg) == NodeRx::CatalogUpdated);

    std::uint8_t body[32];
    btp::SampleWriter w(body, sizeof(body), producer_cat.topic(0x0101U)->fields, 3U);
    w.begin(2U);
    w.put_f64(1400.0);
    w.put_f64(3.71);
    w.put_null();
    std::size_t body_n = 0U;
    CHECK(w.finish(&body_n) == btp::MessageError::Ok);
    sender.send(MessageType::Telemetry, 0x0101U, body, body_n, 2ULL);
    CHECK(deliver(consumer, tx, 0U, &msg) == NodeRx::SampleDelivered);
    CHECK(capture.calls == 1);
}

// routine(now_ms) == publish_subscribed_topics(now_ms) + tick(now_ms) in one
// call -- what example/sender.cpp's and example/receiver.cpp's loops now
// share verbatim, instead of a different tail per role.
void test_routine_publishes_and_ticks() {
    // The publish half: a producer's routine() sends a due topic exactly
    // like publish_subscribed_topics() alone would.
    Sink prod_tx;
    TestConfig producer_cfg = base_config(kSenderId, kSenderBoot, &prod_tx);
    TestNode producer(producer_cfg);
    CHECK(producer.topic(0x0101U, 2U, "drive_status", &fill_drive_by_name)
             .f32("left_rpm")
             .f32("right_rpm")
             .u16("battery_v", 0.001, "", /*is_nullable=*/true)
             .end() == MessageError::Ok);
    producer.serve_catalog(static_cast<std::uint8_t>(Role::Producer));
    CHECK(producer.begin());

    Sink cons_tx;
    TestConfig consumer_cfg = base_config(kPeerId, kPeerBoot, &cons_tx);
    TestNode consumer(consumer_cfg);
    CHECK(consumer.begin());
    consumer.subscribe(kSenderId, kSenderBoot, 0x0101U, 10000U, 1000U);
    ReceivedMessage msg{};
    CHECK(deliver(producer, cons_tx, 0U, &msg) == NodeRx::SubscriptionServed);

    prod_tx.clear();
    producer.routine(0U);
    CHECK(prod_tx.count() == 1U);

    // The tick half: an initiator's routine() sweeps the connection watchdog
    // exactly like tick() alone would (test_connect_times_out_without_a_reply).
    Sink init_tx;
    TestConfig initiator_cfg = base_config(kSenderId, kSenderBoot, &init_tx);
    TestNode initiator(initiator_cfg);
    CHECK(initiator.begin(make_hello(Role::Consumer),
                          /*connect_deadline_ms=*/2000U));
    initiator.routine(1999U);
    CHECK(initiator.initiator_event() == InitiatorEvent::None);
    initiator.routine(2000U);
    CHECK(initiator.initiator_event() == InitiatorEvent::TimedOut);
}

// routine(datagram, size, now_ms, out): size == 0 skips receive() and
// returns NodeRx::NoDatagram (the housekeeping still runs); size != 0 IS
// receive(), same return value, with the housekeeping right after either
// way -- example/sender.cpp's and example/receiver.cpp's loops no longer
// branch on "did a datagram arrive" at all.
void test_routine_with_datagram_decodes_and_still_runs_housekeeping() {
    Sink prod_tx;
    TestConfig producer_cfg = base_config(kSenderId, kSenderBoot, &prod_tx);
    TestNode producer(producer_cfg);
    CHECK(producer.topic(0x0101U, 2U, "drive_status", &fill_drive_by_name)
             .f32("left_rpm")
             .f32("right_rpm")
             .u16("battery_v", 0.001, "", /*is_nullable=*/true)
             .end() == MessageError::Ok);
    producer.serve_catalog(static_cast<std::uint8_t>(Role::Producer));
    CHECK(producer.begin());

    Sink cons_tx;
    TestConfig consumer_cfg = base_config(kPeerId, kPeerBoot, &cons_tx);
    TestNode consumer(consumer_cfg);
    CHECK(consumer.begin());
    consumer.subscribe(kSenderId, kSenderBoot, 0x0101U, 10000U, 1000U);
    ReceivedMessage msg{};
    CHECK(deliver(producer, cons_tx, 0U, &msg) == NodeRx::SubscriptionServed);

    // No datagram this pass -- receive() does not run, but the due topic
    // still gets published.
    prod_tx.clear();
    CHECK(producer.routine(nullptr, 0U, 0U, &msg) == NodeRx::NoDatagram);
    CHECK(prod_tx.count() == 1U);

    // A real datagram this pass -- routine() decodes it (no learn_catalog()
    // here, so the raw TELEMETRY comes back Complete, same as receive()
    // alone would) AND still ran the housekeeping right after. It went out
    // on prod_tx -- the producer's own send() -- checked above.
    CHECK(consumer.routine(prod_tx.frames[0].data(), prod_tx.frames[0].size(),
                           0U, &msg) == NodeRx::Complete);
}

// routine(datagram, size, now_ms) -- no *out -- is the four-argument
// overload above with a throwaway ReceivedMessage: same NodeRx back, same
// housekeeping, for a caller with nothing to read from *out (everything it
// cares about already ran through a callback).
void test_routine_without_out_param_behaves_the_same() {
    Sink prod_tx;
    TestConfig producer_cfg = base_config(kSenderId, kSenderBoot, &prod_tx);
    TestNode producer(producer_cfg);
    CHECK(producer.topic(0x0101U, 2U, "drive_status", &fill_drive_by_name)
             .f32("left_rpm")
             .f32("right_rpm")
             .u16("battery_v", 0.001, "", /*is_nullable=*/true)
             .end() == MessageError::Ok);
    producer.serve_catalog(static_cast<std::uint8_t>(Role::Producer));
    CHECK(producer.begin());

    Sink cons_tx;
    TestConfig consumer_cfg = base_config(kPeerId, kPeerBoot, &cons_tx);
    TestNode consumer(consumer_cfg);
    CHECK(consumer.begin());
    consumer.subscribe(kSenderId, kSenderBoot, 0x0101U, 10000U, 1000U);
    ReceivedMessage msg{};
    CHECK(deliver(producer, cons_tx, 0U, &msg) == NodeRx::SubscriptionServed);

    // No datagram, no *out -- still publishes the due topic.
    prod_tx.clear();
    CHECK(producer.routine(nullptr, 0U, 0U) == NodeRx::NoDatagram);
    CHECK(prod_tx.count() == 1U);

    // A real datagram, no *out -- still decodes it (Complete, same reason
    // as the four-argument test above) and still ran the housekeeping.
    CHECK(consumer.routine(prod_tx.frames[0].data(), prod_tx.frames[0].size(),
                           0U) == NodeRx::Complete);
}

// StaticNode<>'s begin(source_name, hello, ...) overload folds
// catalog().set_config_revision() + serve_catalog() + enable_session() +
// begin(true) into one call -- what example/sender.cpp's old
// configure_producer() did by hand.
void test_static_node_begin_folds_catalog_and_session_setup() {
    Sink tx;
    TestConfig node_cfg = base_config(kSenderId, kSenderBoot, &tx);
    TestNode node(node_cfg);
    CHECK(node.topic(0x0101U, 1U, "drive_status").f32("x").end() ==
          MessageError::Ok);

    const Hello hello = make_hello(Role::Producer);
    CHECK(node.begin("test-source", hello));

    // config_revision defaulted, role came from hello.role, not repeated.
    CHECK(node.catalog().config_revision() == 1U);
    CHECK(node.session()->state() == btp::SessionState::AwaitingHello);
    CHECK(tx.count() == 1U);  // the announce_catalog() MANIFEST_DATA

    btp::DecodedFrame frame{};
    CHECK(btp::decode(tx.frames[0].data(), tx.frames[0].size(),
                      kEspNowTransport, &frame) == btp::Error::Ok);
    CHECK(frame.header.object_id == btp::object_id::kManifestData);
}

// ===========================================================================
// btp::Node -- commands (btp::DedupCache / btp::CommandClient)
// ===========================================================================

using btp::ClientCommand;
using btp::CommandClient;
using btp::CommandEvent;
using btp::DedupCache;
using btp::DedupRequester;
using btp::DedupSlot;
using btp::DedupStorage;
using btp::NodeActionOutcome;

struct ActionCalls {
    int calls;
    std::uint16_t last_action_id;
};

void echo_action(void* ctx, std::uint16_t action_id, std::uint16_t /*action_version*/,
                 btp::ByteView parameters, NodeActionOutcome* outcome) {
    ActionCalls* c = static_cast<ActionCalls*>(ctx);
    ++c->calls;
    c->last_action_id = action_id;
    outcome->status = static_cast<std::uint8_t>(btp::ResultStatus::Success);
    outcome->result_data = parameters.data;  // echo the parameters back
    outcome->result_size = parameters.size;
}

void test_command_round_trip_and_dedup_replay() {
    Sink resp_tx;
    TestConfig responder_cfg = base_config(kSenderId, kSenderBoot, &resp_tx);
    TestNode responder(responder_cfg);
    DedupSlot dedup_slots[4];
    std::uint8_t dedup_bytes[4][64];
    DedupStorage dedup_storage[4];
    for (std::size_t i = 0; i < 4; ++i) dedup_storage[i] = {dedup_bytes[i], 64};
    DedupRequester dedup_requesters[2];
    DedupCache dedup(dedup_slots, dedup_storage, 4, dedup_requesters, 2);
    ActionCalls calls = {};
    responder.enable_commands(&dedup, &echo_action, &calls);
    CHECK(responder.begin());

    Sink init_tx;
    TestConfig initiator_cfg = base_config(kPeerId, kPeerBoot, &init_tx);
    TestNode initiator(initiator_cfg);
    ClientCommand cmd_slots[2];
    CommandClient client(cmd_slots, 2);
    initiator.enable_command_client(&client);
    CHECK(initiator.begin());

    const std::uint8_t params[3] = {1, 2, 3};
    const std::uint32_t local_id =
        initiator.command(kSenderId, kSenderBoot, 42U, 1U, params, sizeof(params));
    CHECK(local_id != 0U);
    CHECK(init_tx.count() == 1U);

    ReceivedMessage msg{};
    CHECK(deliver(responder, init_tx, 0U, &msg) == NodeRx::CommandServed);
    CHECK(calls.calls == 1);
    CHECK(calls.last_action_id == 42U);
    CHECK(resp_tx.count() == 1U);

    CHECK(deliver(initiator, resp_tx, 0U, &msg) == NodeRx::CommandHandled);
    CHECK(initiator.command_outcome().event == CommandEvent::Completed);
    CHECK(initiator.command_outcome().local_id == local_id);
    CHECK(initiator.command_outcome().status ==
          static_cast<std::uint8_t>(btp::ResultStatus::Success));
    CHECK(initiator.command_outcome().result.size == sizeof(params));

    // A RETRANSMISSION of the exact same request frame (same source / boot /
    // sequence) replays the stored result -- the action does NOT run again.
    resp_tx.clear();
    CHECK(deliver(responder, init_tx, 100U, &msg) == NodeRx::CommandServed);
    CHECK(calls.calls == 1);  // still 1
    CHECK(resp_tx.count() == 1U);
}

// Same cfg.reply_seal mechanism as the subscribe test above, exercised on
// BOTH command reply paths: a Fresh COMMAND_REQUEST (emit_command_result)
// and a retransmission's DuplicateComplete replay -- reply_seal_for() is
// called separately by each, and both must still pick by the ORIGINAL
// request's source_id, not by which code path answered it.
void test_reply_seal_hook_picks_seal_per_command_requester() {
    Sink resp_tx;
    TestConfig cfg = base_config(kSenderId, kSenderBoot, &resp_tx);
    cfg.reply_seal_fn = &pick_seal_by_alt_requester;
    TestNode responder(cfg);
    DedupSlot dedup_slots[4];
    std::uint8_t dedup_bytes[4][64];
    DedupStorage dedup_storage[4];
    for (std::size_t i = 0; i < 4; ++i) dedup_storage[i] = {dedup_bytes[i], 64};
    DedupRequester dedup_requesters[2];
    DedupCache dedup(dedup_slots, dedup_storage, 4, dedup_requesters, 2);
    ActionCalls calls = {};
    responder.enable_commands(&dedup, &echo_action, &calls);
    CHECK(responder.begin());

    // kPeerId: falls back to fake_seal (the "A" tag) on the Fresh reply...
    Sink init_tx;
    TestConfig initiator_cfg = base_config(kPeerId, kPeerBoot, &init_tx);
    TestNode initiator(initiator_cfg);
    ClientCommand cmd_slots[2];
    CommandClient client(cmd_slots, 2);
    initiator.enable_command_client(&client);
    CHECK(initiator.begin());
    const std::uint8_t params[3] = {1, 2, 3};
    initiator.command(kSenderId, kSenderBoot, 42U, 1U, params, sizeof(params));
    CHECK(init_tx.count() == 1U);
    ReceivedMessage msg{};
    CHECK(deliver(responder, init_tx, 0U, &msg) == NodeRx::CommandServed);
    CHECK(resp_tx.count() == 1U);
    CHECK(!frame_tagged_b(resp_tx.frames[0]));

    // ...and still the "A" tag on the DuplicateComplete replay of the exact
    // same retransmitted request.
    resp_tx.clear();
    CHECK(deliver(responder, init_tx, 100U, &msg) == NodeRx::CommandServed);
    CHECK(calls.calls == 1);  // replayed, not re-run
    CHECK(resp_tx.count() == 1U);
    CHECK(!frame_tagged_b(resp_tx.frames[0]));

    // kAltPeerId: a DIFFERENT requester, on a Fresh request of its own --
    // the "B" tag, same responder node, same enable_commands() wiring.
    Sink alt_tx;
    TestConfig alt_initiator_cfg = base_config(kAltPeerId, kAltPeerBoot, &alt_tx);
    TestNode alt_initiator(alt_initiator_cfg);
    ClientCommand alt_cmd_slots[2];
    CommandClient alt_client(alt_cmd_slots, 2);
    alt_initiator.enable_command_client(&alt_client);
    CHECK(alt_initiator.begin());
    alt_initiator.command(kSenderId, kSenderBoot, 43U, 1U, params, sizeof(params));
    CHECK(alt_tx.count() == 1U);
    resp_tx.clear();
    CHECK(deliver(responder, alt_tx, 200U, &msg) == NodeRx::CommandServed);
    CHECK(calls.calls == 2);
    CHECK(resp_tx.count() == 1U);
    CHECK(frame_tagged_b(resp_tx.frames[0]));
}

void test_command_times_out_without_a_reply() {
    Sink tx;
    TestConfig initiator_cfg = base_config(kSenderId, kSenderBoot, &tx);
    TestNode initiator(initiator_cfg);
    ClientCommand slots[2];
    CommandClient client(slots, 2);
    initiator.enable_command_client(&client);
    initiator.begin();

    const std::uint8_t params[1] = {0};
    CHECK(initiator.command(kPeerId, kPeerBoot, 1U, 1U, params, sizeof(params)) != 0U);

    initiator.tick(btp::kCommandTimeoutMs - 1U);
    CHECK(initiator.command_outcome().event == CommandEvent::None);
    initiator.tick(btp::kCommandTimeoutMs);
    CHECK(initiator.command_outcome().event == CommandEvent::TimedOut);
}

// ===========================================================================
// btp::Node -- terminal
// ===========================================================================

struct TerminalCapture {
    int calls;
    std::uint16_t last_object_id;
    std::uint8_t first_byte;
};

void capture_terminal(void* ctx, btp::Node& /*node*/, const btp::Header& header,
                      btp::ByteView payload, std::uint64_t /*now_ms*/) {
    TerminalCapture* c = static_cast<TerminalCapture*>(ctx);
    ++c->calls;
    c->last_object_id = header.object_id;
    c->first_byte = payload.size != 0U ? payload.data[0] : 0U;
}

// Without on_terminal(), a TERMINAL frame falls through to Complete, same as
// any type the node does not manage; with one attached, receive() calls it
// and reports NodeRx::TerminalDelivered instead.
void test_on_terminal_delivers_and_falls_back_to_complete() {
    Sink src_tx;
    TestConfig source_cfg = base_config(kSenderId, kSenderBoot, &src_tx);
    TestNode source(source_cfg);
    CHECK(source.begin());
    const std::uint8_t bytes[] = {'h', 'i'};
    CHECK(source.send(MessageType::Terminal, btp::object_id::kTerminalIn, bytes,
                      sizeof(bytes), 0ULL));

    Sink dst_tx;
    TestConfig plain_cfg = base_config(kPeerId, kPeerBoot, &dst_tx);
    TestNode plain(plain_cfg);
    CHECK(plain.begin());
    ReceivedMessage msg{};
    CHECK(deliver(plain, src_tx, 0U, &msg) == NodeRx::Complete);
    CHECK(msg.header.object_id == btp::object_id::kTerminalIn);

    TerminalCapture capture = {};
    plain.on_terminal(&capture_terminal, &capture);
    src_tx.clear();
    CHECK(source.send(MessageType::Terminal, btp::object_id::kTerminalIn, bytes,
                      sizeof(bytes), 0ULL));
    CHECK(deliver(plain, src_tx, 0U, &msg) == NodeRx::TerminalDelivered);
    CHECK(capture.calls == 1);
    CHECK(capture.last_object_id == btp::object_id::kTerminalIn);
    CHECK(capture.first_byte == 'h');
}

// cfg.terminal / cfg.terminal_ctx wire on_terminal() from construction --
// no separate on_terminal() call needed, same round trip as the test above.
void test_node_config_wires_on_terminal() {
    Sink src_tx;
    TestConfig source_cfg = base_config(kSenderId, kSenderBoot, &src_tx);
    TestNode source(source_cfg);
    CHECK(source.begin());
    const std::uint8_t bytes[] = {'h', 'i'};
    CHECK(source.send(MessageType::Terminal, btp::object_id::kTerminalIn, bytes,
                      sizeof(bytes), 0ULL));

    Sink dst_tx;
    TestConfig cfg = base_config(kPeerId, kPeerBoot, &dst_tx);
    TerminalCapture capture = {};
    cfg.terminal_fn = &capture_terminal;
    cfg.terminal_ctx = &capture;
    TestNode node(cfg);
    CHECK(node.begin());

    ReceivedMessage msg{};
    CHECK(deliver(node, src_tx, 0U, &msg) == NodeRx::TerminalDelivered);
    CHECK(capture.calls == 1);
}

// StaticNode<> bundles the command responder AND initiator too -- the
// responder side still needs enable_commands(handler, ctx), the DedupCache
// itself is already there; the initiator side needs no call at all, same
// treatment as subscriptions (test_static_node_bundles_subscription_client
// above).
void test_static_node_bundles_commands() {
    Sink prod_tx;
    TestConfig producer_cfg = base_config(kSenderId, kSenderBoot, &prod_tx);
    TestNode producer(producer_cfg);
    ActionCalls calls = {};
    producer.enable_commands(&echo_action, &calls);  // no DedupCache to pass
    CHECK(producer.begin());

    Sink cons_tx;
    TestConfig consumer_cfg = base_config(kPeerId, kPeerBoot, &cons_tx);
    TestNode consumer(consumer_cfg);
    // No consumer.enable_command_client(...) here -- StaticNode<> already
    // has one of its own.
    CHECK(consumer.begin());

    const std::uint8_t params[3] = {1, 2, 3};
    const std::uint32_t local_id =
        consumer.command(kSenderId, kSenderBoot, 42U, 1U, params, sizeof(params));
    CHECK(local_id != 0U);

    ReceivedMessage msg{};
    CHECK(deliver(producer, cons_tx, 0U, &msg) == NodeRx::CommandServed);
    CHECK(calls.calls == 1);
    CHECK(deliver(consumer, prod_tx, 0U, &msg) == NodeRx::CommandHandled);
    CHECK(consumer.command_outcome().event == CommandEvent::Completed);
}

// cfg.command / cfg.command_ctx wire the responder from construction, same
// as cfg.terminal above -- no separate enable_commands() call needed. Only
// takes effect on a StaticNode<> (it owns the DedupCache this binds to).
void test_static_node_config_wires_commands() {
    Sink prod_tx;
    ActionCalls calls = {};
    TestConfig cfg = base_config(kSenderId, kSenderBoot, &prod_tx);
    cfg.command_fn = &echo_action;
    cfg.command_ctx = &calls;
    TestNode producer(cfg);
    CHECK(producer.begin());

    Sink cons_tx;
    TestConfig consumer_cfg = base_config(kPeerId, kPeerBoot, &cons_tx);
    TestNode consumer(consumer_cfg);
    CHECK(consumer.begin());

    const std::uint8_t params[2] = {9, 8};
    CHECK(consumer.command(kSenderId, kSenderBoot, 7U, 1U, params,
                           sizeof(params)) != 0U);

    ReceivedMessage msg{};
    CHECK(deliver(producer, cons_tx, 0U, &msg) == NodeRx::CommandServed);
    CHECK(calls.calls == 1);
}

// ===========================================================================
// btp::Node -- STATUS
// ===========================================================================

void test_status_disabled_by_default() {
    Sink tx;
    TestConfig node_cfg = base_config(kSenderId, kSenderBoot, &tx);
    TestNode node(node_cfg);
    node.begin();
    CHECK(!node.status_enabled());
    node.tick(1000000U);
    CHECK(tx.count() == 0U);
}

void test_status_sends_after_the_period_and_counts_frames_tx() {
    Sink tx;
    TestConfig node_cfg = base_config(kSenderId, kSenderBoot, &tx);
    TestNode node(node_cfg);
    node.begin();
    node.enable_status(500U);
    CHECK(node.status_enabled());
    CHECK(node.frames_tx() == 0U);

    const std::vector<std::uint8_t> payload = make_payload(8, 0x01);
    CHECK(node.send(MessageType::Telemetry, 0x0101U, payload.data(), payload.size(),
                    1ULL));
    CHECK(node.frames_tx() == 1U);

    node.tick(499U);
    CHECK(tx.count() == 1U);  // just the telemetry frame -- not due yet
    node.tick(500U);
    CHECK(tx.count() == 2U);  // STATUS went out
    CHECK(node.frames_tx() == 2U);  // and counted itself, after being built

    btp::DecodedFrame frame{};
    CHECK(btp::decode(tx.frames[1].data(), tx.frames[1].size(),
                      kEspNowTransport, &frame) == btp::Error::Ok);
    CHECK(frame.header.type == MessageType::Control);
    CHECK(frame.header.object_id == btp::object_id::kStatus);

    btp::StatusV1 status = {};
    std::size_t topics_written = 0U;
    CHECK(btp::decode_status(frame.payload.data, frame.payload.size, &status, nullptr,
                             0U, &topics_written) == btp::MessageError::Ok);
    CHECK(status.status_version == 1U);
    // Read BEFORE this STATUS's own send incremented the counter.
    CHECK(status.frames_tx == 1U);
}

void test_subscribe_renews_before_the_lease_runs_out() {
    Sink prod_tx;
    TestConfig producer_cfg = base_config(kSenderId, kSenderBoot, &prod_tx);
    TestNode producer(producer_cfg);
    btp::StaticCatalog<> served;
    served.add_topic(0x0101U, 2U, "drive_status", kDriveFields);
    producer.serve_catalog(&served, static_cast<std::uint8_t>(Role::Producer),
                           nullptr, "example-robot");
    SubscriptionRecord table_slots[4];
    SubscriptionTable table(table_slots, 4);
    producer.enable_subscriptions(&table);
    producer.begin();

    Sink cons_tx;
    TestConfig consumer_cfg = base_config(kPeerId, kPeerBoot, &cons_tx);
    TestNode consumer(consumer_cfg);
    ClientSubscription client_slots[4];
    SubscriptionClient client(client_slots, 4);
    consumer.enable_subscription_client(&client);
    consumer.begin();

    // A 1000 ms lease -- the renewal margin is 20%, so due at t=800.
    CHECK(consumer.subscribe(kSenderId, kSenderBoot, 0x0101U, 10000U, 1000U) != 0U);
    ReceivedMessage msg{};
    deliver(producer, cons_tx, 0U, &msg);
    deliver(consumer, prod_tx, 0U, &msg);
    CHECK(consumer.subscription_event() == SubscriptionEvent::Granted);

    cons_tx.clear();
    consumer.tick(799U);
    CHECK(cons_tx.count() == 0U);  // not due yet
    consumer.tick(800U);
    CHECK(cons_tx.count() == 1U);  // tick() renewed on its own -- a fresh SUBSCRIBE

    prod_tx.clear();
    CHECK(deliver(producer, cons_tx, 800U, &msg) == NodeRx::SubscriptionServed);
    CHECK(deliver(consumer, prod_tx, 800U, &msg) == NodeRx::SubscriptionHandled);
    CHECK(consumer.subscription_event() == SubscriptionEvent::Granted);  // still alive
}

// btp::SizedNode<NodeSize::Medium> must cost exactly what btp::StaticNode<>
// itself costs -- the whole point of Medium mirroring StaticNode<>'s own
// bare defaults is that neither is more or less "the real one".
void test_sized_node_medium_matches_static_node_defaults() {
    static_assert(sizeof(btp::SizedNode<btp::NodeSize::Medium>) ==
                      sizeof(btp::StaticNode<>),
                  "NodeSize::Medium must match StaticNode<>'s own defaults");
}

// Each tier actually builds and runs the same producer shape end to end --
// not just "it compiles", the smallest (Low) topic/subscribe/publish round
// trip too, since that is the tier most likely to be sized too tight for
// something this test would otherwise miss silently.
void test_sized_node_low_round_trips_a_topic() {
    Sink prod_tx;
    TestConfig producer_cfg = base_config(kSenderId, kSenderBoot, &prod_tx);
    btp::SizedNode<btp::NodeSize::Low> producer(producer_cfg);
    CHECK(producer.topic(0x0101U, 1U, "x", &fill_drive_by_name)
             .f32("left_rpm")
             .f32("right_rpm")
             .u16("battery_v", 0.001, "", /*is_nullable=*/true)
             .end() == MessageError::Ok);
    producer.serve_catalog(static_cast<std::uint8_t>(Role::Producer));
    CHECK(producer.begin());

    Sink cons_tx;
    TestConfig consumer_cfg = base_config(kPeerId, kPeerBoot, &cons_tx);
    btp::SizedNode<btp::NodeSize::Low> consumer(consumer_cfg);
    CHECK(consumer.begin());
    consumer.subscribe(kSenderId, kSenderBoot, 0x0101U, 10000U, 1000U);
    ReceivedMessage msg{};
    // deliver() is typed for TestNode specifically -- inline the same
    // one-frame delivery here rather than templating it for one test.
    NodeRx outcome = NodeRx::Pending;
    for (std::size_t i = 0; i < cons_tx.frames.size(); ++i) {
        outcome = producer.receive(cons_tx.frames[i].data(),
                                   cons_tx.frames[i].size(), 0U, &msg);
    }
    CHECK(outcome == NodeRx::SubscriptionServed);

    prod_tx.clear();  // drop the SUBSCRIBE_RESULT reply just sent above
    CHECK(producer.publish_subscribed_topics(0U) == 1U);
    CHECK(prod_tx.count() == 1U);
}

// High just needs to actually build and begin() -- its whole point is
// capacity headroom (many topics, many subscribers), not a different
// code path from the other two tiers.
void test_sized_node_high_begins() {
    Sink tx;
    TestConfig node_cfg = base_config(kSenderId, kSenderBoot, &tx);
    btp::SizedNode<btp::NodeSize::High> node(node_cfg);
    CHECK(node.begin());
}

}  // namespace

int main() {
    test_begin();
    test_reconfigure_before_begin();
    test_cleartext_roundtrip();
    test_fragmented_roundtrip();
    test_sealed_roundtrip();
    test_session_handshake();
    test_session_ignores_frame_before_hello();
    test_stats();
    test_catalog_discovery();
    test_catalog_serve_and_publish();
    test_request_manifest();

    test_connect_handshake_and_effective_limits();
    test_connect_times_out_without_a_reply();
    test_connect_requires_send();
    test_disconnect_sends_session_close();

    test_static_node_owns_its_catalog();
    test_publish_named_round_trips();
    test_publish_with_overrides_cfg_seal();

    test_command_round_trip_and_dedup_replay();
    test_command_times_out_without_a_reply();
    test_reply_seal_hook_picks_seal_per_command_requester();
    test_on_terminal_delivers_and_falls_back_to_complete();
    test_node_config_wires_on_terminal();
    test_static_node_bundles_commands();
    test_static_node_config_wires_commands();

    test_status_disabled_by_default();
    test_status_sends_after_the_period_and_counts_frames_tx();

    test_subscribe_grant_publish_cadence_and_unsubscribe();
    test_subscribe_renews_before_the_lease_runs_out();
    test_reply_seal_hook_picks_seal_per_subscribe_requester();
    test_publish_subscribed_topics_walks_registered_topics();
    test_topic_with_fill_registers_publish_in_one_call();
    test_static_node_grants_subscriptions_with_no_setup_call();
    test_begin_can_arm_session_and_announce_catalog();
    test_static_node_begin_folds_catalog_and_session_setup();
    test_begin_with_hello_connects_out();
    test_static_node_bundles_subscription_client();
    test_static_node_learn_catalog_with_sample_registers_on_sample();
    test_routine_publishes_and_ticks();
    test_routine_with_datagram_decodes_and_still_runs_housekeeping();
    test_routine_without_out_param_behaves_the_same();

    test_sized_node_medium_matches_static_node_defaults();
    test_sized_node_low_round_trips_a_topic();
    test_sized_node_high_begins();

    if (failures == 0) {
        std::cout << "test_node: all checks passed\n";
        return 0;
    }
    std::cerr << "test_node: " << failures << " check(s) failed\n";
    return 1;
}
