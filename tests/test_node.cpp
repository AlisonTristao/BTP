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
using btp::SessionEvent;
using btp::TransportProfile;

constexpr std::uint32_t kSenderId = 0x00CAFE01U;
constexpr std::uint32_t kSenderBoot = 0x0000B001U;
constexpr std::uint32_t kPeerId = 0x00B0B0FEU;
constexpr std::uint32_t kPeerBoot = 0x0000C0DEU;

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

NodeConfig base_config(std::uint32_t source_id, std::uint32_t boot_id,
                       Sink* sink) {
    NodeConfig cfg{};
    cfg.source_id = source_id;
    cfg.boot_id = boot_id;
    cfg.transport = TransportProfile::EspNow;
    cfg.send = &Sink::send;
    cfg.send_ctx = sink;
    cfg.clock = nullptr;
    cfg.clock_ctx = nullptr;
    cfg.seal = nullptr;
    cfg.seal_ctx = nullptr;
    cfg.open = nullptr;
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

    TestNode ok(base_config(kSenderId, kSenderBoot, &sink));
    CHECK(ok.begin());
    CHECK(ok.configured());
    CHECK(ok.source_id() == kSenderId);
    CHECK(ok.session() == nullptr);

    NodeConfig bad_id = base_config(0U, kSenderBoot, &sink);
    TestNode bad(bad_id);
    CHECK(!bad.begin());

    // A receive-only node may omit `send`: begin() still succeeds, but send()
    // then fails in place.
    NodeConfig nosend_cfg = base_config(kSenderId, kSenderBoot, &sink);
    nosend_cfg.send = nullptr;
    TestNode nosend(nosend_cfg);
    CHECK(nosend.begin());
    const std::uint8_t body[4] = {1, 2, 3, 4};
    CHECK(!nosend.send(MessageType::Telemetry, 0x0101U, body, sizeof(body), 0U));
}

void test_cleartext_roundtrip() {
    Sink tx;
    TestNode sender(base_config(kSenderId, kSenderBoot, &tx));
    Sink rx_out;
    TestNode receiver(base_config(kPeerId, kPeerBoot, &rx_out));
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
    TestNode sender(base_config(kSenderId, kSenderBoot, &tx));
    Sink rx_out;
    TestNode receiver(base_config(kPeerId, kPeerBoot, &rx_out));
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
    NodeConfig sender_cfg = base_config(kSenderId, kSenderBoot, &tx);
    sender_cfg.seal = &fake_seal;
    TestNode sender(sender_cfg);

    Sink rx_out;
    NodeConfig receiver_cfg = base_config(kPeerId, kPeerBoot, &rx_out);
    receiver_cfg.open = &fake_open;
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
    TestNode raw(base_config(0x00ABCDEFU, 0x00FEDCBAU, &other_out));
    raw.begin();
    ReceivedMessage sealed{};
    CHECK(deliver(raw, tx, 0U, &sealed) == NodeRx::Complete);
    CHECK(sealed.payload.size == payload.size() + btp::kEndpointAeadTagSize);
    CHECK(sealed.payload.data[0] == (payload[0] ^ 0x5AU));
}

void test_session_handshake() {
    Sink peer_tx;
    TestNode peer(base_config(kPeerId, kPeerBoot, &peer_tx));
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
    CHECK(initiator.send_logical(hello_msg, TransportProfile::EspNow,
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
                      TransportProfile::EspNow, &reply) == btp::Error::Ok);
    CHECK(reply.header.type == MessageType::Control);
    CHECK(reply.header.object_id == btp::object_id::kHelloResult);
    btp::HelloResult hr{};
    CHECK(btp::decode_hello_result(reply.payload.data, reply.payload.size,
                                   &hr) == btp::MessageError::Ok);
    CHECK(hr.status == static_cast<std::uint8_t>(btp::ResultStatus::Success));

    // Now an application frame from the same identity routes normally and
    // renews the watchdog.
    Sink app_tx;
    TestNode sender(base_config(kSenderId, kSenderBoot, &app_tx));
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
    TestNode peer(base_config(kPeerId, kPeerBoot, &peer_tx));
    peer.enable_session(make_hello(Role::Consumer), 0U);
    peer.begin();
    peer.arm_session(0U);

    Sink app_tx;
    TestNode sender(base_config(kSenderId, kSenderBoot, &app_tx));
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
    TestNode receiver(base_config(kPeerId, kPeerBoot, &rx_out));
    receiver.begin();

    // A too-short buffer is a decode failure.
    const std::uint8_t junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ReceivedMessage msg{};
    CHECK(receiver.receive(junk, sizeof(junk), 0U, &msg) == NodeRx::DroppedFrame);
    CHECK(receiver.stats().rx.dropped_decode >= 1U);

    Sink tx;
    TestNode sender(base_config(kSenderId, kSenderBoot, &tx));
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
    TestNode sender(base_config(kSenderId, kSenderBoot, &tx));
    sender.begin();

    Sink rx_out;
    TestNode consumer(base_config(kPeerId, kPeerBoot, &rx_out));
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
    TestNode producer(base_config(kSenderId, kSenderBoot, &prod_tx));
    btp::StaticCatalog<> served;
    served.set_config_revision(4U);
    CHECK(served.add_topic(0x0101U, 2U, "drive_status", kDriveFields) ==
          btp::MessageError::Ok);
    producer.serve_catalog(&served, static_cast<std::uint8_t>(Role::Producer),
                           uuid, "example-robot");
    producer.begin();

    // Consumer: a learn catalogue.
    Sink cons_tx;
    TestNode consumer(base_config(kPeerId, kPeerBoot, &cons_tx));
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
    TestNode latecomer(base_config(0x00A1A1A1U, 0x00B2B2B2U, &late_tx));
    latecomer.learn_catalog(&late);
    latecomer.begin();
    CHECK(deliver(latecomer, prod_tx, 0U, &msg) == NodeRx::CatalogUpdated);
    CHECK(late.topic(0x0101U) != nullptr);
}

