// example/receiver.cpp
//
// The consumer half, with btp::Node. Run ./build/sender first.
//
// This node has NEVER seen the producer's schema. It reads the MANIFEST_DATA
// frame, builds a btp::FieldSpec table from it (btp::field_spec() per
// FieldRecord), caches it, and then decodes the TELEMETRY sample against the
// cached schema. Nothing about topic 0x0101 is hard-coded here.
//
// by_hand_receiver.cpp is the wire-level walkthrough of a single sample.

#include <btp/messages.hpp>
#include <btp/node.hpp>
#include <btp/telemetry.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// The consumer's schema cache -- one topic is enough for the example. A real
// consumer keeps a fixed-capacity table keyed by (topic_id, schema_version)
// and refreshes it when config_revision moves (docs/library.md 11.2).
struct SchemaCache {
    bool valid;
    std::uint16_t topic_id;
    std::uint16_t schema_version;
    btp::FieldSpec fields[16];
    char names[16][24];
    std::size_t field_count;
};

void ingest_manifest(SchemaCache* cache, const btp::ByteView& payload) {
    btp::ManifestReader reader(payload.data, payload.size);
    btp::ManifestHeader header = {};
    if (reader.header(&header) != btp::MessageError::Ok) {
        std::printf("  manifest header rejected\n");
        return;
    }

    btp::TopicRecord topic = {};
    btp::ByteView field_bytes = {};
    while (reader.next_topic(&topic, &field_bytes) == btp::ManifestStep::Item) {
        btp::FieldRecordReader fields(field_bytes, topic.field_count);
        btp::FieldRecord record = {};
        btp::ByteView enum_bytes = {};
        std::size_t n = 0;
        while (n < 16 &&
               fields.next(&record, &enum_bytes) == btp::ManifestStep::Item) {
            cache->fields[n] = btp::field_spec(record);
            const std::size_t len = record.name.size < sizeof(cache->names[0])
                                        ? record.name.size
                                        : sizeof(cache->names[0]) - 1;
            std::memcpy(cache->names[n], record.name.data, len);
            cache->names[n][len] = '\0';
            ++n;
        }
        cache->valid = true;
        cache->topic_id = topic.topic_id;
        cache->schema_version = topic.schema_version;
        cache->field_count = n;
        std::printf("  learned topic 0x%04X \"", topic.topic_id);
        std::fwrite(topic.name.data, 1, topic.name.size, stdout);
        std::printf("\": %lu fields, schema_version %u\n",
                    static_cast<unsigned long>(n), topic.schema_version);
    }
    reader.finish();
}

void print_sample(const SchemaCache& cache, const btp::ByteView& payload) {
    btp::SampleReader reader(payload.data, payload.size, cache.fields,
                             cache.field_count, btp::kEncodingPackedLe);
    if (reader.schema_version() != cache.schema_version) {
        std::printf("  sample is schema_version %u, cached schema is %u\n",
                    reader.schema_version(), cache.schema_version);
        return;
    }
    std::printf("  {");
    btp::SampleValue v = {};
    int i = 0;
    while (reader.next(&v) == btp::SampleStep::Item) {
        std::printf("%s\n    \"%s\": ", (i != 0) ? "," : "",
                    cache.names[v.field->order]);
        if (v.is_null) {
            std::printf("null");
        } else {
            std::printf("%g", v.f64(0));
        }
        ++i;
    }
    std::printf("\n  }\n");
    if (reader.finish() != btp::MessageError::Ok) {
        std::printf("  (sample rejected: %s)\n",
                    btp::message_error_string(reader.error()));
    }
}

}  // namespace

int main() {
    std::FILE* f = std::fopen("frame.bin", "rb");
    if (f == nullptr) {
        std::printf("cannot open frame.bin -- run ./sender first\n");
        return 1;
    }

    btp::NodeConfig config = {};
    config.source_id = 0x00B0B0FEU;
    config.boot_id = 0x0000C0DEU;
    config.transport = btp::TransportProfile::EspNow;
    // no `send`: this node only receives.

    btp::StaticNode<> node(config);
    if (!node.begin()) {
        std::printf("node configuration rejected\n");
        std::fclose(f);
        return 1;
    }

    SchemaCache cache = {};

    // Each [uint16 length][frame] block in the file is "one datagram arrived".
    std::uint8_t length[2];
    while (std::fread(length, 1, 2, f) == 2) {
        const std::size_t size = static_cast<std::size_t>(length[0]) |
                                 (static_cast<std::size_t>(length[1]) << 8);
        std::uint8_t datagram[btp::kEspNowMaxFrameSize];
        if (size > sizeof(datagram) ||
            std::fread(datagram, 1, size, f) != size) {
            std::printf("truncated frame.bin\n");
            break;
        }

        btp::ReceivedMessage msg = {};
        if (node.receive(datagram, size, /*now_ms=*/0, &msg) !=
            btp::NodeRx::Complete) {
            continue;
        }

        if (msg.header.type == btp::MessageType::Control &&
            msg.header.object_id == btp::object_id::kManifestData) {
            std::printf("MANIFEST_DATA from 0x%08lX:\n",
                        static_cast<unsigned long>(msg.header.source_id));
            ingest_manifest(&cache, msg.payload);
        } else if (msg.header.type == btp::MessageType::Telemetry) {
            std::printf("TELEMETRY topic 0x%04X:\n", msg.header.object_id);
            if (cache.valid && msg.header.object_id == cache.topic_id) {
                print_sample(cache, msg.payload);
            } else {
                std::printf("  no schema for this topic yet\n");
            }
        }
    }

    std::fclose(f);
    std::printf("\nok\n");
    return 0;
}
