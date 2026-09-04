// Unit tests for btp::Endpoint -- the transmit pipeline of docs/library.md
// chapter 14: local identity, the outgoing sequence counter and
// seal -> fragment -> encode.
//
// This is orchestration over btp::codec / btp::fragmentation, not a new wire
// layout, so there is no vector tree (btp::Reassembler has none either). Every
// test drives the endpoint and then decodes what it produced with the public
// btp::decode() / btp::Reassembler, so a frame the endpoint emits is only
// "correct" if the receive path accepts it and reconstructs the payload.

#include "btp/endpoint.hpp"

#include "btp/codec.hpp"
#include "btp/fragmentation.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << __FILE__ << ':' << __LINE__                            \
                      << ": check failed: " #condition << '\n';                 \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

using btp::Endpoint;
using btp::LogicalMessage;

// A sink the endpoint's EndpointSendFn writes into: every frame handed to it,
// copied, in order. `fail_at` makes the Nth send() return false.
struct Sink {
    std::vector<std::vector<std::uint8_t>> frames;
    std::size_t fail_at = 0U;  // 0 == never fail
    std::size_t calls = 0U;

    static bool send(void* context, const std::uint8_t* frame,
                     std::size_t size) {
        auto* self = static_cast<Sink*>(context);
        ++self->calls;
        if (self->fail_at != 0U && self->calls == self->fail_at) return false;
        self->frames.emplace_back(frame, frame + size);
        return true;
    }
};

// A seal that appends `tag_size` bytes of 0xAB. Records how many times it ran
// and the header it last saw, so a test can assert "sealed exactly once, over
// the canonical header".
struct FakeSeal {
    std::size_t calls = 0U;
    btp::Header last_header{};
    std::uint16_t last_payload_size = 0U;
    bool refuse = false;

    static bool seal(void* context, const btp::Header& header,
                     std::uint16_t payload_size, const std::uint8_t* plaintext,
                     std::uint8_t* out) {
        auto* self = static_cast<FakeSeal*>(context);
        ++self->calls;
        self->last_header = header;
        self->last_payload_size = payload_size;
        if (self->refuse) return false;
        std::memcpy(out, plaintext, payload_size);
        std::memset(out + payload_size, 0xABU, btp::kEndpointAeadTagSize);
        return true;
    }
};

std::vector<std::uint8_t> make_payload(std::size_t size, std::uint8_t seed) {
    std::vector<std::uint8_t> v(size);
    for (std::size_t i = 0U; i < size; ++i) {
        v[i] = static_cast<std::uint8_t>(seed + i);
    }
    return v;
}

LogicalMessage message(const std::vector<std::uint8_t>& payload) {
    LogicalMessage m{};
    m.type = btp::MessageType::Telemetry;
    m.object_id = 0x0042U;
    m.timestamp_us = 0x0102030405060708ULL;
    m.payload = {payload.data(), payload.size()};
    return m;
}

// Feeds every frame in `sink` through decode + reassembly and returns the one
// reconstructed logical payload (asserting exactly one completes).
std::vector<std::uint8_t> reassemble(const Sink& sink, btp::Header* header_out) {
    btp::ReassemblySlot slots[2];
    std::uint8_t storage_bytes[2][8192];
    btp::ReassemblyStorage storage[2] = {
        {storage_bytes[0], sizeof(storage_bytes[0])},
        {storage_bytes[1], sizeof(storage_bytes[1])},
    };
    btp::Reassembler reassembler(slots, storage, 2U, 1000U);

    std::vector<std::uint8_t> result;
    int completed = 0;
    for (const auto& frame : sink.frames) {
        btp::DecodedFrame decoded{};
        CHECK(btp::decode(frame.data(), frame.size(),
                          btp::kEspNowTransport, &decoded) ==
              btp::Error::Ok);
        if ((decoded.header.flags & btp::kFlagFragmented) == 0U) {
            if (header_out != nullptr) *header_out = decoded.header;
            result.assign(decoded.payload.data,
                          decoded.payload.data + decoded.payload.size);
            ++completed;
            continue;
        }
        btp::ReassembledMessage message{};
        const btp::ReassemblyEvent event =
            reassembler.push(decoded, 1U, &message);
        if (event == btp::ReassemblyEvent::Complete) {
            if (header_out != nullptr) *header_out = message.header;
            result.assign(message.payload.data,
                          message.payload.data + message.payload.size);
            reassembler.release(message.slot_index);
            ++completed;
        } else {
            CHECK(event == btp::ReassemblyEvent::Accepted);
        }
    }
    CHECK(completed == 1);
    return result;
}

