// example/sender.cpp
//
// The producer, with btp::Node. It owns ONE topic -- declared once, below, on
// the node's own catalogue -- and answers both a connection and a
// subscription from whoever asks:
//
//   node.enable_session(...) + arm_session()  -> answers HELLO with
//                                                HELLO_RESULT, then runs the
//                                                inactivity watchdog
//   node.enable_subscriptions(&table)          -> answers SUBSCRIBE /
//                                                UNSUBSCRIBE against its own
//                                                topic table
//   node.announce_catalog()                    -> MANIFEST_DATA  "here is my schema"
//   table.due(...) + publish_named(...)        -> TELEMETRY, only when a
//                                                subscriber is actually
//                                                waiting for it
//
// receiver.cpp is the other end: it connect()s in, subscribes to the topic,
// and learns the schema from the wire, never from a shared header. The link
// here is faked -- send_frame() just drops the bytes and link_poll() delivers
// nothing, so the loop below never actually runs; a real node hands frames to
// ESP-NOW / a UART / USB-HID and reads them back from there. This file is the
// logic, not a runnable demo. by_hand_sender.cpp is the runnable wire-level
// walkthrough.

#include <btp/node.hpp>

namespace {

    const std::uint16_t kDriveStatus = 0x0101U;

    // Built once and handed to enable_session() below -- this peer's side of
    // the handshake: who it is, what it can take, how long it waits before
    // giving up on a quiet peer.
    btp::Hello make_hello(btp::Role role) {
        btp::Hello h = {};
        h.role                      = static_cast<std::uint8_t>(role);  // Producer here -- the other end answers as Consumer
        h.version_count             = 1U;                               // how many entries versions[] below actually holds
        h.versions[0]               = 1U;                               // the one protocol version this peer speaks
        h.max_logical_payload       = 2048U;                            // largest reassembled message this peer accepts
        h.max_inflight_reassemblies = 4U;                               // concurrent fragmented messages it can track at once
        h.max_subscriptions         = 8U;                               // subscription slots it can grant
        h.max_dedup_entries         = 32U;                              // command-request dedup cache size
        h.session_timeout_ms        = 30000U;                           // inactivity watchdog: this long without a frame ends the session
        for (int i = 0; i < 16; ++i)
            h.peer_uuid[i] = static_cast<std::uint8_t>(i + 1);          // opaque per-boot identity -- any stable 16 bytes work
        return h;
    }

    // publish_named() calls this to write one sample. Each put() must name the
    // schema's NEXT field -- get the order wrong and it fails right there
    // (MessageError::InvalidArgument) instead of sending the wrong value.
    void fill_drive_status(void* /*ctx*/, btp::NamedSampleWriter& w) {
        w.put("left_rpm",   1450.0);    // rev/min
        w.put("right_rpm", -1448.5);    // rev/min -- this wheel runs reversed
        w.put("battery_v",  3.72);      // 3.72 V <-> raw uint16 3720 (scale 0.001)
        w.put_null("temp_c");           // sensor offline
    }

    // The node hands every finished frame here. Fake: a real node transmits it.
    bool send_frame(void* /*ctx*/, const std::uint8_t* /*frame*/, std::size_t /*n*/) {
        return true;
    }

    // Fake link. A real one hands you each datagram as it arrives; this delivers
    // nothing, so the loop below only shows the wiring.
    std::size_t link_poll(std::uint8_t* /*out*/, std::size_t /*cap*/) { return 0U; }

}  // namespace

int main() {
    btp::NodeConfig cfg = {};
    cfg.source_id = 0x00CAFE01U;   // this robot    (non-zero)
    cfg.boot_id   = 0x0000B001U;   // new each boot (non-zero)
    cfg.transport = btp::TransportProfile::EspNow;
    cfg.send      = &send_frame;

    btp::StaticNode<> node(cfg);

    // THE schema, written once, straight on the node's own catalogue -- no
    // separate btp::StaticCatalog to declare or thread through by pointer.
    // Floats travel raw; an integer field carries a scale so a ranged value
    // packs into a small type (engineering value = raw * scale + offset).
    node.topic(kDriveStatus, /*schema_version=*/3U, "drive_status")
        .f32("left_rpm",  "rpm")
        .f32("right_rpm", "rpm")
        .u16("battery_v", 0.001, "V")
        .i16("temp_c",    0.1,   "Cel", /*is_nullable=*/true)
        .end();

    // Pairs the topic above with the function that fills one sample of it --
    // publish_subscribed_topics() in the loop below is what actually calls
    // this, only when a subscriber is due. Add a second topic later with
    // another node.topic(...).end() + node.on_publish(...) pair; nothing
    // else in this file changes, publish_subscribed_topics() already walks
    // every topic registered this way.
    node.on_publish(kDriveStatus, &fill_drive_status, nullptr);

    node.catalog().set_config_revision(1U);
    node.serve_catalog(static_cast<std::uint8_t>(btp::Role::Producer),
                        /*source_uuid=*/nullptr,
                        "example-robot");

    node.enable_session(make_hello(btp::Role::Producer),
                         /*hello_deadline_ms=*/0U);

    // Who is allowed to hear kDriveStatus, at what rate, until when -- the
    // storage stays here, the node just points at it.
    btp::SubscriptionRecord subscription_slots[4];
    btp::SubscriptionTable  subscriptions(subscription_slots, 4);
    node.enable_subscriptions(&subscriptions);

    node.begin();
    node.arm_session();
    node.announce_catalog();  // -> MANIFEST_DATA, so a late joiner needs no request

    std::uint64_t now_ms = 0U;
    std::uint8_t  datagram[256];
    for (std::size_t n; (n = link_poll(datagram, sizeof datagram)) != 0U; ++now_ms) {
        // Hand the datagram to the node. It decodes it and, on its own,
        // already answers whatever it was (HELLO, SUBSCRIBE, ...) before this
        // call even returns -- there is nothing left here to react to, so the
        // outcome is discarded. Keep it (btp::NodeRx outcome = node.receive(...))
        // and switch on it only if you want to OBSERVE a specific event
        // (logging, a counter, a test assertion) -- see the block below.
        btp::ReceivedMessage msg = {};
        node.receive(datagram, n, now_ms, &msg);

        // switch (outcome) {
        //     case btp::NodeRx::SubscriptionServed:
        //         // a SUBSCRIBE / UNSUBSCRIBE -- already answered, and the
        //         // subscription table above already updated
        //         break;
        //     case btp::NodeRx::SessionHandled:
        //         // HELLO / SESSION_CLOSE -- already answered, and the
        //         // session state already updated
        //         break;
        //     default:
        //         break;
        // }

        // Walks every node.on_publish()-registered topic and publishes
        // whichever ones are due -- one call in place of a hand-written
        // "for each topic: if due(), fill and publish, then note_published()"
        // loop. The fastest currently-granted rate sets the cadence, and one
        // publish satisfies every subscriber of that topic at once
        // (subscription.hpp).
        node.publish_subscribed_topics(now_ms);

        // session watchdog + lapsed-lease sweep
        node.tick(now_ms);
    }
    return 0;
}
