// Unit tests for btp::Receiver -- the decode + CRC + reassembly stage of
// docs/library.md chapter 15, the RX mirror of btp::Endpoint.
//
// Orchestration over btp::decode / btp::Reassembler, not a new wire layout, so
// there is no vector tree. Every test builds real frames (mostly with a
// btp::Endpoint, so the pair is exercised end to end) and feeds them to a
// Receiver.

#include "btp/receiver.hpp"

#include "btp/codec.hpp"
#include "btp/endpoint.hpp"

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
using btp::ReceivedMessage;
using btp::Receiver;
using btp::ReceiveOutcome;

constexpr std::size_t kSlots = 4;
constexpr std::size_t kRegion = 2048;  // per-slot storage

// A Receiver plus the caller-owned storage it needs.
struct Fixture {
    btp::ReassemblySlot slots[kSlots];
    std::uint8_t storage_bytes[kSlots][kRegion];
    btp::ReassemblyStorage storage[kSlots];
    std::uint8_t out[kRegion];
    Receiver receiver;

    explicit Fixture(std::uint64_t timeout_ms = 4000,
                     btp::TransportProfile transport = btp::TransportProfile::EspNow)
        : receiver(bind(), storage, kSlots, timeout_ms, transport) {}

    btp::ReassemblySlot* bind() noexcept {
        for (std::size_t i = 0; i < kSlots; ++i) {
            storage[i].data = storage_bytes[i];
            storage[i].capacity = kRegion;
        }
        return slots;
    }
};

// Captures every frame an Endpoint hands to its send callback.
struct Sink {
    std::vector<std::vector<std::uint8_t>> frames;
    static bool send(void* ctx, const std::uint8_t* f, std::size_t n) {
        static_cast<Sink*>(ctx)->frames.emplace_back(f, f + n);
        return true;
    }
};

std::vector<std::uint8_t> make_payload(std::size_t n, std::uint8_t seed) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>(seed + i);
    return v;
}

// Encodes `payload` as 1..N cleartext frames from a fresh endpoint identity.
std::vector<std::vector<std::uint8_t>> frames_for(std::uint32_t source_id,
                                                  std::uint32_t boot_id,
                                                  const std::vector<std::uint8_t>& payload) {
    Endpoint endpoint;
    endpoint.configure(source_id, boot_id);
    Sink sink;
    LogicalMessage msg{btp::MessageType::Telemetry, 0x0055U, 0x11223344U,
                       {payload.data(), payload.size()}};
    const bool ok = endpoint.send_logical(msg, btp::TransportProfile::EspNow,
                                          &Sink::send, &sink, nullptr, 0U);
    if (!ok) return {};
    return sink.frames;
}

ReceiveOutcome submit(Fixture& fx, const std::vector<std::uint8_t>& frame,
                      std::uint64_t now_ms, ReceivedMessage* msg) {
    return fx.receiver.submit(frame.data(), frame.size(), now_ms, fx.out,
                              sizeof(fx.out), msg);
}

// ---------------------------------------------------------------------------

void test_valid() {
    Fixture fx;
    CHECK(fx.receiver.valid());
    CHECK(fx.receiver.slot_count() == kSlots);

    // bad transport
    btp::ReassemblySlot slots[1];
    std::uint8_t bytes[1][64];
    btp::ReassemblyStorage storage[1] = {{bytes[0], sizeof(bytes[0])}};
    Receiver bad(slots, storage, 1, 4000,
                 static_cast<btp::TransportProfile>(99));
    CHECK(!bad.valid());

    // null slots
    Receiver bad2(nullptr, storage, 1, 4000, btp::TransportProfile::EspNow);
    CHECK(!bad2.valid());
}