// ---------------------------------------------------------------------------

void test_configure_and_identity() {
    Endpoint endpoint;
    CHECK(!endpoint.configured());
    CHECK(!endpoint.configure(0U, 5U));
    CHECK(!endpoint.configure(5U, 0U));
    CHECK(!endpoint.configured());

    CHECK(endpoint.configure(0x11223344U, 0x55667788U));
    CHECK(endpoint.configured());
    CHECK(endpoint.source_id() == 0x11223344U);
    CHECK(endpoint.boot_id() == 0x55667788U);

    std::uint32_t seq = 0U;
    CHECK(endpoint.reserve_sequence(&seq));
    CHECK(seq == 1U);
    CHECK(endpoint.reserve_sequence(&seq));
    CHECK(seq == 2U);

    // reconfigure resets the counter
    CHECK(endpoint.configure(1U, 1U));
    CHECK(endpoint.reserve_sequence(&seq));
    CHECK(seq == 1U);
}

void test_sequence_unconfigured_and_null() {
    Endpoint endpoint;
    std::uint32_t seq = 123U;
    CHECK(!endpoint.reserve_sequence(&seq));
    CHECK(!endpoint.try_reserve_sequence(&seq));
    CHECK(seq == 123U);

    CHECK(endpoint.configure(1U, 1U));
    CHECK(!endpoint.reserve_sequence(nullptr));
    CHECK(!endpoint.try_reserve_sequence(nullptr));
}

void test_send_logical_cleartext_single_frame() {
    Endpoint endpoint;
    CHECK(endpoint.configure(0xAABBCCDDU, 0x01020304U));

    const auto payload = make_payload(40U, 0x10U);
    Sink sink;
    CHECK(endpoint.send_logical(message(payload), btp::kEspNowTransport,
                                &Sink::send, &sink, nullptr, 0U));
    CHECK(sink.frames.size() == 1U);

    btp::Header header{};
    const auto out = reassemble(sink, &header);
    CHECK(out == payload);
    CHECK(header.type == btp::MessageType::Telemetry);
    CHECK(header.object_id == 0x0042U);
    CHECK(header.source_id == 0xAABBCCDDU);
    CHECK(header.boot_id == 0x01020304U);
    CHECK(header.sequence == 1U);
    CHECK(header.timestamp_us == 0x0102030405060708ULL);
    CHECK((header.flags & btp::kFlagFragmented) == 0U);
    CHECK((header.flags & btp::kFlagEncrypted) == 0U);
}

void test_send_logical_cleartext_multi_fragment() {
    Endpoint endpoint;
    CHECK(endpoint.configure(1U, 1U));

    // 3 ESP-NOW fragments (210 payload ceiling).
    const auto payload = make_payload(500U, 0x01U);
    Sink sink;
    CHECK(endpoint.send_logical(message(payload), btp::kEspNowTransport,
                                &Sink::send, &sink, nullptr, 0U));
    CHECK(sink.frames.size() == 3U);

    btp::Header header{};
    const auto out = reassemble(sink, &header);
    CHECK(out == payload);
    CHECK(header.sequence == 1U);
}

void test_send_logical_sealed_single_frame() {
    Endpoint endpoint;
    CHECK(endpoint.configure(0x0A0B0C0DU, 0x0E0F1011U));

    const auto payload = make_payload(30U, 0x70U);
    Sink sink;
    FakeSeal seal;
    std::uint8_t scratch[64];
    CHECK(endpoint.send_logical(message(payload), btp::kEspNowTransport,
                                &Sink::send, &sink, scratch, sizeof(scratch),
                                &FakeSeal::seal, &seal));

    CHECK(seal.calls == 1U);  // sealed once, whole message
    CHECK(seal.last_payload_size == 30U);
    CHECK(seal.last_header.fragment_index == 0U);
    CHECK(seal.last_header.fragment_count == 1U);
    CHECK((seal.last_header.flags & btp::kFlagEncrypted) != 0U);
    CHECK((seal.last_header.flags & btp::kFlagFragmented) == 0U);

    CHECK(sink.frames.size() == 1U);
    btp::Header header{};
    const auto sealed = reassemble(sink, &header);
    CHECK(sealed.size() == payload.size() + btp::kEndpointAeadTagSize);
    CHECK(std::memcmp(sealed.data(), payload.data(), payload.size()) == 0);
    for (std::size_t i = payload.size(); i < sealed.size(); ++i) {
        CHECK(sealed[i] == 0xABU);
    }
    CHECK((header.flags & btp::kFlagEncrypted) != 0U);
}

