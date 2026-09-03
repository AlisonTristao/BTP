// example/sender.cpp
//
// The producer half, with btp::Node. It publishes two frames:
//
//   1. a MANIFEST_DATA that DESCRIBES topic 0x0101 -- every field's type,
//      order, scale, offset, nullability, name and unit;
//   2. a TELEMETRY sample of that topic.
//
// A consumer that has never seen this producer learns the schema from the
// manifest and decodes the sample against it -- nothing about the topic is
// hard-coded on the receiving side. The schema is written ONCE here, as the
// producer's own data model.
//
//   cd example && cmake -B build && cmake --build build
//   ./build/sender      writes frame.bin (manifest frame, then sample frame)
//   ./build/receiver    reads it back, schema first
//
// by_hand_sender.cpp is the wire-level walkthrough of a single sample.

#include <btp/messages.hpp>
#include <btp/node.hpp>
#include <btp/telemetry.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

const std::uint16_t kTopicId = 0x0101U;
const std::uint16_t kSchemaVersion = 3U;
const std::uint32_t kSourceId = 0x00CAFE01U;
const std::uint32_t kBootId = 0x0000B001U;

btp::ByteView text(const char* s) {
    return {reinterpret_cast<const std::uint8_t*>(s), std::strlen(s)};
}

// THE schema, defined once: a btp::FieldRecord per field. The manifest carries
// these; the sample codec narrows each to a btp::FieldSpec with field_spec().
btp::FieldRecord field(std::uint16_t id, std::uint16_t order, btp::WireType type,
                       std::uint8_t flags, double scale, const char* name,
                       const char* unit) {
    btp::FieldRecord f = {};
    f.field_id = id;
    f.order = order;
    f.type = static_cast<std::uint8_t>(type);
    f.flags = flags;
    f.element_count = 1;
    f.scale = scale;
    f.offset = 0.0;
    f.name = text(name);
    f.unit = text(unit);
    f.description = text("");
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
// split the two. A packet transport (ESP-NOW, USB HID) delivers whole
// datagrams and needs no such prefix.
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

// The MANIFEST_DATA payload describing our one topic.
std::size_t build_manifest(std::uint8_t* out, std::size_t capacity) {
    btp::ManifestHeader header = {};
    header.status = static_cast<std::uint8_t>(btp::ResultStatus::Success);
    header.flags = btp::kManifestCatalogComplete;
    header.manifest_format_version = 1U;
    header.config_revision = 1U;
    header.described_source_id = kSourceId;
    header.described_boot_id = kBootId;
    header.source_role = static_cast<std::uint8_t>(btp::Role::Producer);
    header.source_flags = btp::kSourceOnline;
    header.catalog_count = 1U;
    header.topic_count = 1U;
    header.source_name = text("example-robot");

    btp::TopicRecord topic = {};
    topic.topic_id = kTopicId;
    topic.schema_version = kSchemaVersion;
    topic.encoding = static_cast<std::uint8_t>(btp::TelemetryEncoding::PackedLe);
    topic.flags = btp::kTopicSubscribable;
    topic.field_count = static_cast<std::uint16_t>(kFieldCount);
    topic.name = text("drive_status");
    topic.description = text("wheel speeds, battery, temperature");

    btp::ManifestWriter w(out, capacity);
    if (w.begin(header) != btp::MessageError::Ok) return 0;
    if (w.begin_topic(topic) != btp::MessageError::Ok) return 0;
    for (std::size_t i = 0; i < kFieldCount; ++i) {
        if (w.add_field(kSchema[i]) != btp::MessageError::Ok) return 0;
    }
    if (w.end_topic() != btp::MessageError::Ok) return 0;
    std::size_t written = 0;
    return w.finish(&written) == btp::MessageError::Ok ? written : 0;
}

}  // namespace

int main() {
    std::remove("frame.bin");  // transmit() appends -- start from empty

    btp::NodeConfig config = {};
    config.source_id = kSourceId;
    config.boot_id = kBootId;
    config.transport = btp::TransportProfile::EspNow;
    config.send = &transmit;

    btp::StaticNode<> node(config);
    if (!node.begin()) {
        std::printf("node configuration rejected\n");
        return 1;
    }

    // 1 -- publish the schema, as a MANIFEST_DATA CONTROL message.
    std::uint8_t manifest[512];
    const std::size_t manifest_size = build_manifest(manifest, sizeof(manifest));
    if (manifest_size == 0) {
        std::printf("manifest build failed\n");
        return 1;
    }
    std::printf("sending MANIFEST_DATA for topic 0x%04X:\n", kTopicId);
    if (!node.send(btp::MessageType::Control, btp::object_id::kManifestData,
                   manifest, manifest_size, 1700000000000000ULL)) {
        std::printf("send failed\n");
        return 1;
    }

    // 2 -- the sample body, against the SAME schema (narrowed to the codec's
    // view with field_spec()).
    btp::FieldSpec spec[kFieldCount];
    for (std::size_t i = 0; i < kFieldCount; ++i) {
        spec[i] = btp::field_spec(kSchema[i]);
    }

    std::uint8_t body[64];
    btp::SampleWriter writer(body, sizeof(body), spec, kFieldCount);
    writer.begin(kSchemaVersion);
    writer.put_f64(1450.0);      // left_rpm
    writer.put_f64(-1448.5);     // right_rpm
    writer.put_f64(3.72);        // battery_v -> raw 3720
    writer.put_null();           // temp_c -- sensor offline
    std::size_t body_size = 0;
    if (writer.finish(&body_size) != btp::MessageError::Ok) {
        std::printf("sample encode failed\n");
        return 1;
    }

    std::printf("sending TELEMETRY topic 0x%04X (%lu-octet body):\n", kTopicId,
                static_cast<unsigned long>(body_size));
    if (!node.send(btp::MessageType::Telemetry, kTopicId, body, body_size,
                   1700000000000000ULL)) {
        std::printf("send failed\n");
        return 1;
    }

    std::printf("wrote frame.bin -- now run ./receiver\n");
    return 0;
}
