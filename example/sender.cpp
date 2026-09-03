// example/sender.cpp
//
// The producer half, with btp::Node. It exposes one topic through a
// btp::Catalog:
//
//   node.announce_catalog()  -> a MANIFEST_DATA describing topic 0x0101
//   node.publish(0x0101, ..) -> a typed TELEMETRY sample of it
//
// A consumer that has never met this producer learns the schema from the
// announcement and decodes the sample against it. The schema is written ONCE,
// here, as the producer's own data model.
//
//   cd example && cmake -B build && cmake --build build
//   ./build/sender      writes frame.bin (manifest frame, then sample frame)
//   ./build/receiver    reads it back, schema first
//
// by_hand_sender.cpp is the wire-level walkthrough of a single sample.

#include <btp/node.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

const std::uint16_t kTopicId = 0x0101U;

btp::ByteView text(const char* s) {
    return btp::ByteView{reinterpret_cast<const std::uint8_t*>(s),
                         std::strlen(s)};
}

// THE schema, defined once: a btp::FieldRecord per field. The catalogue holds
// it; announce_catalog() serialises it into MANIFEST_DATA and publish() encodes
// samples against it.
btp::FieldRecord field(std::uint16_t id, std::uint16_t order, btp::WireType type,
                       std::uint8_t flags, double scale, const char* name,
                       const char* unit) {
    btp::FieldRecord f = {};
    f.field_id = id;
    f.order = order;
    f.type = static_cast<std::uint8_t>(type);
    f.flags = flags;
    f.element_count = 1U;
    f.scale = scale;
    f.name = text(name);
    f.unit = text(unit);
    return f;
}

const btp::FieldRecord kSchema[] = {
    field(1, 0, btp::WireType::Float32, 0,                   1.0,   "left_rpm",  "rpm"),
    field(2, 1, btp::WireType::Float32, 0,                   1.0,   "right_rpm", "rpm"),
    field(3, 2, btp::WireType::Uint16,  0,                   0.001, "battery_v", "V"),
    field(4, 3, btp::WireType::Int16,   btp::kFieldNullable, 0.1,   "temp_c",    "Cel"),
};
const std::size_t kFieldCount = sizeof(kSchema) / sizeof(kSchema[0]);

// "Transmit" one frame: length-prefixed into frame.bin so the receiver can
// split the frames. A packet transport delivers whole datagrams and needs no
// such prefix.
bool transmit(void* /*ctx*/, const std::uint8_t* frame, std::size_t size) {
    std::FILE* f = std::fopen("frame.bin", "ab");
    if (f == nullptr) return false;
    const std::uint8_t len[2] = {static_cast<std::uint8_t>(size & 0xFFU),
                                 static_cast<std::uint8_t>(size >> 8)};
    std::fwrite(len, 1, 2, f);
    std::fwrite(frame, 1, size, f);
    std::fclose(f);
    std::printf("  transmitted %lu octets\n", static_cast<unsigned long>(size));
    return true;
}

// publish() calls this to write the field values, in schema order, into an open
// SampleWriter. put_f64 takes the engineering value (battery_v 3.72 is stored
// as the raw Uint16 3720); put_null marks the offline temp_c absent.
void fill_drive_status(void* /*ctx*/, btp::SampleWriter& writer) {
    writer.put_f64(1450.0);   // left_rpm
    writer.put_f64(-1448.5);  // right_rpm
    writer.put_f64(3.72);     // battery_v
    writer.put_null();        // temp_c
}

}  // namespace

int main() {
    std::remove("frame.bin");  // transmit() appends -- start from empty

    btp::NodeConfig config = {};
    config.source_id = 0x00CAFE01U;  // this robot; non-zero
    config.boot_id = 0x0000B001U;    // changes every reboot
    config.transport = btp::TransportProfile::EspNow;
    config.send = &transmit;

    btp::StaticNode<> node(config);

    // The catalogue: one topic, its schema, its config revision.
    btp::StaticCatalog<> catalog;
    catalog.set_config_revision(1U);
    if (catalog.add_topic(kTopicId, /*schema_version=*/3U,
                          btp::TelemetryEncoding::PackedLe, /*subscribable=*/true,
                          /*max_rate_millihz=*/0U, "drive_status", kSchema,
                          kFieldCount) != btp::MessageError::Ok) {
        std::printf("catalog rejected the schema\n");
        return 1;
    }

    const std::uint8_t uuid[16] = {0xC0, 0xFF, 0xEE, 0x01, 0, 0, 0, 0,
                                   0,    0,    0,    0,    0, 0, 0, 1};
    node.serve_catalog(&catalog, static_cast<std::uint8_t>(btp::Role::Producer),
                       uuid, "example-robot");
    if (!node.begin()) {
        std::printf("node configuration rejected\n");
        return 1;
    }

    // 1 -- announce the schema (a MANIFEST_DATA with no request behind it).
    std::printf("announcing MANIFEST_DATA for topic 0x%04X:\n", kTopicId);
    if (!node.announce_catalog()) {
        std::printf("announce failed\n");
        return 1;
    }

    // 2 -- publish a typed sample. The node finds the schema in the catalogue,
    // runs the SampleWriter, encodes and sends the frame.
    std::printf("publishing TELEMETRY topic 0x%04X:\n", kTopicId);
    if (!node.publish(kTopicId, &fill_drive_status, nullptr,
                      /*timestamp_us=*/1700000000000000ULL)) {
        std::printf("publish failed\n");
        return 1;
    }

    std::printf("wrote frame.bin -- now run ./receiver\n");
    return 0;
}
