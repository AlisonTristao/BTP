// example/receiver.cpp
//
// The consumer half of a BTP exchange, with btp::Node. Run ./build/sender
// first (it writes frame.bin), then this.
//
// The node decodes the datagram, validates the magic / version / transport
// limits / CRC-32, and reassembles fragments; receive() hands back one whole
// logical message. You bring the schema and decide what the message means.
//
// by_hand_receiver.cpp is the same decode step by step at the wire level.

#include <btp/node.hpp>
#include <btp/telemetry.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

// The SAME schema the producer used.
const btp::FieldSpec kSchema[] = {
    {1, 0, static_cast<std::uint8_t>(btp::WireType::Float32), 0,                   1, 0, 1.0,   0.0},
    {2, 1, static_cast<std::uint8_t>(btp::WireType::Float32), 0,                   1, 0, 1.0,   0.0},
    {3, 2, static_cast<std::uint8_t>(btp::WireType::Uint16),  0,                   1, 0, 0.001, 0.0},
    {4, 3, static_cast<std::uint8_t>(btp::WireType::Int16),   btp::kFieldNullable, 1, 0, 0.1,   0.0},
};
const std::size_t kSchemaFields = sizeof(kSchema) / sizeof(kSchema[0]);
const char* const kFieldName[] = {"left_rpm", "right_rpm", "battery_v", "temp_c"};

void print_sample(const btp::ByteView& body) {
    btp::SampleReader reader(body.data, body.size, kSchema, kSchemaFields,
                             btp::kEncodingPackedLe);
    std::printf("  schema_version %u\n  {", reader.schema_version());
    btp::SampleValue v = {};
    int i = 0;
    while (reader.next(&v) == btp::SampleStep::Item) {
        std::printf("%s\n    \"%s\": ", (i++ != 0) ? "," : "",
                    kFieldName[v.field->order]);
        if (v.is_null) {
            std::printf("null");
        } else {
            std::printf("%g", v.f64(0));
        }
    }
    std::printf("\n  }\n");
    if (reader.finish() != btp::MessageError::Ok) {
        std::printf("  (sample body rejected: %s)\n",
                    btp::message_error_string(reader.error()));
    }
}

}  // namespace

int main() {
    // A packet transport delivers whole datagrams. The example sender wrote one
    // frame, so one read is the whole datagram.
    std::uint8_t datagram[btp::kEspNowMaxFrameSize];
    std::FILE* f = std::fopen("frame.bin", "rb");
    if (f == nullptr) {
        std::printf("cannot open frame.bin -- run ./sender first\n");
        return 1;
    }
    const std::size_t size = std::fread(datagram, 1, sizeof(datagram), f);
    std::fclose(f);
    std::printf("received %lu octets\n", static_cast<unsigned long>(size));

    btp::NodeConfig config = {};
    config.source_id = 0x00B0B0FEU;
    config.boot_id = 0x0000C0DEU;
    config.transport = btp::TransportProfile::EspNow;
    // no `send`: this node only receives (no producing, no session).

    btp::StaticNode<> node(config);
    if (!node.begin()) {
        std::printf("node configuration rejected\n");
        return 1;
    }

    btp::ReceivedMessage message = {};
    const btp::NodeRx outcome = node.receive(datagram, size, /*now_ms=*/0, &message);
    if (outcome != btp::NodeRx::Complete) {
        std::printf("receive: %s\n", btp::node_rx_string(outcome));
        return 1;
    }
    if (message.header.type != btp::MessageType::Telemetry) {
        std::printf("not a TELEMETRY frame\n");
        return 1;
    }

    std::printf("  source_id     0x%08lX\n",
                static_cast<unsigned long>(message.header.source_id));
    std::printf("  object_id     0x%04X   (topic)\n", message.header.object_id);
    std::printf("  timestamp_us  %llu   (the producer's)\n",
                static_cast<unsigned long long>(message.header.timestamp_us));
    print_sample(message.payload);

    std::printf("\nok\n");
    return 0;
}