// --- consumer sends MANIFEST_REQUEST ---------------------------------------

void test_request_manifest() {
    Sink tx;
    TestNode consumer(base_config(kPeerId, kPeerBoot, &tx));
    consumer.begin();
    CHECK(consumer.request_manifest(kSenderId, kSenderBoot, /*known_rev=*/0U));
    CHECK(tx.count() == 1U);

    btp::DecodedFrame frame{};
    CHECK(btp::decode(tx.frames[0].data(), tx.frames[0].size(),
                      TransportProfile::EspNow, &frame) == btp::Error::Ok);
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
    TestNode initiator(base_config(kSenderId, kSenderBoot, &init_tx));
    CHECK(initiator.begin());
    CHECK(!initiator.connected());

    CHECK(initiator.connect(make_hello(Role::Consumer), 0U,
                            /*deadline_ms=*/2000U));
    CHECK(init_tx.count() == 1U);

    Sink peer_tx;
    TestNode peer(base_config(kPeerId, kPeerBoot, &peer_tx));
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
    TestNode app(base_config(kPeerId, kPeerBoot, &app_tx));
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
    TestNode initiator(base_config(kSenderId, kSenderBoot, &tx));
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
    NodeConfig cfg = base_config(kSenderId, kSenderBoot, &tx);
    cfg.send = nullptr;
    TestNode initiator(cfg);
    initiator.begin();
    CHECK(!initiator.connect(make_hello(Role::Consumer), 0U, 2000U));
}

void test_disconnect_sends_session_close() {
    Sink init_tx;
    TestNode initiator(base_config(kSenderId, kSenderBoot, &init_tx));
    initiator.begin();
    CHECK(initiator.connect(make_hello(Role::Consumer), 0U, 2000U));

    Sink peer_tx;
    TestNode peer(base_config(kPeerId, kPeerBoot, &peer_tx));
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
                      TransportProfile::EspNow, &frame) == btp::Error::Ok);
    CHECK(frame.header.object_id == btp::object_id::kSessionClose);
}

// ===========================================================================
// btp::StaticNode -- its own catalogue (no separate btp::StaticCatalog)
// ===========================================================================

void test_static_node_owns_its_catalog() {
    Sink tx;
    TestNode node(base_config(kSenderId, kSenderBoot, &tx));

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
                      TransportProfile::EspNow, &frame) == btp::Error::Ok);
    CHECK(frame.header.object_id == btp::object_id::kManifestData);

    // The consumer side of the same sugar: learn_catalog() with no argument.
    Sink cons_tx;
    TestNode consumer(base_config(kPeerId, kPeerBoot, &cons_tx));
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
    TestNode producer(base_config(kSenderId, kSenderBoot, &prod_tx));
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
    TestNode consumer(base_config(kPeerId, kPeerBoot, &cons_tx));
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
    TestNode producer(base_config(kSenderId, kSenderBoot, &prod_tx));
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
    TestNode consumer(base_config(kPeerId, kPeerBoot, &cons_tx));
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
    TestNode responder(base_config(kSenderId, kSenderBoot, &resp_tx));
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
    TestNode initiator(base_config(kPeerId, kPeerBoot, &init_tx));
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

void test_command_times_out_without_a_reply() {
    Sink tx;
    TestNode initiator(base_config(kSenderId, kSenderBoot, &tx));
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
// btp::Node -- STATUS
// ===========================================================================

void test_status_disabled_by_default() {
    Sink tx;
    TestNode node(base_config(kSenderId, kSenderBoot, &tx));
    node.begin();
    CHECK(!node.status_enabled());
    node.tick(1000000U);
    CHECK(tx.count() == 0U);
}

void test_status_sends_after_the_period_and_counts_frames_tx() {
    Sink tx;
    TestNode node(base_config(kSenderId, kSenderBoot, &tx));
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
                      TransportProfile::EspNow, &frame) == btp::Error::Ok);
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
    TestNode producer(base_config(kSenderId, kSenderBoot, &prod_tx));
    btp::StaticCatalog<> served;
    served.add_topic(0x0101U, 2U, "drive_status", kDriveFields);
    producer.serve_catalog(&served, static_cast<std::uint8_t>(Role::Producer),
                           nullptr, "example-robot");
    SubscriptionRecord table_slots[4];
    SubscriptionTable table(table_slots, 4);
    producer.enable_subscriptions(&table);
    producer.begin();

    Sink cons_tx;
    TestNode consumer(base_config(kPeerId, kPeerBoot, &cons_tx));
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

}  // namespace

int main() {
    test_begin();
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

    test_command_round_trip_and_dedup_replay();
    test_command_times_out_without_a_reply();

    test_status_disabled_by_default();
    test_status_sends_after_the_period_and_counts_frames_tx();

    test_subscribe_grant_publish_cadence_and_unsubscribe();
    test_subscribe_renews_before_the_lease_runs_out();

    if (failures == 0) {
        std::cout << "test_node: all checks passed\n";
        return 0;
    }
    std::cerr << "test_node: " << failures << " check(s) failed\n";
    return 1;
}
