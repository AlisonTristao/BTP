// example/sender.cpp
//
// The producer, with btp::Node. It owns ONE topic -- declared once, below, on
// the node's own catalogue -- and answers both a connection and a
// subscription from whoever asks:
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
//   cfg.command = &handle_command               -> answers COMMAND_REQUEST;
//                                                StaticNode<> already owns
//                                                the dedup cache
//   cfg.terminal = &handle_terminal_in          -> calls it directly for
//                                                TERMINAL_IN (NodeRx::
//                                                TerminalDelivered), which
//                                                answers with TERMINAL_OUT
//   cfg.seal / cfg.open = AES-128-GCM (btp/aead.hpp), kDemoAeadKey below --
//                                                every one of the above,
//                                                sealed / opened the same
//                                                way, ENCRYPTED set on its
//                                                own from cfg.seal != nullptr
//
// receiver.cpp is the other end: it connect()s in, subscribes to the topic,
// and learns the schema from the wire, never from a shared header. The link
// here is faked -- send_frame() just drops the bytes and link_poll() always
// delivers nothing, so receive() never actually decodes anything below; a
// real node hands frames to ESP-NOW / a UART / USB-HID and reads them back
// from there. This file is the logic, not a runnable demo. by_hand_sender.cpp
// is the runnable wire-level walkthrough.
//
// The loop itself DOES run, though -- forever, same as real firmware, paced
// by the hardware/RTOS tick, not by a counter. One call to routine() covers
// every pass, datagram or not: it only decodes when link_poll() actually
// delivered something (n != 0), but publishing due topics and sweeping the
// session watchdog happen every single pass regardless -- a real producer
// publishes telemetry and sweeps its watchdog on its own schedule, whether
// or not a SUBSCRIBE happened to land that same instant.

#include <btp/aead.hpp>
#include <btp/node.hpp>

namespace {
    // Both peers need the SAME key -- sender.cpp and receiver.cpp hardcode
    // the same 16 bytes here for the demo. A real deployment provisions
    // each pair's key out of band (radio pairing, a manifest, a factory
    // step) -- never like this, and never the same key for every pair.
    const std::uint8_t kDemoAeadKey[btp::kAesGcmKeySize] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    };
    // Built once and handed to node.begin() below -- this peer's side of
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

    // node.enable_commands() calls this for a Fresh COMMAND_REQUEST (docs/
    // commands.md section 2) -- SYNCHRONOUSLY, no "pending, complete later"
    // path, so a slow action belongs on a task of its own that answers once
    // it's done. `outcome` arrives pre-set to Success / no message / no
    // result -- good news needs no field touched at all.
    void handle_command(void* /*ctx*/, std::uint16_t action_id, std::uint16_t /*action_version*/, btp::ByteView /*parameters*/, btp::NodeActionOutcome* outcome) {
        switch (action_id) {
            case 0x0001U:  // e.g. "stop" -- no parameters
                // ... actually stop the robot ...
                break;     // outcome is already Success
            default:
                outcome->status = static_cast<std::uint8_t>(btp::ResultStatus::Rejected);
                outcome->error_code = static_cast<std::uint16_t>(btp::ResultError::NotFound);
                break;
        }
    }

    // TERMINAL has no other btp::Node support -- its payload is opaque
    // bytes, no struct, on purpose (docs/session-and-terminal.md section
    // 7). node.on_terminal() below is what gets this called for a
    // TERMINAL_IN frame at all; `node`/`now_ms` arrive straight from
    // receive() itself, so answering back needs nothing threaded in via
    // `ctx`. Here it just echoes the bytes back as TERMINAL_OUT -- a real
    // shell would feed `payload` to a line editor instead.
    void handle_terminal_in(void* /*ctx*/, btp::Node& node, const btp::Header& /*header*/, btp::ByteView payload, std::uint64_t now_ms) {
        node.send(btp::MessageType::Terminal, btp::object_id::kTerminalOut, payload.data, payload.size, now_ms * 1000ULL);
    }

    // EndpointSealFn -- `context` is kDemoAeadKey. CIPHER_ID on `header` is
    // already AesGcm (fragmenting_flags() never sets those bits, and AesGcm
    // is 0), so aead_seal() dispatches there on its own; the KEY is the only
    // thing living here, never inside BTP itself.
    bool seal_message(void* context, const btp::Header& header, std::uint16_t payload_size, const std::uint8_t* plaintext, std::uint8_t* out) {
        const btp::AeadKey key{static_cast<const std::uint8_t*>(context), btp::kAesGcmKeySize};
        return btp::aead_seal(key, header, payload_size, plaintext, out) == btp::AeadError::Ok;
    }

    // NodeOpenFn -- the mirror; same key, dispatched by whatever CIPHER_ID
    // the sender actually sealed with.
    bool open_message(void* context, const btp::Header& header, std::uint16_t payload_size, const std::uint8_t* plaintext, std::uint8_t* out_plaintext) {
        const btp::AeadKey key{static_cast<const std::uint8_t*>(context),
                               btp::kAesGcmKeySize};
        return btp::aead_open(key, header, payload_size, plaintext, out_plaintext) == btp::AeadError::Ok;
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
    // The node's own configuration -- its identity, the transport it targets,
    // and the callbacks it needs to send, seal, open, and handle commands /
    btp::NodeConfig cfg = {};
    // config to set the node's identity, transport, and callbacks
    cfg.source_id = 0x00CAFE01U;                                // this robot    (non-zero)
    cfg.boot_id   = 0x0000B001U;                                // new each boot (non-zero) // attention: to use criptography, you need count the boots
    cfg.transport = btp::TransportLimits{250U, true};           // max_frame_size (payload derives automatically, max_frame_size - 40), allow_encrypted -- set by hand for whatever your link's MTU actually is, no named preset required
    cfg.send      = &send_frame;                                // function to send the frame using your trasport metod
    cfg.command   = &handle_command;                            // function to handle the command request, the node will call it when a COMMAND_REQUEST arrives
    cfg.terminal  = &handle_terminal_in;                        // function to handle the terminal input, the node will call it when a TERMINAL_IN arrives
    cfg.seal      = &seal_message;                              // function to seal the message, the node will call it when a message needs to be sealed
    cfg.open      = &open_message;                              // function to open the message, the node will call it when a message needs to be opened
    cfg.seal_ctx  = const_cast<std::uint8_t*>(kDemoAeadKey);    // void* ctx never modifies it
    cfg.open_ctx  = const_cast<std::uint8_t*>(kDemoAeadKey);    // void* ctx never modifies it

    btp::StaticNode<> node(cfg);

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

    if (!node.begin("example-robot", make_hello(btp::Role::Producer))) {
        return 1;
    }

    // in your code, you need to call the clock, and get the time
    std::uint64_t now_ms = 0U;

    std::uint8_t datagram[256];
    for (;;) {
        const std::size_t n = link_poll(datagram, sizeof datagram);

        // datagram (if any) -> receive(); either way, publish due topics +
        // session watchdog. COMMAND_REQUEST / TERMINAL_IN are both handled
        // inside routine() itself now -- handle_command() / handle_terminal_in()
        // above already ran, nothing left to check here, so no *out to pass.
        node.routine(datagram, n, now_ms);

        // remember to update the clock in your code, and get the time
        now_ms += 1U;
    }
    return 0;
}