void test_send_logical_sealed_grows_across_fragment_boundary() {
    Endpoint endpoint;
    CHECK(endpoint.configure(1U, 1U));

    // 210 plaintext fits one ESP-NOW frame; +16 tag does not -> 2 fragments.
    const auto payload = make_payload(210U, 0x01U);
    Sink sink;
    FakeSeal seal;
    std::uint8_t scratch[256];
    CHECK(endpoint.send_logical(message(payload), btp::kEspNowTransport,
                                &Sink::send, &sink, scratch, sizeof(scratch),
                                &FakeSeal::seal, &seal));
    CHECK(seal.calls == 1U);
    CHECK(sink.frames.size() == 2U);

    btp::Header header{};
    const auto sealed = reassemble(sink, &header);
    CHECK(sealed.size() == 226U);
    CHECK((header.flags & btp::kFlagEncrypted) != 0U);
}

void test_send_logical_seal_refused_sends_nothing() {
    Endpoint endpoint;
    CHECK(endpoint.configure(1U, 1U));

    const auto payload = make_payload(30U, 0x01U);
    Sink sink;
    FakeSeal seal;
    seal.refuse = true;
    std::uint8_t scratch[64];
    CHECK(!endpoint.send_logical(message(payload), btp::kEspNowTransport,
                                 &Sink::send, &sink, scratch, sizeof(scratch),
                                 &FakeSeal::seal, &seal));
    CHECK(seal.calls == 1U);
    CHECK(sink.frames.empty());
    CHECK(sink.calls == 0U);
}

void test_send_logical_sealed_scratch_too_small() {
    Endpoint endpoint;
    CHECK(endpoint.configure(1U, 1U));

    const auto payload = make_payload(30U, 0x01U);
    Sink sink;
    FakeSeal seal;
    std::uint8_t scratch[40];  // needs 30 + 16 = 46
    CHECK(!endpoint.send_logical(message(payload), btp::kEspNowTransport,
                                 &Sink::send, &sink, scratch, sizeof(scratch),
                                 &FakeSeal::seal, &seal));
    CHECK(seal.calls == 0U);
    CHECK(sink.frames.empty());
}

void test_send_logical_unconfigured() {
    Endpoint endpoint;
    const auto payload = make_payload(10U, 0x01U);
    Sink sink;
    CHECK(!endpoint.send_logical(message(payload), btp::kEspNowTransport,
                                 &Sink::send, &sink, nullptr, 0U));
    CHECK(sink.frames.empty());
}

void test_send_stops_when_send_callback_fails() {
    Endpoint endpoint;
    CHECK(endpoint.configure(1U, 1U));

    const auto payload = make_payload(500U, 0x01U);  // 3 fragments
    Sink sink;
    sink.fail_at = 2U;  // second frame is rejected
    CHECK(!endpoint.send_logical(message(payload), btp::kEspNowTransport,
                                 &Sink::send, &sink, nullptr, 0U));
    CHECK(sink.calls == 2U);       // did not attempt the third
    CHECK(sink.frames.size() == 1U);
}

void test_send_logical_reserved_uses_given_sequence() {
    Endpoint endpoint;
    CHECK(endpoint.configure(1U, 1U));
    std::uint32_t burned = 0U;
    CHECK(endpoint.reserve_sequence(&burned));  // now the counter is at 2
    CHECK(burned == 1U);

    const auto payload = make_payload(20U, 0x01U);
    Sink sink;
    CHECK(endpoint.send_logical_reserved(99U, message(payload),
                                         btp::kEspNowTransport,
                                         &Sink::send, &sink, nullptr, 0U));
    btp::Header header{};
    reassemble(sink, &header);
    CHECK(header.sequence == 99U);

    // the internal counter was not advanced by the reserved send
    std::uint32_t next = 0U;
    CHECK(endpoint.reserve_sequence(&next));
    CHECK(next == 2U);

    CHECK(!endpoint.send_logical_reserved(0U, message(payload),
                                          btp::kEspNowTransport,
                                          &Sink::send, &sink, nullptr, 0U));
}

