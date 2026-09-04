// example/hybrid.cpp
//
// A THIRD role, next to sender.cpp (pure producer) and receiver.cpp (pure
// consumer): one node that is both at once, on the SAME session -- it serves
// its own topic to whoever subscribes, AND learns + subscribes to a peer's
// topic to receive it too. There is no separate "hybrid mode" to switch on
// in btp::Node: HELLO's `role` field only labels who a peer says it is
// (docs/session-and-terminal.md section 1.5 -- producer / gateway / consumer
// / diagnostic tool, one value); it does not gate which optional halves of
// the API a Node may use. serve_catalog()/learn_catalog(),
// publish()/on_sample(), enable_commands()/enable_command_client(),
// enable_subscriptions()/enable_subscription_client() and
// enable_session()/connect() are six INDEPENDENT opt-in pairs -- responder
// and consumer here, initiator and producer in sender.cpp/receiver.cpp,
// any other combination just as valid. routine()'s own comment (node.hpp)
// already says as much: "The SAME call for a producer, a consumer, or a
// node that is both."
//
// This file is the RESPONDER half of that pairing (answers the peer's
// HELLO, same as sender.cpp) -- it does not choose who connects to it, so
// unlike receiver.cpp it cannot hardcode the peer's source_id/boot_id to
// subscribe() up front. Instead it reads them off node.session() the moment
// SessionEvent::HelloAccepted fires (Session::peer_source_id() /
// peer_boot_id(), session.hpp) and subscribes right then -- the responder
// learns who it's talking to from the HELLO it just answered, same as a
// hub would before aggregating a subscription upstream.
//
// The topic it SERVES ("node_status", 0x0201) and the topic it CONSUMES
// (0x0101, "drive_status" -- sender.cpp's own topic, same schema) are
// unrelated to each other -- there is no rule tying what a node publishes
// to what it subscribes to. Point this node at sender.cpp's peer identity
// (kPeerSourceId / kPeerBootId below) and any consumer of node_status
// (receiver.cpp, pointed at THIS file's cfg.source_id/boot_id instead of
// sender.cpp's) chains the three into one line: sender -> hybrid -> receiver.
//
// Same convention as sender.cpp / receiver.cpp: the link is faked
// (HybridLink::send() drops the bytes, link_poll() never delivers anything),
// so routine() never actually decodes below and the subscribe() branch never
// fires -- this file is the wiring, not a runnable demo.

#include <btp/aead.hpp>
#include <btp/node.hpp>

namespace {
    // Same 16 bytes as sender.cpp / receiver.cpp -- AEAD is symmetric, every
    // peer in this demo shares one key. A real deployment provisions each
    // pair's key out of band, never like this.
    const std::uint8_t kDemoAeadKey[btp::kAesGcmKeySize] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    };

    // sender.cpp's own identity -- the peer this node subscribes to once its
    // HELLO is accepted. Hardcoded here purely because this demo's link never
    // actually connects anyone; a responder in the field reads these off
    // node.session() instead (see main() below), never off a constant.
    const std::uint32_t kPeerSourceId = 0x00CAFE01U;
    const std::uint32_t kPeerBootId   = 0x0000B001U;

    // Opaque per-boot identity -- any stable 16 bytes work; a real
    // deployment derives or provisions its own. Handed to btp::HelloBuilder
    // below, which needs it as a constructor argument (it has no safe
    // default -- all-zero is invalid on the wire).
    const std::uint8_t kDemoPeerUuid[16] = {1, 2, 3, 4, 5, 6, 7, 8,
                                            9, 10, 11, 12, 13, 14, 15, 16};

    // ---- producer half: fills a sample of THIS node's own topic ----------
    void fill_node_status(void* /*ctx*/, btp::NamedSampleWriter& w) {
        w.put("uptime_s", 812.0);      // seconds since boot
        w.put_bool("link_ok", true);   // upstream link currently healthy
    }

    // ---- consumer half: decodes a sample of the PEER's topic --------------
    // Same shape as receiver.cpp's on_drive_status() -- reader yields values
    // in schema order, raw * scale + offset already applied.
    void on_drive_status(void* /*ctx*/, const btp::CatalogTopic& topic,
                         btp::SampleReader& reader) {
        btp::SampleValue v = {};
        while (reader.next(&v) == btp::SampleStep::Item) {
            const char* name  = topic.field_names[v.field->order];
            double      value = v.f64(0);
            // ... hand (name, value) to whatever this node does with it --
            // re-publish under node_status, feed a control loop, log it.
        }
    }

    // Everything btp::Node calls out to: the fake link (send()) and
    // AES-128-GCM (seal()/open()) -- same shape as ProducerLink /
    // ConsumerLink in sender.cpp / receiver.cpp, just one class doing both.
    class HybridLink : public btp::NodeConfig {
    public:
        bool send(const std::uint8_t* /*frame*/, std::size_t /*n*/) override {
            return true;
        }

        bool has_seal() const noexcept override { return true; }
        bool seal(const btp::Header& header, std::uint16_t payload_size,
                  const std::uint8_t* plaintext, std::uint8_t* out) override {
            const btp::AeadKey key{kDemoAeadKey, btp::kAesGcmKeySize};
            return btp::aead_seal(key, header, payload_size, plaintext, out) ==
                   btp::AeadError::Ok;
        }

        bool has_open() const noexcept override { return true; }
        bool open(const btp::Header& header, std::uint16_t sealed_size,
                  const std::uint8_t* sealed, std::uint8_t* out_plaintext) override {
            const btp::AeadKey key{kDemoAeadKey, btp::kAesGcmKeySize};
            return btp::aead_open(key, header, sealed_size, sealed,
                                  out_plaintext) == btp::AeadError::Ok;
        }
    };

    // Fake link, same as sender.cpp / receiver.cpp.
    std::size_t link_poll(std::uint8_t* /*out*/, std::size_t /*cap*/) { return 0U; }

}  // namespace

