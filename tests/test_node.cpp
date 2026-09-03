// Unit tests for btp::Node -- the facade of docs/library.md chapter 16.
//
// The Node is wiring over btp::Endpoint + btp::Receiver + btp::Session, not a
// new wire layout, so there is no vector tree. Every test drives a Node (often
// a sender / receiver pair) and checks the frames, the reassembled payloads and
// the session replies with the public btp:: APIs.

#include "btp/node.hpp"

#include "btp/codec.hpp"
#include "btp/messages.hpp"

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

    NodeConfig bad_send = base_config(kSenderId, kSenderBoot, &sink);
    bad_send.send = nullptr;
    TestNode nosend(bad_send);
    CHECK(!nosend.begin());
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

}  // namespace

int main() {
    test_begin();
    test_cleartext_roundtrip();
    test_fragmented_roundtrip();
    test_sealed_roundtrip();
    test_session_handshake();
    test_session_ignores_frame_before_hello();
    test_stats();

    if (failures == 0) {
        std::cout << "test_node: all checks passed\n";
        return 0;
    }
    std::cerr << "test_node: " << failures << " check(s) failed\n";
    return 1;
}
