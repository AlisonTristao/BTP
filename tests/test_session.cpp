// Unit tests for btp::session -- btp::DedupCache, the command-deduplication
// cache of docs/commands.md sections 2.5-2.6 and
// docs/session-and-terminal.md section 5.3.
//
// Deduplication is behaviour with state, not wire layout, so there is no
// vector tree (btp::Reassembler has none either). The golden sequences the
// book defines are checked here:
//
//   * fresh -> execute -> complete -> a retransmission replays the exact
//     stored COMMAND_RESULT and does not execute again;
//   * a retransmission that arrives mid-execution is DuplicateInFlight;
//   * the same identity with different request bytes is Conflict;
//   * the ring evicts the oldest completed entry and keeps serving, and an
//     evicted identity is Evicted (never re-executed), not Fresh;
//   * capacity limits: every slot in flight, request too large, requester
//     table full of distinct devices;
//   * record_result / release / clear / valid() edge cases.

#include "btp/session.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>

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

using btp::ByteView;
using btp::DedupCache;
using btp::DedupKey;
using btp::DedupRequester;
using btp::DedupStorage;
using btp::DedupSlot;
using btp::DedupVerdict;
using btp::MessageError;

// A self-owned cache: `Slots` slots of `Region` bytes each, `Requesters` rows.
template <std::size_t Slots, std::size_t Region, std::size_t Requesters>
struct Fixture {
    DedupSlot slots[Slots];
    std::uint8_t bytes[Slots][Region];
    DedupStorage storage[Slots];
    DedupRequester requesters[Requesters];
    DedupCache cache;

    Fixture() : cache(bind()) {}

    DedupCache bind() noexcept {
        for (std::size_t i = 0U; i < Slots; ++i) {
            storage[i].data = bytes[i];
            storage[i].capacity = Region;
        }
        return DedupCache(slots, storage, Slots, requesters, Requesters);
    }
};

// A COMMAND_REQUEST-shaped blob whose bytes depend on `tag`, so two calls with
// a different tag are "different logical requests".
struct Request {
    std::uint8_t data[24];
    std::size_t size;
};

Request make_request(std::uint8_t tag) {
    Request r = {};
    r.size = sizeof(r.data);
    for (std::size_t i = 0U; i < r.size; ++i) {
        r.data[i] = static_cast<std::uint8_t>(tag + i);
    }
    return r;
}

DedupVerdict classify(DedupCache& cache, const DedupKey& key, const Request& req,
                      std::size_t* slot_out, ByteView* result_out) {
    return cache.classify(key, req.data, req.size, slot_out, result_out);
}

// ---------------------------------------------------------------------------

void test_invalid_config() {
    DedupSlot slots[2];
    DedupStorage storage[2] = {};  // null data -> invalid
    DedupRequester requesters[1];
    DedupCache cache(slots, storage, 2U, requesters, 1U);
    CHECK(!cache.valid());

    DedupKey key = {1U, 1U, 1U};
    const Request req = make_request(0U);
    std::size_t slot = 99U;
    CHECK(classify(cache, key, req, &slot, nullptr) ==
          DedupVerdict::InvalidArgument);
    CHECK(slot == 99U);

    DedupCache null_cache(nullptr, nullptr, 0U, nullptr, 0U);
    CHECK(!null_cache.valid());
}