int main() {
    HybridLink cfg;
    cfg.source_id = 0x00FEED01U;                        // this node    (non-zero)
    cfg.boot_id   = 0x0000B002U;                         // new each boot (non-zero)
    cfg.transport = btp::TransportLimits{250U, true};    // must match the peer's

    btp::SizedNode<btp::NodeSize::Low> node(cfg);

    // consumer half: learn the peer's schema off the wire (never written by
    // hand -- that's the whole point of learn_catalog()) and decode every
    // TELEMETRY sample of whatever topic it turns out to describe.
    node.learn_catalog(&on_drive_status);

    // producer half: this node's OWN topic, declared once and served to
    // whoever subscribes -- identical to sender.cpp's node.topic() call,
    // just a different id/schema so it never collides with the one this
    // node also happens to consume.
    node.topic(
        /*topic_id=*/       0x0201U,
        /*schema_version=*/ 1U,
        /*topic_name=*/     "node_status",
        /*fill_function=*/  &fill_node_status)
        .u32("uptime_s", 1.0, "s")
        .boolean("link_ok")
        .end();

    // RESPONDER: answers the peer's HELLO with HELLO_RESULT, serves
    // node_status, arms the session and announces the catalogue -- the same
    // sugar sender.cpp uses. The `role` announced here is Producer, same as
    // sender.cpp -- role is advisory metadata (docs/session-and-terminal.md
    // section 1.5), not a switch the library reads; nothing about it
    // prevents learn_catalog()/subscribe() below from also running on this
    // node. There is no wire value that means "producer AND consumer" --
    // Gateway (0x02) is reserved for a relay/forwarding peer instead
    // (docs/encryption.md section 9), a different concept from this file's.
    // Every other Hello field takes btp::HelloBuilder's own default (see its
    // comment, messages.hpp).
    const btp::Hello hello =
        btp::HelloBuilder(btp::Role::Producer, kDemoPeerUuid).build();
    if (!node.begin("example-hybrid", hello)) {
        return 1;
    }

    std::uint64_t now_ms = 0U;
    std::uint32_t subscription_id = 0U;   // set once the peer's identity is known

    std::uint8_t datagram[256];
    for (;;) {
        const std::size_t n = link_poll(datagram, sizeof datagram);
        const btp::NodeRx rx = node.routine(datagram, n, now_ms);

        // The instant this node's HELLO_RESULT goes out, node.session()
        // knows who the peer IS -- unlike receiver.cpp, this side did not
        // pick the peer, so it could not subscribe() any earlier than this.
        // kPeerSourceId/kPeerBootId above would be
        // node.session()->peer_source_id()/peer_boot_id() in a deployment
        // where more than one peer could ever connect in; hardcoded here
        // only because this demo's fake link never actually delivers a HELLO.
        if (subscription_id == 0U && rx == btp::NodeRx::SessionHandled &&
            node.session_event() == btp::SessionEvent::HelloAccepted) {
            subscription_id = node.subscribe(
                kPeerSourceId, kPeerBootId,
                /*topic_id=*/     0x0101U,     // sender.cpp's "drive_status"
                /*rate_millihz=*/ 10000U,
                /*lease_ms=*/     60000U);
        }

        // remember to update the clock in your code, and get the time
        now_ms += 1U;
    }

    if (subscription_id != 0U) node.unsubscribe(subscription_id);
    return 0;
}
