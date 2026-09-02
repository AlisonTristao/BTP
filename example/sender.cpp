// example/sender.cpp
//
// The producer half of a BTP exchange: turn one telemetry reading into the
// exact octets that go on the wire.
//
//   cd example && cmake -B build && cmake --build build
//   ./build/sender      writes frame.bin
//   ./build/receiver    reads it back
//
// ===========================================================================
// What we are sending, as JSON
// ===========================================================================
//
// A robot publishes one sample of topic 0x0101 "drive_status", schema_version 3:
//
//   {
//     "left_rpm":   1450.0,     // Float32,  rev/min
//     "right_rpm": -1448.5,     // Float32,  rev/min  (this wheel runs reversed)
//     "battery_v":  3.72,       // stored as Uint16 millivolts, scale 0.001 -> volts
//     "temp_c":     null        // Int16, scale 0.1, nullable -- sensor is offline
//   }
//
// BTP puts neither that JSON nor a C struct on the wire. Text breaks on the
// first payload byte that is a newline; a shipped struct breaks on the first
// different compiler or alignment. BTP puts:
//
//   +--------------------+------------------------+-----------+
//   | header (36 octets) | PACKED_LE body (N)     | CRC32 (4) |
//   +--------------------+------------------------+-----------+
//     who / when / topic    the 4 values, little-endian,
//                           against a schema both ends agree on
//
// so a microcontroller and a desktop exchange identical octets, with no ABI to
// negotiate and nothing that a 0x0A can corrupt.
// ===========================================================================

#include <btp/codec.hpp>
#include <btp/telemetry.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

// Step 0 -- the schema. The producer has this compiled in; a consumer either
// has the same table or rebuilds it from the topic's FieldRecords in a
// MANIFEST_DATA (docs/telemetry.md, btp::messages). Per field:
//   field_id           unique within the topic, non-zero
//   order              0, 1, 2, ... in array order
//   type               a btp::WireType
//   flags              btp::kFieldNullable / btp::kFieldVariableCount
//   element_count      1 for a scalar
//   max_element_count  0 unless VARIABLE_COUNT
//   scale, offset      engineering value = raw * scale + offset
const btp::FieldSpec kSchema[] = {
    // id  ord  type                                               flags                n  max  scale   offset
    {   1,   0, static_cast<std::uint8_t>(btp::WireType::Float32),  0,                   1,  0,  1.0,    0.0 },
    {   2,   1, static_cast<std::uint8_t>(btp::WireType::Float32),  0,                   1,  0,  1.0,    0.0 },
    {   3,   2, static_cast<std::uint8_t>(btp::WireType::Uint16),   0,                   1,  0,  0.001,  0.0 },
    {   4,   3, static_cast<std::uint8_t>(btp::WireType::Int16),    btp::kFieldNullable, 1,  0,  0.1,    0.0 },
};
const std::size_t kSchemaFields = sizeof(kSchema) / sizeof(kSchema[0]);

const std::uint16_t kTopicId = 0x0101U;       // the frame's object_id for TELEMETRY
const std::uint16_t kSchemaVersion = 3U;

void dump_hex(const char* label, const std::uint8_t* data, std::size_t n) {
    std::printf("%s (%lu octets)\n  ", label, static_cast<unsigned long>(n));
    for (std::size_t i = 0; i < n; ++i) {
        std::printf("%02x ", data[i]);
        if ((i + 1) % 16 == 0 && i + 1 < n) {
            std::printf("\n  ");
        }
    }
    std::printf("\n\n");
}

}  // namespace

int main() {
    // -----------------------------------------------------------------------
    // Step 1 -- encode the sample body: the 4 values -> PACKED_LE octets.
    //
    // put_f64() takes the *engineering* value and applies the inverse of the
    // schema conversion, so battery_v 3.72 with scale 0.001 is stored as the
    // raw integer 3720 in a Uint16. put_null() marks the nullable temp_c as
    // absent -- its presence bit stays 0 and no value is written for it.
    // -----------------------------------------------------------------------
    std::uint8_t body[64];
    btp::SampleWriter writer(body, sizeof(body), kSchema, kSchemaFields);
    writer.begin(kSchemaVersion);   // LogicalPayload: writes schema_version + the presence bitmap
    writer.put_f64(1450.0);         // left_rpm
    writer.put_f64(-1448.5);        // right_rpm
    writer.put_f64(3.72);           // battery_v  -> raw 3720
    writer.put_null();              // temp_c

    std::size_t body_size = 0;
    if (writer.finish(&body_size) != btp::MessageError::Ok) {
        std::printf("sample encode failed\n");
        return 1;
    }
    dump_hex("TELEMETRY logical payload   [schema_version | bitmap | values]",
             body, body_size);

    // -----------------------------------------------------------------------
    // Step 2 -- build the BTP header.
    //
    //   {
    //     "type":          "TELEMETRY",
    //     "source_id":     "0x00CAFE01",       // this robot; non-zero
    //     "boot_id":       "0x0000B001",       // changes every reboot
    //     "sequence":      42,                 // per-source, shared by all message types
    //     "timestamp_us":  1700000000000000,   // set by the producer, at measurement time
    //     "object_id":     "0x0101",           // the topic id
    //     "fragment_count": 1                  // 1, not 0, when the payload is not fragmented
    //   }
    // -----------------------------------------------------------------------
    btp::Header header = {};
    header.type = btp::MessageType::Telemetry;
    header.source_id = 0x00CAFE01U;
    header.boot_id = 0x0000B001U;
    header.sequence = 42U;
    header.timestamp_us = 1700000000000000ULL;   // your app fills this from its own monotonic/wall clock
    header.object_id = kTopicId;
    header.fragment_count = 1U;

    // -----------------------------------------------------------------------
    // Step 3 -- encode the frame: 36-octet header + body + CRC-32, for the
    // ESP-NOW profile (one datagram == one frame). The Serial profile would
    // additionally wrap this in COBS, USB HID in 64-octet reports -- the
    // header and payload semantics are identical across all three.
    // -----------------------------------------------------------------------
    const btp::Frame frame = { header, { body, body_size } };
    std::uint8_t wire[btp::kEspNowMaxFrameSize];
    std::size_t wire_size = 0;
    const btp::Error err = btp::encode(frame, btp::TransportProfile::EspNow,
                                       wire, sizeof(wire), &wire_size);
    if (err != btp::Error::Ok) {
        std::printf("frame encode failed: %s\n", btp::error_string(err));
        return 1;
    }
    dump_hex("BTP frame on the wire   [header 36 | payload | crc32 4]",
             wire, wire_size);

    // -----------------------------------------------------------------------
    // Step 4 -- "transmit". Here that is a file the receiver example reads.
    // -----------------------------------------------------------------------
    std::FILE* f = std::fopen("frame.bin", "wb");
    if (f == nullptr) {
        std::printf("cannot open frame.bin for writing\n");
        return 1;
    }
    std::fwrite(wire, 1, wire_size, f);
    std::fclose(f);
    std::printf("wrote frame.bin (%lu octets) -- now run ./receiver\n",
                static_cast<unsigned long>(wire_size));
    return 0;
}