void test_fresh_execute_complete_replay() {
    Fixture<4U, 128U, 2U> fx;
    CHECK(fx.cache.valid());

    const DedupKey key = {0x1111U, 0x2222U, 100U};
    const Request req = make_request(7U);

    std::size_t slot = 0U;
    ByteView stored = {};
    CHECK(classify(fx.cache, key, req, &slot, &stored) == DedupVerdict::Fresh);

    // Mid-execution: a retransmission is held off, not executed.
    CHECK(classify(fx.cache, key, req, nullptr, nullptr) ==
          DedupVerdict::DuplicateInFlight);

    const std::uint8_t result[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    stored = ByteView{};
    CHECK(fx.cache.record_result(slot, result, sizeof(result), &stored) ==
          MessageError::Ok);
    CHECK(stored.size == sizeof(result));
    CHECK(stored.data != nullptr &&
          std::memcmp(stored.data, result, sizeof(result)) == 0);

    // Every later retransmission replays the exact bytes.
    for (int i = 0; i < 3; ++i) {
        ByteView replay = {};
        CHECK(classify(fx.cache, key, req, nullptr, &replay) ==
              DedupVerdict::DuplicateComplete);
        CHECK(replay.size == sizeof(result));
        CHECK(replay.data != nullptr &&
              std::memcmp(replay.data, result, sizeof(result)) == 0);
    }

    const DedupCache::Stats s = fx.cache.stats();
    CHECK(s.reserved == 1U);
    CHECK(s.completed == 1U);
    CHECK(s.in_flight == 1U);
    CHECK(s.replayed == 3U);
    CHECK(s.evicted == 0U);
}

void test_conflict_same_key_different_request() {
    Fixture<4U, 128U, 2U> fx;
    const DedupKey key = {5U, 6U, 42U};

    std::size_t slot = 0U;
    CHECK(classify(fx.cache, key, make_request(1U), &slot, nullptr) ==
          DedupVerdict::Fresh);

    // Reused sequence, different bytes -- conflict while still in flight...
    CHECK(classify(fx.cache, key, make_request(2U), nullptr, nullptr) ==
          DedupVerdict::Conflict);

    const std::uint8_t result[] = {1, 2, 3};
    CHECK(fx.cache.record_result(slot, result, sizeof(result)) ==
          MessageError::Ok);

    // ...and after completion.
    CHECK(classify(fx.cache, key, make_request(2U), nullptr, nullptr) ==
          DedupVerdict::Conflict);
    // The original identity still replays.
    CHECK(classify(fx.cache, key, make_request(1U), nullptr, nullptr) ==
          DedupVerdict::DuplicateComplete);
    CHECK(fx.cache.stats().conflicts == 2U);
}

void test_ring_eviction_keeps_serving() {
    Fixture<2U, 128U, 2U> fx;
    const std::uint8_t result[] = {0x10, 0x20};

    // Fill both slots with completed entries, sequences 10 then 20.
    for (std::uint32_t seq = 10U; seq <= 20U; seq += 10U) {
        const DedupKey key = {1U, 1U, seq};
        std::size_t slot = 0U;
        CHECK(classify(fx.cache, key, make_request(static_cast<std::uint8_t>(seq)),
                       &slot, nullptr) == DedupVerdict::Fresh);
        CHECK(fx.cache.record_result(slot, result, sizeof(result)) ==
              MessageError::Ok);
    }

    // A third fresh identity still gets served -- the oldest (seq 10) is evicted.
    const DedupKey fresh = {1U, 1U, 30U};
    std::size_t slot = 0U;
    CHECK(classify(fx.cache, fresh, make_request(30U), &slot, nullptr) ==
          DedupVerdict::Fresh);
    CHECK(fx.cache.record_result(slot, result, sizeof(result)) ==
          MessageError::Ok);
    CHECK(fx.cache.stats().evicted == 1U);

    // seq 20 is still cached and replays; seq 30 is now cached.
    CHECK(classify(fx.cache, DedupKey{1U, 1U, 20U}, make_request(20U), nullptr,
                   nullptr) == DedupVerdict::DuplicateComplete);
    CHECK(classify(fx.cache, fresh, make_request(30U), nullptr, nullptr) ==
          DedupVerdict::DuplicateComplete);
}

void test_evicted_identity_is_not_re_executed() {
    Fixture<2U, 128U, 2U> fx;
    const std::uint8_t result[] = {0x33};

    for (std::uint32_t seq = 10U; seq <= 20U; seq += 10U) {
        const DedupKey key = {1U, 1U, seq};
        std::size_t slot = 0U;
        CHECK(classify(fx.cache, key, make_request(static_cast<std::uint8_t>(seq)),
                       &slot, nullptr) == DedupVerdict::Fresh);
        CHECK(fx.cache.record_result(slot, result, sizeof(result)) ==
              MessageError::Ok);
    }
    std::size_t slot = 0U;
    CHECK(classify(fx.cache, DedupKey{1U, 1U, 30U}, make_request(30U), &slot,
                   nullptr) == DedupVerdict::Fresh);  // evicts seq 10

    const std::uint32_t reserved_before = fx.cache.stats().reserved;

    // The evicted command's retransmission must NOT reserve a slot / execute.
    CHECK(classify(fx.cache, DedupKey{1U, 1U, 10U}, make_request(10U), nullptr,
                   nullptr) == DedupVerdict::Evicted);
    // A never-seen sequence below the evicted high-water mark is conservatively
    // Evicted too (a well-behaved requester's sequence only increases).
    CHECK(classify(fx.cache, DedupKey{1U, 1U, 5U}, make_request(5U), nullptr,
                   nullptr) == DedupVerdict::Evicted);
    CHECK(fx.cache.stats().reserved == reserved_before);

    // A fresh sequence above the mark is still served.
    CHECK(classify(fx.cache, DedupKey{1U, 1U, 40U}, make_request(40U), &slot,
                   nullptr) == DedupVerdict::Fresh);
}

void test_capacity_exhausted_when_all_in_flight() {
    Fixture<2U, 128U, 2U> fx;

    for (std::uint32_t seq = 1U; seq <= 2U; ++seq) {
        std::size_t slot = 0U;
        CHECK(classify(fx.cache, DedupKey{1U, 1U, seq},
                       make_request(static_cast<std::uint8_t>(seq)), &slot,
                       nullptr) == DedupVerdict::Fresh);
        // no record_result -> the slot stays Reserved (in flight)
    }

    // Nothing is evictable while every slot holds an unfinished execution.
    CHECK(classify(fx.cache, DedupKey{1U, 1U, 3U}, make_request(3U), nullptr,
                   nullptr) == DedupVerdict::CapacityExhausted);
    CHECK(fx.cache.stats().evicted == 0U);
}

void test_record_result_edges() {
    Fixture<2U, 40U, 2U> fx;  // 40-byte region, 24-byte request -> 16 left
    const DedupKey key = {1U, 1U, 1U};
    std::size_t slot = 0U;
    CHECK(classify(fx.cache, key, make_request(0U), &slot, nullptr) ==
          DedupVerdict::Fresh);

    std::uint8_t big[20] = {};
    CHECK(fx.cache.record_result(slot, big, sizeof(big)) ==
          MessageError::BufferTooSmall);

    std::uint8_t fits[16] = {};
    CHECK(fx.cache.record_result(slot, fits, sizeof(fits)) == MessageError::Ok);
    // Already Complete -> a second record_result is out of order.
    CHECK(fx.cache.record_result(slot, fits, 1U) == MessageError::WrongOrder);
    // Out-of-range index.
    CHECK(fx.cache.record_result(99U, fits, 1U) == MessageError::InvalidArgument);
    // A free slot was never reserved.
    CHECK(fx.cache.record_result(1U, fits, 1U) == MessageError::WrongOrder);
}

void test_release_makes_identity_fresh_again() {
    Fixture<2U, 128U, 2U> fx;
    const DedupKey key = {1U, 1U, 7U};
    std::size_t slot = 0U;
    CHECK(classify(fx.cache, key, make_request(1U), &slot, nullptr) ==
          DedupVerdict::Fresh);

    CHECK(fx.cache.release(slot));
    CHECK(!fx.cache.release(slot));  // already free

    // The abandoned execution's identity can be retried from scratch.
    CHECK(classify(fx.cache, key, make_request(1U), &slot, nullptr) ==
          DedupVerdict::Fresh);
}

void test_requester_table_full() {
    Fixture<4U, 128U, 1U> fx;  // one requester row

    std::size_t slot = 0U;
    CHECK(classify(fx.cache, DedupKey{100U, 1U, 1U}, make_request(1U), &slot,
                   nullptr) == DedupVerdict::Fresh);
    // A second, distinct device has nowhere to record its high-water mark.
    CHECK(classify(fx.cache, DedupKey{200U, 1U, 1U}, make_request(2U), nullptr,
                   nullptr) == DedupVerdict::CapacityExhausted);
}

void test_stale_boot_reuses_requester_row() {
    Fixture<4U, 128U, 1U> fx;  // one requester row

    std::size_t slot = 0U;
    CHECK(classify(fx.cache, DedupKey{100U, 1U, 5U}, make_request(1U), &slot,
                   nullptr) == DedupVerdict::Fresh);
    // Same source, new boot -> the old boot is gone, its row is reused.
    CHECK(classify(fx.cache, DedupKey{100U, 2U, 1U}, make_request(2U), &slot,
                   nullptr) == DedupVerdict::Fresh);
    CHECK(fx.cache.stats().exhausted == 0U);
}

void test_clear_resets_everything() {
    Fixture<2U, 128U, 2U> fx;
    const std::uint8_t result[] = {9};
    for (std::uint32_t seq = 1U; seq <= 2U; ++seq) {
        std::size_t slot = 0U;
        CHECK(classify(fx.cache, DedupKey{1U, 1U, seq},
                       make_request(static_cast<std::uint8_t>(seq)), &slot,
                       nullptr) == DedupVerdict::Fresh);
        CHECK(fx.cache.record_result(slot, result, sizeof(result)) ==
              MessageError::Ok);
    }

    fx.cache.clear();

    const DedupCache::Stats s = fx.cache.stats();
    CHECK(s.reserved == 0U && s.completed == 0U && s.replayed == 0U);
    // A previously handled identity is Fresh again after a clear.
    std::size_t slot = 0U;
    CHECK(classify(fx.cache, DedupKey{1U, 1U, 1U}, make_request(1U), &slot,
                   nullptr) == DedupVerdict::Fresh);
}

void test_zero_length_request() {
    Fixture<2U, 64U, 2U> fx;
    const DedupKey key = {1U, 1U, 1U};
    std::size_t slot = 0U;
    CHECK(fx.cache.classify(key, nullptr, 0U, &slot, nullptr) ==
          DedupVerdict::Fresh);
    const std::uint8_t result[] = {1, 2};
    CHECK(fx.cache.record_result(slot, result, sizeof(result)) ==
          MessageError::Ok);
    ByteView replay = {};
    CHECK(fx.cache.classify(key, nullptr, 0U, nullptr, &replay) ==
          DedupVerdict::DuplicateComplete);
    CHECK(replay.size == 2U);
}

void test_verdict_strings() {
    CHECK(std::strcmp(btp::dedup_verdict_string(DedupVerdict::Fresh), "Fresh") ==
          0);
    CHECK(std::strcmp(btp::dedup_verdict_string(DedupVerdict::Evicted),
                      "Evicted") == 0);
    CHECK(std::strcmp(btp::dedup_verdict_string(DedupVerdict::DuplicateComplete),
                      "DuplicateComplete") == 0);
}

// ===========================================================================
// btp::Session -- the responder state machine
// ===========================================================================

using btp::EffectiveLimits;
using btp::Hello;
using btp::HelloResult;
using btp::Session;
using btp::SessionEvent;
using btp::SessionState;

// This peer's own HELLO advertisement.
Hello make_local(std::uint32_t max_payload = 2048U,
                 std::uint32_t timeout_ms = 30000U) {
    Hello h = {};
    h.role = static_cast<std::uint8_t>(btp::Role::Producer);
    h.version_count = 1U;
    h.versions[0] = 1U;
    h.max_logical_payload = max_payload;
    h.max_inflight_reassemblies = 4U;
    h.max_subscriptions = 8U;
    h.max_dedup_entries = 32U;
    h.session_timeout_ms = timeout_ms;
    for (std::size_t i = 0U; i < 16U; ++i) {
        h.peer_uuid[i] = static_cast<std::uint8_t>(0xA0U + i);
    }
    h.config_revision = 7U;
    return h;
}

// A decoded frame whose payload buffer lives inside the struct, so the
// btp::DecodedFrame handed to on_frame() stays valid for the call.
struct FakeFrame {
    std::uint8_t payload[96];
    btp::DecodedFrame frame;

    FakeFrame() : frame() {}
};

FakeFrame control_frame(std::uint16_t object_id, const std::uint8_t* payload,
                        std::size_t payload_size, std::uint32_t source_id,
                        std::uint32_t boot_id, std::uint32_t sequence) {
    FakeFrame ff;
    CHECK(payload_size <= sizeof(ff.payload));
    std::memcpy(ff.payload, payload, payload_size);
    ff.frame.header.type = btp::MessageType::Control;
    ff.frame.header.source_id = source_id;
    ff.frame.header.boot_id = boot_id;
    ff.frame.header.sequence = sequence;
    ff.frame.header.object_id = object_id;
    ff.frame.header.fragment_count = 1U;
    ff.frame.payload.data = ff.payload;
    ff.frame.payload.size = payload_size;
    return ff;
}

FakeFrame hello_frame(const Hello& hello, std::uint32_t source_id,
                      std::uint32_t boot_id, std::uint32_t sequence) {
    std::uint8_t buffer[64];
    std::size_t written = 0U;
    CHECK(btp::encode_hello(hello, buffer, sizeof(buffer), &written) ==
          btp::MessageError::Ok);
    return control_frame(btp::object_id::kHello, buffer, written, source_id,
                         boot_id, sequence);
}

// Drive a fresh session all the way to Active and return it.
void bring_up(Session* session, std::uint64_t now_ms) {
    session->arm(now_ms);
    const FakeFrame hello = hello_frame(make_local(4096U, 20000U), 0x1111U,
                                        0x2222U, 1U);
    std::uint8_t reply[btp::kSessionMaxReplySize];
    const btp::SessionOutcome o =
        session->on_frame(hello.frame, now_ms, reply, sizeof(reply));
    CHECK(o.event == SessionEvent::HelloAccepted);
    CHECK(session->active());
}

void test_session_valid_rejects_malformed_local() {
    Hello bad = make_local();
    bad.version_count = 0U;  // no announced versions
    Session session(bad, 2000U);
    CHECK(!session.valid());

    // A false valid() means every HELLO is rejected.
    session.arm(0U);
    const FakeFrame hello = hello_frame(make_local(), 5U, 6U, 1U);
    std::uint8_t reply[btp::kSessionMaxReplySize];
    const btp::SessionOutcome o =
        session.on_frame(hello.frame, 10U, reply, sizeof(reply));
    CHECK(o.event == SessionEvent::HelloRejected);
    CHECK(session.state() == SessionState::Idle);
}

void test_session_starts_idle_and_ignores_frames() {
    Session session(make_local(), 2000U);
    CHECK(session.valid());
    CHECK(session.state() == SessionState::Idle);

    const FakeFrame hello = hello_frame(make_local(), 5U, 6U, 1U);
    std::uint8_t reply[btp::kSessionMaxReplySize];
    const btp::SessionOutcome o =
        session.on_frame(hello.frame, 100U, reply, sizeof(reply));
    CHECK(o.event == SessionEvent::None);
    CHECK(session.state() == SessionState::Idle);
}

void test_session_hello_accepted_negotiates_minimum() {
    Session session(make_local(/*max_payload=*/700U, /*timeout_ms=*/15000U),
                    2000U);
    session.arm(1000U);
    CHECK(session.state() == SessionState::AwaitingHello);

    // Remote asks for more room and a shorter watchdog.
    Hello remote = make_local(/*max_payload=*/53550U, /*timeout_ms=*/5000U);
    remote.role = static_cast<std::uint8_t>(btp::Role::Consumer);
    remote.max_inflight_reassemblies = 32U;
    remote.max_subscriptions = 64U;
    remote.max_dedup_entries = 128U;
    const FakeFrame hello = hello_frame(remote, 0x0C0D0E0FU, 0x10203040U, 9U);

    std::uint8_t reply[btp::kSessionMaxReplySize];
    const btp::SessionOutcome o =
        session.on_frame(hello.frame, 1500U, reply, sizeof(reply));

    CHECK(o.event == SessionEvent::HelloAccepted);
    CHECK(session.state() == SessionState::Active);
    CHECK(session.peer_source_id() == 0x0C0D0E0FU);
    CHECK(session.peer_boot_id() == 0x10203040U);

    HelloResult decoded = {};
    CHECK(btp::decode_hello_result(reply, o.reply_size, &decoded) ==
          btp::MessageError::Ok);
    CHECK(decoded.status == static_cast<std::uint8_t>(btp::ResultStatus::Success));
    CHECK(decoded.selected_version == 1U);
    CHECK(decoded.request.request_source_id == 0x0C0D0E0FU);
    CHECK(decoded.request.reply_to_sequence == 9U);
    CHECK(decoded.max_logical_payload == 700U);   // min(53550, 700)
    CHECK(decoded.max_inflight_reassemblies == 4U);   // min(32, 4)
    CHECK(decoded.max_subscriptions == 8U);           // min(64, 8)
    CHECK(decoded.max_dedup_entries == 32U);          // min(128, 32)
    CHECK(decoded.session_timeout_ms == 5000U);       // min(15000, 5000)
    CHECK(std::memcmp(decoded.peer_uuid, make_local().peer_uuid, 16U) == 0);
    CHECK(decoded.config_revision == 7U);

    const EffectiveLimits& eff = session.effective_limits();
    CHECK(eff.max_logical_payload == 700U);
    CHECK(eff.session_timeout_ms == 5000U);
}

void test_session_rejects_hello_without_common_version() {
    Session session(make_local(), 2000U);
    session.arm(0U);

    Hello remote = make_local();
    remote.versions[0] = 2U;  // only version 2, which this peer does not speak
    const FakeFrame hello = hello_frame(remote, 42U, 43U, 7U);

    std::uint8_t reply[btp::kSessionMaxReplySize];
    const btp::SessionOutcome o =
        session.on_frame(hello.frame, 100U, reply, sizeof(reply));

    CHECK(o.event == SessionEvent::HelloRejected);
    CHECK(session.state() == SessionState::Idle);

    HelloResult decoded = {};
    CHECK(btp::decode_hello_result(reply, o.reply_size, &decoded) ==
          btp::MessageError::Ok);
    CHECK(decoded.status ==
          static_cast<std::uint8_t>(btp::ResultStatus::Unsupported));
    CHECK(decoded.selected_version == 0U);
}

void test_session_rejects_malformed_hello_payload() {
    Session session(make_local(), 2000U);
    session.arm(0U);

    const std::uint8_t junk[6] = {1, 2, 3, 4, 5, 6};
    const FakeFrame bad =
        control_frame(btp::object_id::kHello, junk, sizeof(junk), 1U, 1U, 1U);

    std::uint8_t reply[btp::kSessionMaxReplySize];
    const btp::SessionOutcome o =
        session.on_frame(bad.frame, 10U, reply, sizeof(reply));
    CHECK(o.event == SessionEvent::HelloRejected);
    CHECK(session.state() == SessionState::Idle);
}

void test_session_non_hello_does_not_renew_hello_deadline() {
    Session session(make_local(), 2000U);
    session.arm(1000U);

    // A CONTROL frame that is not HELLO, arriving well before the deadline.
    const std::uint8_t body[4] = {0, 0, 0, 0};
    const FakeFrame other =
        control_frame(btp::object_id::kStatus, body, sizeof(body), 1U, 1U, 1U);
    CHECK(session.on_frame(other.frame, 2000U, nullptr, 0U).event ==
          SessionEvent::None);
    CHECK(session.state() == SessionState::AwaitingHello);

    // The deadline is still the original arm() + 2000, not renewed to 4000.
    CHECK(session.poll(2999U).event == SessionEvent::None);
    CHECK(session.poll(3000U).event == SessionEvent::TimedOut);
    CHECK(session.state() == SessionState::Idle);
}

void test_session_hello_deadline_times_out_via_poll() {
    Session session(make_local(), 2000U);
    session.arm(1000U);
    CHECK(session.poll(2999U).event == SessionEvent::None);
    CHECK(session.poll(3000U).event == SessionEvent::TimedOut);
    CHECK(session.state() == SessionState::Idle);
    // Reported once.
    CHECK(session.poll(9999U).event == SessionEvent::None);
}

void test_session_late_hello_is_dropped_as_timed_out() {
    Session session(make_local(), 2000U);
    session.arm(1000U);

    const FakeFrame hello = hello_frame(make_local(), 5U, 6U, 1U);
    std::uint8_t reply[btp::kSessionMaxReplySize];
    // now_ms is past arm() + 2000: on_frame sweeps the deadline first.
    const btp::SessionOutcome o =
        session.on_frame(hello.frame, 3500U, reply, sizeof(reply));
    CHECK(o.event == SessionEvent::TimedOut);
    CHECK(session.state() == SessionState::Idle);
}

void test_session_active_frames_renew_the_watchdog() {
    Session session(make_local(), 2000U);
    bring_up(&session, 0U);
    // negotiated session_timeout_ms is min(30000, 20000) = 20000.
    CHECK(session.effective_limits().session_timeout_ms == 20000U);

    const std::uint8_t body[8] = {0};
    std::uint64_t now = 0U;
    for (int i = 0; i < 10; ++i) {
        now += 15000U;  // < 20000, so each frame renews before expiry
        const FakeFrame f = control_frame(btp::object_id::kStatus, body,
                                          sizeof(body), 0x1111U, 0x2222U,
                                          static_cast<std::uint32_t>(i + 2));
        const btp::SessionOutcome o =
            session.on_frame(f.frame, now, nullptr, 0U);
        CHECK(o.event == SessionEvent::FrameAccepted);
        CHECK(session.active());
    }
    // Total elapsed 150000 ms, far past one 20 s window -- still alive.
    CHECK(session.active());
}

void test_session_watchdog_expires_when_idle() {
    Session session(make_local(), 2000U);
    bring_up(&session, 1000U);  // deadline 1000 + 20000
    CHECK(session.poll(20999U).event == SessionEvent::None);
    CHECK(session.poll(21000U).event == SessionEvent::TimedOut);
    CHECK(session.state() == SessionState::Idle);
    CHECK(session.poll(99999U).event == SessionEvent::None);  // once
}

void test_session_close_returns_to_idle() {
    Session session(make_local(), 2000U);
    bring_up(&session, 0U);

    std::uint8_t close_payload[8] = {0};
    close_payload[0] = static_cast<std::uint8_t>(btp::CloseReason::ClientShutdown);
    // drain_timeout_ms at offset 4, little-endian.
    close_payload[4] = 0xF4U;
    close_payload[5] = 0x01U;  // 500
    const FakeFrame close = control_frame(btp::object_id::kSessionClose,
                                          close_payload, sizeof(close_payload),
                                          0x1111U, 0x2222U, 5U);

    std::uint8_t reply[btp::kSessionMaxReplySize];
    const btp::SessionOutcome o =
        session.on_frame(close.frame, 100U, reply, sizeof(reply));
    CHECK(o.event == SessionEvent::SessionClosed);
    CHECK(session.state() == SessionState::Idle);

    btp::ControlResult decoded = {};
    CHECK(btp::decode_session_close_result(reply, o.reply_size, &decoded) ==
          btp::MessageError::Ok);
    CHECK(decoded.status ==
          static_cast<std::uint8_t>(btp::ResultStatus::Success));
    CHECK(decoded.request.request_source_id == 0x1111U);
    CHECK(decoded.request.reply_to_sequence == 5U);
}

void test_session_close_malformed_payload_is_rejected() {
    Session session(make_local(), 2000U);
    bring_up(&session, 0U);

    const std::uint8_t junk[3] = {9, 9, 9};
    const FakeFrame close = control_frame(btp::object_id::kSessionClose, junk,
                                          sizeof(junk), 0x1111U, 0x2222U, 5U);
    std::uint8_t reply[btp::kSessionMaxReplySize];
    const btp::SessionOutcome o =
        session.on_frame(close.frame, 100U, reply, sizeof(reply));
    CHECK(o.event == SessionEvent::SessionClosed);
    CHECK(session.state() == SessionState::Idle);

    btp::ControlResult decoded = {};
    CHECK(btp::decode_session_close_result(reply, o.reply_size, &decoded) ==
          btp::MessageError::Ok);
    CHECK(decoded.status ==
          static_cast<std::uint8_t>(btp::ResultStatus::Rejected));
    CHECK(decoded.error_code ==
          static_cast<std::uint16_t>(btp::ResultError::MalformedPayload));
}

void test_session_reset_reports_abandoned_once() {
    Session session(make_local(), 2000U);
    bring_up(&session, 0U);

    CHECK(session.reset().event == SessionEvent::Abandoned);
    CHECK(session.state() == SessionState::Idle);
    CHECK(session.reset().event == SessionEvent::None);

    // Also from AwaitingHello.
    session.arm(10U);
    CHECK(session.reset().event == SessionEvent::Abandoned);
    CHECK(session.state() == SessionState::Idle);
}

void test_session_zero_hello_deadline_never_times_out_awaiting() {
    Session session(make_local(), 0U);  // HELLO deadline disabled
    session.arm(1000U);
    CHECK(session.poll(9'999'999U).event == SessionEvent::None);
    CHECK(session.state() == SessionState::AwaitingHello);
}

void test_session_hello_reply_buffer_too_small_stays_put() {
    Session session(make_local(), 2000U);
    session.arm(0U);
    const FakeFrame hello = hello_frame(make_local(), 5U, 6U, 1U);
    std::uint8_t tiny[8];
    const btp::SessionOutcome o =
        session.on_frame(hello.frame, 10U, tiny, sizeof(tiny));
    CHECK(o.event == SessionEvent::None);
    CHECK(o.reply_size == 0U);
    CHECK(session.state() == SessionState::AwaitingHello);
    CHECK(!session.active());
}

void test_session_round_trips_a_real_encoded_hello() {
    // The full path: encode_hello -> btp::encode -> btp::decode -> on_frame.
    Hello remote = make_local(4096U, 12000U);
    std::uint8_t hello_payload[64];
    std::size_t payload_size = 0U;
    CHECK(btp::encode_hello(remote, hello_payload, sizeof(hello_payload),
                            &payload_size) == btp::MessageError::Ok);

    btp::Frame frame = {};
    frame.header.type = btp::MessageType::Control;
    frame.header.source_id = 0xABCDEF01U;
    frame.header.boot_id = 0x00112233U;
    frame.header.sequence = 3U;
    frame.header.object_id = btp::object_id::kHello;
    frame.header.fragment_count = 1U;
    frame.payload.data = hello_payload;
    frame.payload.size = payload_size;

    std::uint8_t wire[128];
    std::size_t wire_size = 0U;
    CHECK(btp::encode(frame, btp::TransportProfile::Serial, wire, sizeof(wire),
                      &wire_size) == btp::Error::Ok);

    btp::DecodedFrame decoded = {};
    CHECK(btp::decode(wire, wire_size, btp::TransportProfile::Serial, &decoded) ==
          btp::Error::Ok);

    Session session(make_local(2048U, 30000U), 2000U);
    session.arm(0U);
    std::uint8_t reply[btp::kSessionMaxReplySize];
    const btp::SessionOutcome o =
        session.on_frame(decoded, 5U, reply, sizeof(reply));
    CHECK(o.event == SessionEvent::HelloAccepted);
    CHECK(session.active());
    CHECK(session.peer_source_id() == 0xABCDEF01U);
    CHECK(session.effective_limits().max_logical_payload == 2048U);
    CHECK(session.effective_limits().session_timeout_ms == 12000U);
}

void test_session_set_local_updates_the_advertisement() {
    Session session(make_local(2048U, 30000U), 2000U);
    CHECK(session.valid());

    // A later manifest revision + a different uuid; still a legal HELLO.
    Hello updated = make_local(2048U, 30000U);
    updated.config_revision = 99U;
    for (std::size_t i = 0U; i < 16U; ++i) {
        updated.peer_uuid[i] = static_cast<std::uint8_t>(0x40U + i);
    }
    CHECK(session.set_local(updated));

    session.arm(0U);
    const FakeFrame hello = hello_frame(make_local(4096U, 20000U), 7U, 8U, 1U);
    std::uint8_t reply[btp::kSessionMaxReplySize];
    const btp::SessionOutcome o =
        session.on_frame(hello.frame, 5U, reply, sizeof(reply));
    CHECK(o.event == SessionEvent::HelloAccepted);

    HelloResult decoded = {};
    CHECK(btp::decode_hello_result(reply, o.reply_size, &decoded) ==
          btp::MessageError::Ok);
    CHECK(decoded.config_revision == 99U);
    CHECK(decoded.peer_uuid[0] == 0x40U);

    // A malformed advertisement flips valid() and makes every HELLO reject.
    Hello broken = make_local();
    broken.peer_uuid[0] = 0U;
    for (std::size_t i = 1U; i < 16U; ++i) broken.peer_uuid[i] = 0U;
    CHECK(!session.set_local(broken));
    CHECK(!session.valid());
}

void test_session_state_and_event_strings() {
    CHECK(btp::session_state_string(SessionState::Idle) != nullptr);
    CHECK(std::strcmp(btp::session_state_string(SessionState::Active),
                      "Active") == 0);
    CHECK(std::strcmp(btp::session_event_string(SessionEvent::TimedOut),
                      "TimedOut") == 0);
    CHECK(std::strcmp(btp::session_event_string(SessionEvent::HelloAccepted),
                      "HelloAccepted") == 0);
}

// ===========================================================================
// btp::SessionInitiator -- the other end of the handshake
// ===========================================================================

using btp::InitiatorEvent;
using btp::InitiatorOutcome;
using btp::InitiatorState;
using btp::SessionInitiator;

const std::uint32_t kOwnSource = 0x00CAFE01U;
const std::uint32_t kOwnBoot = 0x0000B001U;
const std::uint32_t kOwnSequence = 42U;

FakeFrame hello_result_frame(const HelloResult& result, std::uint32_t source_id,
                             std::uint32_t boot_id, std::uint32_t sequence) {
    std::uint8_t buffer[btp::kSessionMaxReplySize];
    std::size_t written = 0U;
    CHECK(btp::encode_hello_result(result, buffer, sizeof(buffer), &written) ==
          btp::MessageError::Ok);
    return control_frame(btp::object_id::kHelloResult, buffer, written,
                         source_id, boot_id, sequence);
}

// A SUCCESS HELLO_RESULT that correlates with (kOwnSource, kOwnBoot,
// kOwnSequence) -- the identity connect() below always uses.
HelloResult make_success_result(std::uint32_t max_payload = 4096U,
                                std::uint32_t timeout_ms = 20000U) {
    HelloResult r = {};
    r.request.request_source_id = kOwnSource;
    r.request.request_boot_id = kOwnBoot;
    r.request.reply_to_sequence = kOwnSequence;
    r.status = static_cast<std::uint8_t>(btp::ResultStatus::Success);
    r.selected_version = 1U;
    r.max_logical_payload = max_payload;
    r.max_inflight_reassemblies = 4U;
    r.max_subscriptions = 8U;
    r.max_dedup_entries = 32U;
    r.session_timeout_ms = timeout_ms;
    for (std::size_t i = 0U; i < 16U; ++i) {
        r.peer_uuid[i] = static_cast<std::uint8_t>(0xB0U + i);
    }
    r.config_revision = 3U;
    return r;
}

bool do_connect(SessionInitiator* initiator, std::uint64_t now_ms,
                std::uint64_t deadline_ms = 2000U) {
    std::uint8_t hello[btp::kSessionMaxHelloSize];
    std::size_t written = 0U;
    return initiator->connect(make_local(), kOwnSource, kOwnBoot, kOwnSequence,
                              now_ms, deadline_ms, hello, sizeof(hello),
                              &written);
}

void test_initiator_starts_idle() {
    SessionInitiator initiator;
    CHECK(initiator.state() == InitiatorState::Idle);
    CHECK(!initiator.connected());
}

void test_initiator_connect_encodes_a_real_hello() {
    SessionInitiator initiator;
    std::uint8_t hello[btp::kSessionMaxHelloSize];
    std::size_t written = 0U;
    CHECK(initiator.connect(make_local(4096U, 12000U), kOwnSource, kOwnBoot,
                            kOwnSequence, 0U, 2000U, hello, sizeof(hello),
                            &written));
    CHECK(initiator.state() == InitiatorState::AwaitingResult);

    Hello decoded = {};
    CHECK(btp::decode_hello(hello, written, &decoded) == btp::MessageError::Ok);
    CHECK(decoded.max_logical_payload == 4096U);
    CHECK(decoded.session_timeout_ms == 12000U);
}

void test_initiator_connect_is_a_no_op_outside_idle() {
    SessionInitiator initiator;
    CHECK(do_connect(&initiator, 0U));
    CHECK(!do_connect(&initiator, 1U));  // still AwaitingResult
    CHECK(initiator.state() == InitiatorState::AwaitingResult);
}

void test_initiator_hello_result_success_connects() {
    SessionInitiator initiator;
    CHECK(do_connect(&initiator, 0U));

    const FakeFrame result = hello_result_frame(
        make_success_result(4096U, 20000U), 0x1111U, 0x2222U, 9U);
    const InitiatorOutcome o = initiator.on_frame(result.frame, 5U);
    CHECK(o.event == InitiatorEvent::Connected);
    CHECK(initiator.connected());
    CHECK(initiator.effective_limits().max_logical_payload == 4096U);
    CHECK(initiator.effective_limits().session_timeout_ms == 20000U);
    CHECK(initiator.peer_source_id() == 0x1111U);
    CHECK(initiator.peer_boot_id() == 0x2222U);
}

void test_initiator_hello_result_unsupported_rejects() {
    SessionInitiator initiator;
    CHECK(do_connect(&initiator, 0U));

    HelloResult reject = {};
    reject.request.request_source_id = kOwnSource;
    reject.request.request_boot_id = kOwnBoot;
    reject.request.reply_to_sequence = kOwnSequence;
    reject.status = static_cast<std::uint8_t>(btp::ResultStatus::Unsupported);
    reject.error_code =
        static_cast<std::uint16_t>(btp::ResultError::UnsupportedVersion);
    for (std::size_t i = 0U; i < 16U; ++i) reject.peer_uuid[i] = 0U;

    const FakeFrame result = hello_result_frame(reject, 0x1111U, 0x2222U, 9U);
    const InitiatorOutcome o = initiator.on_frame(result.frame, 5U);
    CHECK(o.event == InitiatorEvent::Rejected);
    CHECK(initiator.state() == InitiatorState::Idle);
    CHECK(!initiator.connected());
}

void test_initiator_ignores_uncorrelated_hello_result() {
    SessionInitiator initiator;
    CHECK(do_connect(&initiator, 0U));

    // Right shape, wrong sequence -- an answer to some earlier attempt.
    HelloResult stale = make_success_result();
    stale.request.reply_to_sequence = kOwnSequence + 1U;
    const FakeFrame result = hello_result_frame(stale, 0x1111U, 0x2222U, 9U);
    const InitiatorOutcome o = initiator.on_frame(result.frame, 5U);
    CHECK(o.event == InitiatorEvent::None);
    CHECK(initiator.state() == InitiatorState::AwaitingResult);
}

void test_initiator_non_result_does_not_renew_deadline() {
    SessionInitiator initiator;
    CHECK(do_connect(&initiator, 0U, /*deadline_ms=*/2000U));

    const std::uint8_t no_payload[1] = {0U};
    const FakeFrame stray = control_frame(0x0099U, no_payload, 0U, 5U, 6U, 1U);
    const InitiatorOutcome o = initiator.on_frame(stray.frame, 1000U);
    CHECK(o.event == InitiatorEvent::None);
    // Still times out at the ORIGINAL deadline -- the stray frame did not
    // extend it (docs section 1: "no application message before HELLO_RESULT").
    CHECK(initiator.poll(2000U).event == InitiatorEvent::TimedOut);
    CHECK(initiator.state() == InitiatorState::Idle);
}

void test_initiator_awaiting_result_times_out_via_poll() {
    SessionInitiator initiator;
    CHECK(do_connect(&initiator, 0U, /*deadline_ms=*/2000U));
    CHECK(initiator.poll(1999U).event == InitiatorEvent::None);
    CHECK(initiator.poll(2000U).event == InitiatorEvent::TimedOut);
    CHECK(initiator.state() == InitiatorState::Idle);
}

void test_initiator_zero_deadline_never_times_out_awaiting() {
    SessionInitiator initiator;
    CHECK(do_connect(&initiator, 0U, /*deadline_ms=*/0U));
    CHECK(initiator.poll(1000000U).event == InitiatorEvent::None);
    CHECK(initiator.state() == InitiatorState::AwaitingResult);
}

void test_initiator_active_frames_renew_the_watchdog() {
    SessionInitiator initiator;
    CHECK(do_connect(&initiator, 0U));
    const FakeFrame result = hello_result_frame(
        make_success_result(4096U, 5000U), 0x1111U, 0x2222U, 9U);
    CHECK(initiator.on_frame(result.frame, 0U).event ==
          InitiatorEvent::Connected);

    // A telemetry frame at t=4000 renews past the original 5000 deadline.
    const std::uint8_t no_payload[1] = {0U};
    const FakeFrame telemetry = control_frame(0x0101U, no_payload, 0U, 0x1111U,
                                              0x2222U, 10U);
    CHECK(initiator.on_frame(telemetry.frame, 4000U).event ==
          InitiatorEvent::FrameAccepted);
    CHECK(initiator.poll(8999U).event == InitiatorEvent::None);
    CHECK(initiator.poll(9000U).event == InitiatorEvent::TimedOut);
    CHECK(initiator.state() == InitiatorState::Idle);
}

void test_initiator_disconnect_returns_to_idle() {
    SessionInitiator initiator;
    CHECK(do_connect(&initiator, 0U));
    CHECK(initiator.on_frame(hello_result_frame(make_success_result(), 0x1111U,
                                                0x2222U, 9U)
                                 .frame,
                             0U)
             .event == InitiatorEvent::Connected);

    std::uint8_t buffer[btp::kSessionMaxHelloSize];
    std::size_t written = 0U;
    CHECK(initiator.disconnect(1U, static_cast<std::uint8_t>(1U), 500U, buffer,
                               sizeof(buffer), &written));
    CHECK(initiator.state() == InitiatorState::Idle);

    btp::SessionClose close = {};
    CHECK(btp::decode_session_close(buffer, written, &close) ==
          btp::MessageError::Ok);
    CHECK(close.drain_timeout_ms == 500U);

    CHECK(!initiator.disconnect(2U, 1U, 500U, buffer, sizeof(buffer),
                                &written));  // already Idle
}

void test_initiator_reset_reports_disconnected_once() {
    SessionInitiator initiator;
    CHECK(initiator.reset().event == InitiatorEvent::None);  // idle already
    CHECK(do_connect(&initiator, 0U));
    CHECK(initiator.reset().event == InitiatorEvent::Disconnected);
    CHECK(initiator.state() == InitiatorState::Idle);
    CHECK(initiator.reset().event == InitiatorEvent::None);  // reported once
}

void test_initiator_state_and_event_strings() {
    CHECK(std::strcmp(btp::initiator_state_string(InitiatorState::Active),
                      "Active") == 0);
    CHECK(std::strcmp(btp::initiator_event_string(InitiatorEvent::Connected),
                      "Connected") == 0);
}

}  // namespace

