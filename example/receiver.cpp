// example/receiver.cpp
//
// The consumer, with btp::Node. It has never seen the producer's schema and
// writes nothing about topic 0x0101 -- it learns everything from the wire.
// It is also the INITIATOR: it connects OUT to the producer and asks to be
// subscribed, instead of waiting for whatever the producer feels like
// sending. Every callback btp::Node calls out to (send/seal/open/terminal)
// is ConsumerLink, in node_config.hpp -- this file is the loop, not the
// wiring:
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
//   ConsumerLink::terminal()                     -> called directly for
//                                                TERMINAL_OUT (NodeRx::
//                                                TerminalDelivered); a
//                                                node.send(kTerminalIn, ...)
//                                                below is what prompted it
//   ConsumerLink::seal()/open() = AES-128-GCM   -> every one of the above,
//                                                sealed / opened the same
//                                                way -- SAME key as
//                                                sender.cpp/ProducerLink,
//                                                AEAD is symmetric
//
// sender.cpp is the other end. The link here is faked -- ConsumerLink::send()
// just drops the bytes and link_poll() below always delivers nothing, so
// receive() never actually decodes anything below; a real node hands frames
// to ESP-NOW / a UART / USB-HID and reads them back from there. This file is
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

#include "node_config.hpp"

namespace {
    // Opaque per-boot identity -- any stable 16 bytes work; a real
    // deployment derives or provisions its own. Handed to btp::HelloBuilder
    // below, which needs it as a constructor argument (it has no safe
    // default -- all-zero is invalid on the wire).
    const std::uint8_t kDemoPeerUuid[16] = {1, 2, 3, 4, 5, 6, 7, 8,
                                            9, 10, 11, 12, 13, 14, 15, 16};

    // The node calls this for each TELEMETRY sample of a topic it has
    // learned. `reader` yields the values in schema order, raw * scale +
    // offset already applied. Unrelated to NodeConfig -- learn_catalog()'s
    // own callback, a raw function pointer independent of send/seal/open/...
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

    // Fake link. A real one hands you each datagram as it arrives; this delivers
    // nothing, so the loop below only shows the wiring.
    std::size_t link_poll(std::uint8_t* /*out*/, std::size_t /*cap*/) { return 0U; }

}  // namespace

int main() {
    // `cfg` is held by REFERENCE (NodeConfig's own comment on lifetime) --
    // it outlives `node` below simply because it is declared first in the
    // same scope.
    example::ConsumerLink cfg;
    cfg.source_id = 0x00B0B0FEU;   // this device  (non-zero)
    cfg.boot_id   = 0x0000C0DEU;   // new each boot (non-zero)
    cfg.transport = btp::TransportLimits{250U, true};  // max_frame_size, allow_encrypted -- must match sender.cpp's

    // NodeSize::Low -- learns one small catalogue, holds one subscription
    // and one outstanding command. sizeof() == 7,248 (node.hpp's own
    // comment on SizedNode<>). Medium/High are the same shape, more room.
    btp::SizedNode<btp::NodeSize::Low> node(cfg);
    node.learn_catalog(&on_drive_status);  // this node's own catalogue, learned from the wire

    // Every other Hello field takes btp::HelloBuilder's own default (see its
    // comment, messages.hpp) -- override one with e.g. .max_subscriptions(1U)
    // if this consumer only ever holds one.
    const btp::Hello hello =
        btp::HelloBuilder(btp::Role::Consumer, kDemoPeerUuid).build();
    if (!node.begin(hello, /*connect_deadline_ms=*/2000U)) {
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
        /*action_id=*/      0x0001U,       // "stop" -- see sender.cpp's ProducerLink::command()
        /*action_version=*/ 1U,
        /*parameters=*/     nullptr, 0U);

    // in your code, you need to call the clock, and get the time
    std::uint64_t now_ms = 0U;

    // Simulate a user typing something into this node's shell -- see
    // sender.cpp's ProducerLink::terminal() for what answers it (TERMINAL_OUT,
    // which ConsumerLink::terminal() (node_config.hpp) receives).
    const std::uint8_t keystrokes[] = "status\n";
    node.send(btp::MessageType::Terminal, btp::object_id::kTerminalIn,
             keystrokes, sizeof(keystrokes) - 1, now_ms * 1000ULL);

    std::uint8_t datagram[256];
    for (;;) {
        const std::size_t n = link_poll(datagram, sizeof datagram);

        // datagram (if any) -> receive(); either way, connection watchdog +
        // subscription renewal. TERMINAL_OUT is handled inside routine()
        // itself now -- ConsumerLink::terminal() (node_config.hpp) already
        // ran; only CommandHandled still needs a check here, to read
        // command_outcome().
        if (node.routine(datagram, n, now_ms) == btp::NodeRx::CommandHandled) {
            handle_command_outcome(node.command_outcome());
        }

        // remember to update the clock in your code, and get the time
        now_ms += 1U;
    }

    node.unsubscribe(subscription_id);
    return 0;
}