void test_encode_fragment_rejects_sealed_multifragment() {
    Endpoint endpoint;
    CHECK(endpoint.configure(1U, 1U));

    const auto payload = make_payload(20U, 0x01U);
    std::uint8_t out[btp::kEspNowMaxFrameSize];
    std::size_t written = 0U;
    FakeSeal seal;
    // fragment_count 2 with a sealer -> refused (the tag covers a whole message)
    CHECK(!endpoint.encode_fragment(message(payload),
                                    btp::kEspNowTransport, 1U, 0U, 2U,
                                    out, sizeof(out), &written, &FakeSeal::seal,
                                    &seal));
    CHECK(seal.calls == 0U);

    // fragment_count 1 is fine
    CHECK(endpoint.encode_fragment(message(payload),
                                   btp::kEspNowTransport, 1U, 0U, 1U, out,
                                   sizeof(out), &written, &FakeSeal::seal,
                                   &seal));
    CHECK(written > 0U);
    CHECK(seal.calls == 1U);
}

void test_encode_fragment_argument_checks() {
    Endpoint endpoint;
    std::uint8_t out[btp::kEspNowMaxFrameSize];
    std::size_t written = 0U;
    const auto payload = make_payload(10U, 0x01U);

    // unconfigured
    CHECK(!endpoint.encode_fragment(message(payload),
                                    btp::kEspNowTransport, 1U, 0U, 1U,
                                    out, sizeof(out), &written));
    CHECK(endpoint.configure(1U, 1U));
    // sequence 0
    CHECK(!endpoint.encode_fragment(message(payload),
                                    btp::kEspNowTransport, 0U, 0U, 1U,
                                    out, sizeof(out), &written));
    // fragment_index >= fragment_count
    CHECK(!endpoint.encode_fragment(message(payload),
                                    btp::kEspNowTransport, 1U, 2U, 2U,
                                    out, sizeof(out), &written));
    // output capacity too small
    CHECK(!endpoint.encode_fragment(message(payload),
                                    btp::kEspNowTransport, 1U, 0U, 1U,
                                    out, 4U, &written));
}

void test_send_encoded_passthrough() {
    Endpoint endpoint;
    CHECK(endpoint.configure(1U, 1U));

    // build a real frame to relay
    const auto payload = make_payload(12U, 0x01U);
    std::uint8_t frame[btp::kEspNowMaxFrameSize];
    std::size_t frame_size = 0U;
    CHECK(endpoint.encode_fragment(message(payload),
                                   btp::kEspNowTransport, 7U, 0U, 1U,
                                   frame, sizeof(frame), &frame_size));

    Sink sink;
    CHECK(endpoint.send_encoded(frame, frame_size, btp::kEspNowTransport,
                                &Sink::send, &sink));
    CHECK(sink.frames.size() == 1U);
    CHECK(sink.frames[0].size() == frame_size);
    CHECK(std::memcmp(sink.frames[0].data(), frame, frame_size) == 0);

    // too short / too long are rejected without a send
    Sink rejected;
    CHECK(!endpoint.send_encoded(frame, btp::kV1MinimumFrameSize - 1U,
                                 btp::kEspNowTransport, &Sink::send,
                                 &rejected));
    CHECK(!endpoint.send_encoded(frame, btp::kEspNowMaxFrameSize + 1U,
                                 btp::kEspNowTransport, &Sink::send,
                                 &rejected));
    CHECK(rejected.calls == 0U);
}

void test_null_callbacks() {
    Endpoint endpoint;
    CHECK(endpoint.configure(1U, 1U));
    const auto payload = make_payload(10U, 0x01U);
    CHECK(!endpoint.send_logical(message(payload), btp::kEspNowTransport,
                                 nullptr, nullptr, nullptr, 0U));
    CHECK(!endpoint.send_fragment(message(payload), btp::kEspNowTransport,
                                  1U, 0U, 1U, nullptr, nullptr));
    std::uint8_t frame[btp::kEspNowMaxFrameSize] = {0};
    CHECK(!endpoint.send_encoded(frame, btp::kV1MinimumFrameSize,
                                 btp::kEspNowTransport, nullptr,
                                 nullptr));
}

}  // namespace

int main() {
    test_configure_and_identity();
    test_sequence_unconfigured_and_null();
    test_send_logical_cleartext_single_frame();
    test_send_logical_cleartext_multi_fragment();
    test_send_logical_sealed_single_frame();
    test_send_logical_sealed_grows_across_fragment_boundary();
    test_send_logical_seal_refused_sends_nothing();
    test_send_logical_sealed_scratch_too_small();
    test_send_logical_unconfigured();
    test_send_stops_when_send_callback_fails();
    test_send_logical_reserved_uses_given_sequence();
    test_encode_fragment_rejects_sealed_multifragment();
    test_encode_fragment_argument_checks();
    test_send_encoded_passthrough();
    test_null_callbacks();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all btp::Endpoint checks passed\n";
    return 0;
}