int main() {
    test_invalid_config();
    test_fresh_execute_complete_replay();
    test_conflict_same_key_different_request();
    test_ring_eviction_keeps_serving();
    test_evicted_identity_is_not_re_executed();
    test_capacity_exhausted_when_all_in_flight();
    test_record_result_edges();
    test_release_makes_identity_fresh_again();
    test_requester_table_full();
    test_stale_boot_reuses_requester_row();
    test_clear_resets_everything();
    test_zero_length_request();
    test_verdict_strings();

    test_session_valid_rejects_malformed_local();
    test_session_starts_idle_and_ignores_frames();
    test_session_hello_accepted_negotiates_minimum();
    test_session_rejects_hello_without_common_version();
    test_session_rejects_malformed_hello_payload();
    test_session_non_hello_does_not_renew_hello_deadline();
    test_session_hello_deadline_times_out_via_poll();
    test_session_late_hello_is_dropped_as_timed_out();
    test_session_active_frames_renew_the_watchdog();
    test_session_watchdog_expires_when_idle();
    test_session_close_returns_to_idle();
    test_session_close_malformed_payload_is_rejected();
    test_session_reset_reports_abandoned_once();
    test_session_zero_hello_deadline_never_times_out_awaiting();
    test_session_hello_reply_buffer_too_small_stays_put();
    test_session_round_trips_a_real_encoded_hello();
    test_session_set_local_updates_the_advertisement();
    test_session_state_and_event_strings();

    test_initiator_starts_idle();
    test_initiator_connect_encodes_a_real_hello();
    test_initiator_connect_is_a_no_op_outside_idle();
    test_initiator_hello_result_success_connects();
    test_initiator_hello_result_unsupported_rejects();
    test_initiator_ignores_uncorrelated_hello_result();
    test_initiator_non_result_does_not_renew_deadline();
    test_initiator_awaiting_result_times_out_via_poll();
    test_initiator_zero_deadline_never_times_out_awaiting();
    test_initiator_active_frames_renew_the_watchdog();
    test_initiator_disconnect_returns_to_idle();
    test_initiator_reset_reports_disconnected_once();
    test_initiator_state_and_event_strings();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all btp::session checks passed\n";
    return 0;
}
