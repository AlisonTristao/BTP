// example/receiver.cpp
//
// The consumer, with btp::Node. It has never seen the producer's schema and
// writes nothing about topic 0x0101 -- it learns everything from the wire.
// It is also the INITIATOR: it connects OUT to the producer and asks to be
// subscribed, instead of waiting for whatever the producer feels like
// sending:
//
//   node.begin(hello, deadline_ms)              -> HELLO -> HELLO_RESULT;
//                                                once connected(), every
//                                                frame from the peer renews
//                                                the watchdog on its own
//   node.learn_catalog(&on_drive_status)        -> ingests MANIFEST_DATA into
//                                                this node's own catalogue
//                                                and decodes every TELEMETRY
//                                                sample of a learned topic,
//                                                fields already scaled
//   node.subscribe(peer, topic, rate, lease_ms) -> SUBSCRIBE; routine()
//                                                renews it on its own before
//                                                the lease runs out
//   node.command(peer, action_id, ...)          -> COMMAND_REQUEST; the
//                                                correlated result comes
//                                                back as NodeRx::CommandHandled
//                                                (command_outcome())
//   cfg.terminal = &handle_terminal_out          -> calls it directly for
//                                                TERMINAL_OUT (NodeRx::
//                                                TerminalDelivered); a
//                                                node.send(kTerminalIn, ...)
//                                                below is what prompted it
//   cfg.seal / cfg.open = AES-128-GCM (btp/aead.hpp), kDemoAeadKey below --
//                                                every one of the above,
//                                                sealed / opened the same
//                                                way -- SAME key as
//                                                sender.cpp, AEAD is symmetric
//
// sender.cpp is the other end. The link here is faked -- send_frame() just
// drops the bytes and link_poll() always delivers nothing, so receive()
// never actually decodes anything below; a real node hands frames to
// ESP-NOW / a UART / USB-HID and reads them back from there. This file is
// the logic, not a runnable demo. by_hand_receiver.cpp is the runnable
// wire-level walkthrough.
//
// The loop itself DOES run, though -- forever, same as real firmware, paced
// by the hardware/RTOS tick, not by a counter. One call to routine() covers
// every pass, datagram or not: it only decodes when link_poll() actually
// delivered something (n != 0), but sweeping the connection watchdog and
// renewing the subscription happen every single pass regardless -- a real
// consumer does that on its own schedule, whether or not a frame happened
// to land that same instant.

#include <btp/aead.hpp>
#include <btp/node.hpp>