void test_unfragmented() {
    Fixture fx;
    const auto payload = make_payload(40, 0x10);
    const auto frames = frames_for(0xAABBCCDDU, 0x01020304U, payload);
    CHECK(frames.size() == 1U);

    ReceivedMessage msg{};
    CHECK(submit(fx, frames[0], 1U, &msg) == ReceiveOutcome::Complete);
    CHECK(!msg.reassembled);
    CHECK(msg.header.source_id == 0xAABBCCDDU);
    CHECK(msg.header.object_id == 0x0055U);
    CHECK((msg.header.flags & btp::kFlagFragmented) == 0U);
    CHECK(msg.payload.size == payload.size());
    CHECK(std::memcmp(msg.payload.data, payload.data(), payload.size()) == 0);
    CHECK(msg.payload.data == fx.out);  // copied into the caller's buffer
    CHECK(fx.receiver.stats().completed == 1U);
}

void test_fragmented_in_order() {
    Fixture fx;
    const auto payload = make_payload(500, 0x01);  // 3 ESP-NOW fragments
    const auto frames = frames_for(1U, 1U, payload);
    CHECK(frames.size() == 3U);

    ReceivedMessage msg{};
    CHECK(submit(fx, frames[0], 1U, &msg) == ReceiveOutcome::FragmentAccepted);
    CHECK(submit(fx, frames[1], 1U, &msg) == ReceiveOutcome::FragmentAccepted);
    CHECK(submit(fx, frames[2], 1U, &msg) == ReceiveOutcome::Complete);
    CHECK(msg.reassembled);
    CHECK((msg.header.flags & btp::kFlagFragmented) == 0U);  // normalised
    CHECK(msg.payload.size == payload.size());
    CHECK(std::memcmp(msg.payload.data, payload.data(), payload.size()) == 0);

    const auto stats = fx.receiver.stats();
    CHECK(stats.fragments_accepted == 2U);
    CHECK(stats.completed == 1U);

    // the slot was released -- a second message goes through the same pool
    const auto p2 = make_payload(400, 0x80);
    const auto f2 = frames_for(1U, 1U, p2);
    ReceivedMessage msg2{};
    ReceiveOutcome last = ReceiveOutcome::InvalidArgument;
    for (const auto& fr : f2) last = submit(fx, fr, 2U, &msg2);
    CHECK(last == ReceiveOutcome::Complete);
    CHECK(msg2.payload.size == p2.size());
}

void test_fragmented_out_of_order() {
    Fixture fx;
    const auto payload = make_payload(450, 0x01);
    const auto frames = frames_for(7U, 3U, payload);
    CHECK(frames.size() == 3U);

    ReceivedMessage msg{};
    CHECK(submit(fx, frames[2], 1U, &msg) == ReceiveOutcome::FragmentAccepted);
    CHECK(submit(fx, frames[0], 1U, &msg) == ReceiveOutcome::FragmentAccepted);
    CHECK(submit(fx, frames[1], 1U, &msg) == ReceiveOutcome::Complete);
    CHECK(std::memcmp(msg.payload.data, payload.data(), payload.size()) == 0);
}

void test_two_sources_interleaved() {
    Fixture fx;
    const auto pa = make_payload(500, 0x01);
    const auto pb = make_payload(480, 0xA0);
    const auto fa = frames_for(0x1111U, 1U, pa);
    const auto fb = frames_for(0x2222U, 1U, pb);
    CHECK(fa.size() == 3U);
    CHECK(fb.size() == 3U);

    ReceivedMessage msg{};
    CHECK(submit(fx, fa[0], 1U, &msg) == ReceiveOutcome::FragmentAccepted);
    CHECK(submit(fx, fb[0], 1U, &msg) == ReceiveOutcome::FragmentAccepted);
    CHECK(submit(fx, fb[1], 1U, &msg) == ReceiveOutcome::FragmentAccepted);
    CHECK(submit(fx, fa[1], 1U, &msg) == ReceiveOutcome::FragmentAccepted);
    CHECK(submit(fx, fb[2], 1U, &msg) == ReceiveOutcome::Complete);
    CHECK(msg.header.source_id == 0x2222U);
    CHECK(std::memcmp(msg.payload.data, pb.data(), pb.size()) == 0);
    CHECK(submit(fx, fa[2], 1U, &msg) == ReceiveOutcome::Complete);
    CHECK(msg.header.source_id == 0x1111U);
    CHECK(std::memcmp(msg.payload.data, pa.data(), pa.size()) == 0);
}

