// example/sender.cpp
//
// The producer, with btp::Node. It owns ONE topic -- declared once, below, on
// the node's own catalogue -- and answers both a connection and a
// subscription from whoever asks. Every callback btp::Node calls out to
// (send/seal/open/terminal/command) is ProducerLink, in node_config.hpp --
// this file is the loop, not the wiring:
//
//   node.begin(source_name, hello)             -> arms the session (answers
//                                                HELLO with HELLO_RESULT,
//                                                runs the inactivity
//                                                watchdog) and announces the
//                                                catalogue -- MANIFEST_DATA,
//                                                "here is my schema"
//   (StaticNode grants this on its own)        -> answers SUBSCRIBE /
//                                                UNSUBSCRIBE against its own
//                                                topic table, no setup call
//   node.topic(..., fill) + routine()          -> TELEMETRY, only when a
//                                                subscriber is actually
//                                                waiting for it
//   ProducerLink::command()                     -> answers COMMAND_REQUEST;
//                                                StaticNode<> already owns
//                                                the dedup cache
//   ProducerLink::terminal()                    -> called directly for
//                                                TERMINAL_IN (NodeRx::
//                                                TerminalDelivered), which
//                                                answers with TERMINAL_OUT
//   ProducerLink::seal()/open() = AES-128-GCM   -> every one of the above,
//                                                sealed / opened the same
//                                                way, ENCRYPTED set on its
//                                                own from has_seal()
//
// receiver.cpp is the other end: it connect()s in, subscribes to the topic,
// and learns the schema from the wire, never from a shared header. The link
// here is faked -- ProducerLink::send() just drops the bytes and
// link_poll() below always delivers nothing, so receive() never actually
// decodes anything below; a real node hands frames to ESP-NOW / a UART /
// USB-HID and reads them back from there. This file is the logic, not a
// runnable demo. by_hand_sender.cpp is the runnable wire-level walkthrough.
//
// The loop itself DOES run, though -- forever, same as real firmware, paced
// by the hardware/RTOS tick, not by a counter. One call to routine() covers
// every pass, datagram or not: it only decodes when link_poll() actually
// delivered something (n != 0), but publishing due topics and sweeping the
// session watchdog happen every single pass regardless -- a real producer
// publishes telemetry and sweeps its watchdog on its own schedule, whether
// or not a SUBSCRIBE happened to land that same instant.

#include "node_config.hpp"

namespace {
    // Opaque per-boot identity -- any stable 16 bytes work; a real
    // deployment derives or provisions its own. Shared with make_hello()'s
    // uuid below since btp::HelloBuilder needs it as a constructor argument
    // (it has no safe default -- all-zero is invalid on the wire).
    const std::uint8_t kDemoPeerUuid[16] = {1, 2, 3, 4, 5, 6, 7, 8,
                                            9, 10, 11, 12, 13, 14, 15, 16};

    // publish_named() calls this to write one sample. Each put() must name the
    // schema's NEXT field -- get the order wrong and it fails right there
    // (MessageError::InvalidArgument) instead of sending the wrong value.
    void fill_drive_status(void* /*ctx*/, btp::NamedSampleWriter& w) {
        w.put("left_rpm",   1450.0);    // rev/min
        w.put("right_rpm", -1448.5);    // rev/min -- this wheel runs reversed
        w.put("battery_v",  3.72);      // 3.72 V <-> raw uint16 3720 (scale 0.001)
        w.put_null("temp_c");           // sensor offline
    }

    // Fake link. A real one hands you each datagram as it arrives; this delivers
    // nothing, so the loop below only shows the wiring.
    std::size_t link_poll(std::uint8_t* /*out*/, std::size_t /*cap*/) { return 0U; }

}  // namespace

int main() {
    // The node's own configuration -- its identity, the transport it
    // targets, and every callback it needs (send / seal / open / terminal /
    // command) as ONE object. `cfg` is held by REFERENCE (NodeConfig's own
    // comment on lifetime) -- it outlives `node` below simply because it is
    // declared first in the same scope.
    example::ProducerLink cfg;
    cfg.source_id = 0x00CAFE01U;                                // this robot    (non-zero)
    cfg.boot_id   = 0x0000B001U;                                // new each boot (non-zero) // attention: to use criptography, you need count the boots
    cfg.transport = btp::TransportLimits{250U, true};           // max_frame_size (payload derives automatically, max_frame_size - 40), allow_encrypted -- set by hand for whatever your link's MTU actually is, no named preset required

    // NodeSize::Low -- one small topic, one command, no subscribers to
    // speak of. sizeof() == 7,248 (node.hpp's own comment on SizedNode<>).
    // Medium/High are the same shape, more room.
    btp::SizedNode<btp::NodeSize::Low> node(cfg);

    // in this example, we declare this topic:
    /*  topic_id:       0x0101U
        schema_version: 3U
        topic_name:     "drive_status"
        fields:
            left_rpm:   f32, scale=1.0, unit="rpm"
            right_rpm:  f32, scale=1.0, unit="rpm"
            battery_v:  u16, scale=0.001, unit="V"
            temp_c:     i16, scale=0.1, unit="Cel", is_nullable=true */
    node.topic(
        /*topic_id=*/       0x0101U,
        /*schema_version=*/ 3U,
        /*topic_name=*/     "drive_status",
        /*fill_function=*/  &fill_drive_status)

        // the schema's fields
        .f32("left_rpm",  "rpm")
        .f32("right_rpm", "rpm")
        .u16("battery_v", 0.001, "V")
        .i16("temp_c",    0.1,   "Cel", /*is_nullable=*/true)
        .end();

    // Every other Hello field (protocol version, payload/reassembly/
    // subscription/dedup limits, watchdog timeout) takes btp::HelloBuilder's
    // own default (see its comment, messages.hpp) -- override one with e.g.
    // .session_timeout_ms(15000U) if this robot needs a shorter watchdog.
    if (!node.begin("example-robot",
                    btp::HelloBuilder(btp::Role::Producer, kDemoPeerUuid).build())) {
        return 1;
    }

    // in your code, you need to call the clock, and get the time
    std::uint64_t now_ms = 0U;

    std::uint8_t datagram[256];
    for (;;) {
        const std::size_t n = link_poll(datagram, sizeof datagram);

        // datagram (if any) -> receive(); either way, publish due topics +
        // session watchdog. COMMAND_REQUEST / TERMINAL_IN are both handled
        // inside routine() itself now -- ProducerLink::command() /
        // ProducerLink::terminal() (node_config.hpp) already ran, nothing
        // left to check here, so no *out to pass.
        node.routine(datagram, n, now_ms);

        // remember to update the clock in your code, and get the time
        now_ms += 1U;
    }
    return 0;
}
