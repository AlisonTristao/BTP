// example/by_hand_receiver.cpp
//
// The consumer half of a BTP exchange, step by step at the wire level: turn the
// wire octets back into the telemetry reading. receiver.cpp does the same with
// btp::Node. Run ./build/by_hand_sender first (it writes frame.bin), then this.
//
// ===========================================================================
// What we expect to receive, as JSON -- the same reading sender.cpp built
// ===========================================================================
//
//   header:
//   { "type":"TELEMETRY", "source_id":"0x00CAFE01", "boot_id":"0x0000B001",
//     "sequence":42, "timestamp_us":1700000000000000, "object_id":"0x0101" }
//
//   body (topic 0x0101, schema_version 3):
//   {
//     "left_rpm":   1450.0,
//     "right_rpm": -1448.5,
//     "battery_v":  3.72,
//     "temp_c":     null
//   }
//
// The bytes carry neither that JSON nor a C struct (see sender.cpp for why).
// Decoding is two steps: frame -> header + payload, then payload -> values
// against the schema.
// ===========================================================================

#include <btp/codec.hpp>
#include <btp/telemetry.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

// Step 0 -- the SAME schema the producer used. Both ends must agree on it. A
// consumer without it compiled in rebuilds this table from the topic's
// FieldRecords in a MANIFEST_DATA (docs/telemetry.md, btp::messages), then
// caches it keyed by (topic_id, schema_version).
const btp::FieldSpec kSchema[] = {
    {   1,   0, static_cast<std::uint8_t>(btp::WireType::Float32),  0,                   1,  0,  1.0,    0.0 },
    {   2,   1, static_cast<std::uint8_t>(btp::WireType::Float32),  0,                   1,  0,  1.0,    0.0 },
    {   3,   2, static_cast<std::uint8_t>(btp::WireType::Uint16),   0,                   1,  0,  0.001,  0.0 },
    {   4,   3, static_cast<std::uint8_t>(btp::WireType::Int16),    btp::kFieldNullable, 1,  0,  0.1,    0.0 },
};
const std::size_t kSchemaFields = sizeof(kSchema) / sizeof(kSchema[0]);
const char* const kFieldName[] = { "left_rpm", "right_rpm", "battery_v", "temp_c" };

}  // namespace

int main() {
    // -----------------------------------------------------------------------
    // Step 1 -- receive the bytes. Here: read the file sender.cpp wrote.
    // -----------------------------------------------------------------------
    std::uint8_t wire[btp::kEspNowMaxFrameSize];
    std::FILE* f = std::fopen("frame.bin", "rb");
    if (f == nullptr) {
        std::printf("cannot open frame.bin -- run ./sender first\n");
        return 1;
    }
    const std::size_t wire_size = std::fread(wire, 1, sizeof(wire), f);
    std::fclose(f);
    std::printf("received %lu octets\n", static_cast<unsigned long>(wire_size));

    // -----------------------------------------------------------------------
    // Step 2 -- decode the frame. This validates the magic, the envelope
    // version, the transport limits and the CRC-32. On success
    // decoded.payload points straight into `wire` (zero-copy) and
    // decoded.header is normalised.
    // -----------------------------------------------------------------------
    btp::DecodedFrame decoded = {};
    const btp::Error err = btp::decode(wire, wire_size,
                                       btp::TransportProfile::EspNow, &decoded);
    if (err != btp::Error::Ok) {
        std::printf("frame decode failed: %s\n", btp::error_string(err));
        return 1;
    }

    // -----------------------------------------------------------------------
    // Step 3 -- read the header. timestamp_us was set by the PRODUCER at
    // measurement time, not on arrival -- carrying it is the point.
    // -----------------------------------------------------------------------
    if (decoded.header.type != btp::MessageType::Telemetry) {
        std::printf("not a TELEMETRY frame\n");
        return 1;
    }
    std::printf("  source_id     0x%08lX\n",
                static_cast<unsigned long>(decoded.header.source_id));
    std::printf("  boot_id       0x%08lX\n",
                static_cast<unsigned long>(decoded.header.boot_id));
    std::printf("  sequence      %lu\n",
                static_cast<unsigned long>(decoded.header.sequence));
    std::printf("  timestamp_us  %llu\n",
                static_cast<unsigned long long>(decoded.header.timestamp_us));
    std::printf("  object_id     0x%04X   (topic)\n", decoded.header.object_id);

    // -----------------------------------------------------------------------
    // Step 4 -- decode the sample body against the schema. next() yields
    // every schema field exactly once, in order; a nullable field with no
    // reading comes back as is_null. f64() applies raw * scale + offset.
    // finish() must be checked -- a structural fault rejects the whole sample
    // (no partial decode).
    // -----------------------------------------------------------------------
    btp::SampleReader reader(decoded.payload.data, decoded.payload.size,
                             kSchema, kSchemaFields, btp::kEncodingPackedLe);
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
        std::printf("sample body rejected: %s\n",
                    btp::message_error_string(reader.error()));
        return 1;
    }
    std::printf("\nok\n");
    return 0;
}
