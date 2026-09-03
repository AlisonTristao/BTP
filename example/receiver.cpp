// example/receiver.cpp
//
// The consumer, with btp::Node. It has NEVER seen the producer's schema and
// writes nothing about topic 0x0101. It is also the INITIATOR -- it connects
// OUT to the producer and asks to be subscribed, instead of receiving
// whatever the producer feels like sending:
//
//   node.connect(my_hello, deadline_ms)   -- HELLO -> HELLO_RESULT; once
//                                          connected(), every frame from the
//                                          peer renews the watchdog on its own
//   node.subscribe(peer, topic, rate,     -- SUBSCRIBE; tick() renews it on
//                  lease_ms)                 its own before the lease runs out
//   node.learn_catalog()                  -- from now on the node ingests
//                                          every MANIFEST_DATA into its OWN
//                                          catalogue (node.catalog())
//   node.on_sample(&on_drive_status)      -- and decodes every TELEMETRY
//                                          sample of a learned topic, fields
//                                          already scaled
//
// The link is faked -- link_poll() delivers nothing, so the loop below never
// actually runs; it is only there to show the shape. by_hand_receiver.cpp is
// the runnable wire-level version.

#include <btp/node.hpp>

namespace {

const std::uint32_t kProducerSourceId = 0x00CAFE01U;  // must match sender.cpp
const std::uint32_t kProducerBootId = 0x0000B001U;
const std::uint16_t kDriveStatus = 0x0101U;

btp::Hello make_hello(btp::Role role) {
    btp::Hello h = {};
    h.role = static_cast<std::uint8_t>(role);
    h.version_count = 1U;
    h.versions[0] = 1U;
    h.max_logical_payload = 2048U;
    h.max_inflight_reassemblies = 4U;
    h.max_subscriptions = 8U;
    h.max_dedup_entries = 32U;
    h.session_timeout_ms = 30000U;
    for (int i = 0; i < 16; ++i) h.peer_uuid[i] = static_cast<std::uint8_t>(i + 1);
    return h;
}

// The node calls this for each TELEMETRY sample of a topic it has learned.
// `reader` yields the values in schema order, raw * scale + offset applied.
void on_drive_status(void* /*ctx*/, const btp::CatalogTopic& topic,
                     btp::SampleReader& reader) {
    btp::SampleValue v = {};
    while (reader.next(&v) == btp::SampleStep::Item) {
        const char* name  = topic.field_names[v.field->order];  // "left_rpm", ...
        double      value = v.f64(0);   // scale already applied; skip if v.is_null
        // ... hand (name, value) to the UI / log
    }
}

// Hand one delivered datagram to the node and act on what it was.
void on_datagram(btp::Node& node, const std::uint8_t* data, std::size_t size,
                 std::uint64_t now_ms) {
    btp::ReceivedMessage msg = {};
    switch (node.receive(data, size, now_ms, &msg)) {
        case btp::NodeRx::InitiatorHandled:
            // connect()'s HELLO_RESULT arrived, or the connection timed out --
            // node.initiator_event() says which (Connected / Rejected / TimedOut)
            break;
        case btp::NodeRx::SubscriptionHandled:
            // subscribe()'s SUBSCRIBE_RESULT (or a renewal's) arrived --
            // node.subscription_event() says Granted / Rejected
            break;
        case btp::NodeRx::CatalogUpdated:
            // learned / refreshed the schema from a MANIFEST_DATA
            break;
        case btp::NodeRx::SampleDelivered:
            // on_drive_status() already ran for this sample
            break;
        case btp::NodeRx::Ignored:
            // a sample arrived before its schema -- wait for the manifest
            break;
        case btp::NodeRx::Pending:
            // one fragment of a larger message -- more to come
            break;
        default:
            // Complete (a message the node did not consume) / DroppedFrame
            break;
    }
}

// The node hands every finished frame here. Fake: a real node transmits it.
// Needed now that this node also connect()s / subscribe()s out.
bool send_frame(void* /*ctx*/, const std::uint8_t* /*frame*/, std::size_t /*n*/) {
    return true;
}

// Fake link. A real one hands you each datagram as it arrives; this delivers
// nothing, so the loop below only shows the wiring.
std::size_t link_poll(std::uint8_t* /*out*/, std::size_t /*cap*/) { return 0U; }

}  // namespace

int main() {
    btp::NodeConfig cfg = {};
    cfg.source_id = 0x00B0B0FEU;
    cfg.boot_id   = 0x0000C0DEU;
    cfg.transport = btp::TransportProfile::EspNow;
    cfg.send      = &send_frame;

    btp::StaticNode<> node(cfg);
    node.learn_catalog();  // the node's own catalogue
    node.on_sample(&on_drive_status, nullptr);

    // The subscriptions THIS node holds on peers -- one slot per topic
    // subscribed to, across however many peers.
    btp::ClientSubscription subscription_slots[4];
    btp::SubscriptionClient subscriptions(subscription_slots, 4);
    node.enable_subscription_client(&subscriptions);

    node.begin();
    node.connect(make_hello(btp::Role::Consumer), /*deadline_ms=*/2000U);

    // subscribe() works whether or not the schema has been learned yet --
    // SUBSCRIBE only names a topic_id, not its fields. A real loop would
    // more likely wait for InitiatorHandled / connected() first; shown right
    // here only because the fake link never delivers that event anyway.
    const std::uint32_t subscription_id = node.subscribe(
        kProducerSourceId, kProducerBootId, kDriveStatus,
        /*rate_millihz=*/10000U, /*lease_ms=*/60000U);

    std::uint64_t now_ms = 0U;
    std::uint8_t datagram[256];
    for (std::size_t n; (n = link_poll(datagram, sizeof datagram)) != 0U; ++now_ms) {
        on_datagram(node, datagram, n, now_ms);
        node.tick(now_ms);  // connection watchdog + subscription renewal, both automatic
    }

    node.unsubscribe(subscription_id);
    return 0;
}