namespace {
    // Must be the SAME 16 bytes as sender.cpp's kDemoAeadKey -- AEAD is
    // symmetric, both peers seal/open with one shared key. A real
    // deployment provisions each pair's key out of band, never like this.
    const std::uint8_t kDemoAeadKey[btp::kAesGcmKeySize] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    };
    // Built once and handed to node.begin() below -- this peer's side of
    // the handshake: who it is, what it can take, how long it waits before
    // giving up on a quiet peer.
    btp::Hello make_hello(btp::Role role) {
        btp::Hello h = {};
        h.role                      = static_cast<std::uint8_t>(role);  // Consumer here -- the other end answers as Producer
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

    // The node calls this for each TELEMETRY sample of a topic it has
    // learned. `reader` yields the values in schema order, raw * scale +
    // offset already applied.
    void on_drive_status(void* /*ctx*/, const btp::CatalogTopic& topic,
                         btp::SampleReader& reader) {
        btp::SampleValue v = {};
        while (reader.next(&v) == btp::SampleStep::Item) {
            const char* name  = topic.field_names[v.field->order];  // "left_rpm", ...
            double      value = v.f64(0);   // scale already applied; skip if v.is_null
            // ... hand (name, value) to the UI / log
        }
    }

    // After a NodeRx::CommandHandled, node.command_outcome() is the
    // correlated COMMAND_RESULT for a command() this node holds -- or a
    // TimedOut if none arrived within btp::kCommandTimeoutMs.
    void handle_command_outcome(const btp::CommandOutcome& outcome) {
        if (outcome.event == btp::CommandEvent::Completed) {
            // outcome.status / error_code / message / result -- whatever the
            // action decided; Success is not implied, check status.
        }  // else TimedOut -- nothing arrived in time
    }

    // node.on_terminal() below is what gets this called for a TERMINAL_OUT
    // frame at all -- see sender.cpp's handle_terminal_in() for the other
    // direction. A real shell would render `payload` to the console instead.
    void handle_terminal_out(void* /*ctx*/, btp::Node& /*node*/,
                             const btp::Header& /*header*/,
                             btp::ByteView /*payload*/, std::uint64_t /*now_ms*/) {}

    // EndpointSealFn / NodeOpenFn -- see sender.cpp's seal_message() /
    // open_message() for the full comment; identical here, same key.
    bool seal_message(void* context, const btp::Header& header,
                      std::uint16_t payload_size, const std::uint8_t* plaintext,
                      std::uint8_t* out) {
        const btp::AeadKey key{static_cast<const std::uint8_t*>(context),
                               btp::kAesGcmKeySize};
        return btp::aead_seal(key, header, payload_size, plaintext, out) ==
               btp::AeadError::Ok;
    }

    bool open_message(void* context, const btp::Header& header,
                      std::uint16_t sealed_size, const std::uint8_t* sealed,
                      std::uint8_t* out_plaintext) {
        const btp::AeadKey key{static_cast<const std::uint8_t*>(context),
                               btp::kAesGcmKeySize};
        return btp::aead_open(key, header, sealed_size, sealed,
                              out_plaintext) == btp::AeadError::Ok;
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
    cfg.source_id = 0x00B0B0FEU;   // this device  (non-zero)
    cfg.boot_id   = 0x0000C0DEU;   // new each boot (non-zero)
    cfg.transport = btp::TransportLimits{250U, true};  // max_frame_size, allow_encrypted -- must match sender.cpp's
    cfg.send      = &send_frame;
    cfg.terminal  = &handle_terminal_out;
    cfg.seal      = &seal_message;
    cfg.seal_ctx  = const_cast<std::uint8_t*>(kDemoAeadKey);  // void* ctx never modifies it
    cfg.open      = &open_message;
    cfg.open_ctx  = const_cast<std::uint8_t*>(kDemoAeadKey);

    // NodeSize::Low -- learns one small catalogue, holds one subscription
    // and one outstanding command. sizeof() == 7,248 (node.hpp's own
    // comment on SizedNode<>). Medium/High are the same shape, more room.
    btp::SizedNode<btp::NodeSize::Low> node(cfg);
    node.learn_catalog(&on_drive_status);  // this node's own catalogue, learned from the wire

    if (!node.begin(make_hello(btp::Role::Consumer), /*connect_deadline_ms=*/2000U)) {
        return 1;
    }

    // subscribe() works whether or not the schema has been learned yet --
    // SUBSCRIBE only names a topic_id, not its fields. StaticNode<> already
    // has a subscription client of its own, so no separate setup call.
    const std::uint32_t subscription_id = node.subscribe(
        /*peer_source_id=*/ 0x00CAFE01U,   // must match sender.cpp's cfg.source_id
        /*peer_boot_id=*/   0x0000B001U,   // must match sender.cpp's cfg.boot_id
        /*topic_id=*/       0x0101U,
        /*rate_millihz=*/   10000U,
        /*lease_ms=*/       60000U);

    // command() works whether or not connected() yet -- same "a real caller
    // would more likely wait for InitiatorHandled first" note as subscribe()
    // above; here only because the fake link never delivers that event anyway.
    const std::uint32_t command_id = node.command(
        /*peer_source_id=*/ 0x00CAFE01U,   // must match sender.cpp's cfg.source_id
        /*peer_boot_id=*/   0x0000B001U,   // must match sender.cpp's cfg.boot_id
        /*action_id=*/      0x0001U,       // "stop" -- see sender.cpp's handle_command()
        /*action_version=*/ 1U,
        /*parameters=*/     nullptr, 0U);

    // in your code, you need to call the clock, and get the time
    std::uint64_t now_ms = 0U;

    // Simulate a user typing something into this node's shell -- see
    // sender.cpp's handle_terminal_in() for what answers it (TERMINAL_OUT,
    // which on_terminal() above hands straight to handle_terminal_out()).
    const std::uint8_t keystrokes[] = "status\n";
    node.send(btp::MessageType::Terminal, btp::object_id::kTerminalIn,
             keystrokes, sizeof(keystrokes) - 1, now_ms * 1000ULL);

    std::uint8_t datagram[256];
    for (;;) {
        const std::size_t n = link_poll(datagram, sizeof datagram);

        // datagram (if any) -> receive(); either way, connection watchdog +
        // subscription renewal. TERMINAL_OUT is handled inside routine()
        // itself now -- handle_terminal_out() above already ran; only
        // CommandHandled still needs a check here, to read command_outcome().
        if (node.routine(datagram, n, now_ms) == btp::NodeRx::CommandHandled) {
            handle_command_outcome(node.command_outcome());
        }

        // remember to update the clock in your code, and get the time
        now_ms += 1U;
    }

    node.unsubscribe(subscription_id);
    return 0;
}