void test_duplicate_fragment() {
    Fixture fx;
    const auto payload = make_payload(400, 0x01);
    const auto frames = frames_for(1U, 1U, payload);
    ReceivedMessage msg{};
    CHECK(submit(fx, frames[0], 1U, &msg) == ReceiveOutcome::FragmentAccepted);
    CHECK(submit(fx, frames[0], 1U, &msg) == ReceiveOutcome::DuplicateFragment);
    CHECK(fx.receiver.stats().duplicate_fragments == 1U);
    // still completes
    ReceiveOutcome last = ReceiveOutcome::InvalidArgument;
    for (std::size_t i = 1; i < frames.size(); ++i) last = submit(fx, frames[i], 1U, &msg);
    CHECK(last == ReceiveOutcome::Complete);
}

void test_crc_and_decode_dropped_apart() {
    Fixture fx;
    const auto payload = make_payload(30, 0x01);
    auto frames = frames_for(1U, 1U, payload);
    CHECK(frames.size() == 1U);

    // flip the last octet -> CRC-32 fails
    auto corrupt = frames[0];
    corrupt.back() ^= 0xFFU;
    ReceivedMessage msg{};
    CHECK(submit(fx, corrupt, 1U, &msg) == ReceiveOutcome::DroppedCrc);

    // total garbage -> decode fails for another reason
    std::vector<std::uint8_t> junk(50, 0x5AU);
    CHECK(submit(fx, junk, 1U, &msg) == ReceiveOutcome::DroppedDecode);

    const auto stats = fx.receiver.stats();
    CHECK(stats.dropped_crc == 1U);
    CHECK(stats.dropped_decode == 1U);
}

void test_fifth_concurrent_reassembly_rejected() {
    Fixture fx;
    ReceivedMessage msg{};
    // four distinct senders, each one first fragment -> four slots taken
    for (std::uint32_t s = 1U; s <= 4U; ++s) {
        const auto f = frames_for(0x100U + s, 1U, make_payload(400, 0x01));
        CHECK(submit(fx, f[0], 1U, &msg) == ReceiveOutcome::FragmentAccepted);
    }
    const auto fifth = frames_for(0x999U, 1U, make_payload(400, 0x01));
    CHECK(submit(fx, fifth[0], 1U, &msg) == ReceiveOutcome::DroppedReassembly);
    CHECK(fx.receiver.stats().dropped_reassembly == 1U);
}

void test_partial_expires() {
    Fixture fx(1000);  // 1s timeout
    const auto frames = frames_for(1U, 1U, make_payload(400, 0x01));
    ReceivedMessage msg{};
    CHECK(submit(fx, frames[0], 1000U, &msg) == ReceiveOutcome::FragmentAccepted);

    // explicit sweep well past the deadline
    CHECK(fx.receiver.expire(5000U) == 1U);
    CHECK(fx.receiver.stats().reassembly_timeouts == 1U);

    // a fresh partial, then a submit() far in the future sweeps it too
    CHECK(submit(fx, frames[0], 6000U, &msg) == ReceiveOutcome::FragmentAccepted);
    CHECK(submit(fx, frames[1], 20000U, &msg) == ReceiveOutcome::FragmentAccepted);
    CHECK(fx.receiver.stats().reassembly_timeouts == 2U);
}

