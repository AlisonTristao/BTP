// example/receiver.cpp
//
// The consumer half, with btp::Node. Run ./build/sender first.
//
// This node has NEVER seen the producer's schema. node.learn_catalog() points
// it at a btp::Catalog; from then on the node ingests every MANIFEST_DATA into
// it, and -- with node.on_sample() set -- decodes each TELEMETRY sample against
// the learned schema. Nothing about topic 0x0101 is written here.
//
// by_hand_receiver.cpp walks the same decode step by step at the wire level.

#include <btp/node.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

// The node calls this for every TELEMETRY sample of a topic in the catalogue --
// values already converted (raw * scale + offset), matched to the field names
// the manifest carried.
void print_sample(void* /*ctx*/, const btp::CatalogTopic& topic,
                  btp::SampleReader& reader) {
    std::printf("TELEMETRY topic 0x%04X \"%s\":\n  {", topic.topic_id,
                topic.name);
    btp::SampleValue v = {};
    int i = 0;
    while (reader.next(&v) == btp::SampleStep::Item) {
        const char* name = topic.field_names != nullptr
                               ? topic.field_names[v.field->order]
                               : "";
        std::printf("%s\n    \"%s\": ", (i != 0) ? "," : "", name);
        if (v.is_null) {
            std::printf("null");
        } else {
            std::printf("%g", v.f64(0));
        }
        ++i;
    }
    std::printf("\n  }\n");
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
    btp::StaticCatalog<> catalog;   // the node fills this from MANIFEST_DATA
    node.learn_catalog(&catalog);
    node.on_sample(&print_sample, nullptr);
    if (!node.begin()) {
        std::printf("node configuration rejected\n");
        std::fclose(f);
        return 1;
    }

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
        const btp::NodeRx rx = node.receive(datagram, size, /*now_ms=*/0, &msg);
        switch (rx) {
            case btp::NodeRx::CatalogUpdated:
                std::printf("learned %lu topic(s) from MANIFEST_DATA "
                            "(config_revision %lu)\n",
                            static_cast<unsigned long>(catalog.topic_count()),
                            static_cast<unsigned long>(catalog.config_revision()));
                break;
            case btp::NodeRx::SampleDelivered:
                break;  // print_sample already ran
            case btp::NodeRx::Ignored:
                std::printf("(a sample arrived before its manifest)\n");
                break;
            case btp::NodeRx::Pending:
                break;  // a fragment -- more to come
            default:
                std::printf("frame: %s\n", btp::node_rx_string(rx));
                break;
        }
    }

    std::fclose(f);
    std::printf("\nok\n");
    return 0;
}
