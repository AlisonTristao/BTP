// example/sender.cpp
//
// The producer, with btp::Node. It owns ONE topic -- declared once, below, on
// the node's own catalogue -- and answers a connection from whoever asks:
//
//   node.enable_session(...) + arm_session()  -> answers HELLO with
//                                                HELLO_RESULT, then runs the
//                                                inactivity watchdog
//   node.announce_catalog()                   -> MANIFEST_DATA  "here is my schema"
//   node.publish_named(kDriveStatus, ..)       -> TELEMETRY      "here is a sample"
//
// receiver.cpp is the other end: it connect()s in and learns the schema from
// the wire, never from a shared header. The link here is faked -- send_frame()
// just drops the bytes; a real node hands them to ESP-NOW / a UART / USB-HID
// untouched. This file is the logic, not a runnable demo. by_hand_sender.cpp
// is the runnable wire-level walkthrough.

#include <btp/node.hpp>

namespace {

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

// publish_named() calls this to write one sample. Each put() must name the
// schema's NEXT field -- get the order wrong and it fails right there
// (MessageError::InvalidArgument) instead of sending the wrong value.
void fill_drive_status(void* /*ctx*/, btp::NamedSampleWriter& w) {
    w.put("left_rpm", 1450.0);
    w.put("right_rpm", -1448.5);
    w.put("battery_v", 3.72);   // 3.72 V <-> raw uint16 3720 (scale 0.001)
    w.put_null("temp_c");       // sensor offline
}

// The node hands every finished frame here. Fake: a real node transmits it.
bool send_frame(void* /*ctx*/, const std::uint8_t* /*frame*/, std::size_t /*n*/) {
    return true;
}

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
        .f32("left_rpm", "rpm")
        .f32("right_rpm", "rpm")
        .u16("battery_v", 0.001, "V")
        .i16("temp_c", 0.1, "Cel", /*is_nullable=*/true)
        .end();
    node.catalog().set_config_revision(1U);
    node.serve_catalog(static_cast<std::uint8_t>(btp::Role::Producer), nullptr,
                       "example-robot");
    node.enable_session(make_hello(btp::Role::Producer),
                        /*hello_deadline_ms=*/0U);

    node.begin();
    node.arm_session();

    node.announce_catalog();                          // -> MANIFEST_DATA
    node.publish_named(kDriveStatus, &fill_drive_status,
                       nullptr, /*timestamp_us=*/1700000000000000ULL);
    return 0;
}
