// example/sender.cpp
//
// The producer half of a BTP exchange, with btp::Node -- the friendly facade.
// It puts the same reading on the wire as by_hand_sender.cpp: the node fills in
// the identity, the sequence, the 36-octet header, the fragment count and the
// frame encoding (and the "a zero-initialised Header is not valid" trap goes
// away with them). You bring the sample and a way to transmit bytes.
//
//   cd example && cmake -B build && cmake --build build
//   ./build/sender      writes frame.bin
//   ./build/receiver    reads it back
//
// by_hand_sender.cpp is the same exchange step by step at the wire level, and
// its top comment explains the schema and why BTP puts neither JSON nor a C
// struct on the wire.

#include <btp/node.hpp>
#include <btp/telemetry.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

// The schema both ends agree on. Per field: id, order, wire type, flags,
// element_count, max_element_count, scale, offset -- see by_hand_sender.cpp.
const btp::FieldSpec kSchema[] = {
    {1, 0, static_cast<std::uint8_t>(btp::WireType::Float32), 0,                   1, 0, 1.0,   0.0},
    {2, 1, static_cast<std::uint8_t>(btp::WireType::Float32), 0,                   1, 0, 1.0,   0.0},
    {3, 2, static_cast<std::uint8_t>(btp::WireType::Uint16),  0,                   1, 0, 0.001, 0.0},
    {4, 3, static_cast<std::uint8_t>(btp::WireType::Int16),   btp::kFieldNullable, 1, 0, 0.1,   0.0},
};
const std::size_t kSchemaFields = sizeof(kSchema) / sizeof(kSchema[0]);

// "Transmit" one fully encoded frame. A real producer hands these octets to a
// radio / UART / HID; the example appends them to a file the receiver reads.
// The node calls this once per frame (once here -- the body fits one frame).
bool transmit(void* /*ctx*/, const std::uint8_t* frame, std::size_t size) {
    std::FILE* f = std::fopen("frame.bin", "ab");
    if (f == nullptr) return false;   // returning false stops the send
    std::fwrite(frame, 1, size, f);
    std::fclose(f);
    std::printf("  transmitted %lu octets\n", static_cast<unsigned long>(size));
    return true;
}

}  // namespace

int main() {
    std::remove("frame.bin");  // transmit() appends -- start from empty

    // Step 1 -- the sample body: the 4 values -> PACKED_LE octets, against the
    // schema. put_f64() takes the engineering value (battery_v 3.72 is stored
    // as the raw Uint16 3720); put_null() marks the offline temp_c absent.
    std::uint8_t body[64];
    btp::SampleWriter writer(body, sizeof(body), kSchema, kSchemaFields);
    writer.begin(/*schema_version=*/3);
    writer.put_f64(1450.0);
    writer.put_f64(-1448.5);
    writer.put_f64(3.72);
    writer.put_null();
    std::size_t body_size = 0;
    if (writer.finish(&body_size) != btp::MessageError::Ok) {
        std::printf("sample encode failed\n");
        return 1;
    }

    // Step 2 -- the node. One identity, one transport profile, one send
    // callback. StaticNode<> owns its buffers. That is the whole setup; there
    // is no header to fill in and no sequence to track.
    btp::NodeConfig config = {};
    config.source_id = 0x00CAFE01U;  // this robot; non-zero
    config.boot_id = 0x0000B001U;    // changes every reboot
    config.transport = btp::TransportProfile::EspNow;
    config.send = &transmit;

    btp::StaticNode<> node(config);
    if (!node.begin()) {
        std::printf("node configuration rejected\n");
        return 1;
    }

    // Step 3 -- send it. The node reserves the sequence, builds the canonical
    // header, fragments if the body needs it, encodes each frame with its
    // CRC-32 and hands it to transmit(). The timestamp is the producer's, set
    // at measurement time.
    std::printf("sending TELEMETRY topic 0x0101 (%lu-octet body):\n",
                static_cast<unsigned long>(body_size));
    if (!node.send(btp::MessageType::Telemetry, /*object_id=*/0x0101U, body,
                   body_size, /*timestamp_us=*/1700000000000000ULL)) {
        std::printf("send failed\n");
        return 1;
    }

    std::printf("wrote frame.bin -- now run ./receiver\n");
    return 0;
}