void test_output_buffer_too_small() {
    Fixture fx;
    const auto payload = make_payload(500, 0x01);
    const auto frames = frames_for(1U, 1U, payload);
    ReceivedMessage msg{};
    // 3 fragments; the completed message is 500 octets but the out buffer is 64
    std::uint8_t small[64];
    ReceiveOutcome last = ReceiveOutcome::Complete;
    for (const auto& fr : frames) {
        last = fx.receiver.submit(fr.data(), fr.size(), 1U, small, sizeof(small), &msg);
    }
    CHECK(last == ReceiveOutcome::InvalidArgument);
    CHECK(fx.receiver.stats().invalid_argument == 1U);
    // the slot was released: a fresh, fitting message still goes through
    ReceivedMessage msg2{};
    last = ReceiveOutcome::InvalidArgument;
    for (const auto& fr : frames_for(1U, 1U, make_payload(100, 0x01)))
        last = submit(fx, fr, 2U, &msg2);
    CHECK(last == ReceiveOutcome::Complete);

    // unfragmented too small
    const auto one = frames_for(1U, 1U, make_payload(30, 0x01));
    CHECK(fx.receiver.submit(one[0].data(), one[0].size(), 3U, small, 8U, &msg) ==
          ReceiveOutcome::InvalidArgument);
}

void test_submit_decoded_frame() {
    Fixture fx;
    const auto payload = make_payload(450, 0x01);
    const auto frames = frames_for(1U, 1U, payload);
    ReceivedMessage msg{};
    ReceiveOutcome last = ReceiveOutcome::InvalidArgument;
    for (const auto& fr : frames) {
        btp::DecodedFrame decoded{};
        CHECK(btp::decode(fr.data(), fr.size(), btp::TransportProfile::EspNow,
                          &decoded) == btp::Error::Ok);
        last = fx.receiver.submit(decoded, 1U, fx.out, sizeof(fx.out), &msg);
    }
    CHECK(last == ReceiveOutcome::Complete);
    CHECK(std::memcmp(msg.payload.data, payload.data(), payload.size()) == 0);
    // the DecodedFrame path never touches the decode counters
    const auto stats = fx.receiver.stats();
    CHECK(stats.dropped_crc == 0U);
    CHECK(stats.dropped_decode == 0U);
}

void test_null_args() {
    Fixture fx;
    ReceivedMessage msg{};
    const auto frames = frames_for(1U, 1U, make_payload(30, 0x01));
    CHECK(fx.receiver.submit(nullptr, 10U, 1U, fx.out, sizeof(fx.out), &msg) ==
          ReceiveOutcome::InvalidArgument);
    CHECK(fx.receiver.submit(frames[0].data(), 0U, 1U, fx.out, sizeof(fx.out),
                             &msg) == ReceiveOutcome::InvalidArgument);
    CHECK(fx.receiver.submit(frames[0].data(), frames[0].size(), 1U, nullptr,
                             10U, &msg) == ReceiveOutcome::InvalidArgument);
    CHECK(fx.receiver.stats().invalid_argument == 3U);
}

void test_clear_and_string() {
    Fixture fx;
    const auto frames = frames_for(1U, 1U, make_payload(400, 0x01));
    ReceivedMessage msg{};
    CHECK(submit(fx, frames[0], 1U, &msg) == ReceiveOutcome::FragmentAccepted);
    fx.receiver.clear();
    CHECK(fx.receiver.stats().fragments_accepted == 0U);
    // the partial is gone -- pushing fragment 0 again is a fresh Accepted
    CHECK(submit(fx, frames[0], 1U, &msg) == ReceiveOutcome::FragmentAccepted);

    CHECK(std::strcmp(btp::receive_outcome_string(ReceiveOutcome::Complete),
                      "complete") == 0);
    CHECK(std::strcmp(btp::receive_outcome_string(ReceiveOutcome::DroppedCrc),
                      "dropped: crc mismatch") == 0);
}

}  // namespace

int main() {
    test_valid();
    test_unfragmented();
    test_fragmented_in_order();
    test_fragmented_out_of_order();
    test_two_sources_interleaved();
    test_duplicate_fragment();
    test_crc_and_decode_dropped_apart();
    test_fifth_concurrent_reassembly_rejected();
    test_partial_expires();
    test_output_buffer_too_small();
    test_submit_decoded_frame();
    test_null_args();
    test_clear_and_string();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all btp::Receiver checks passed\n";
    return 0;
}
