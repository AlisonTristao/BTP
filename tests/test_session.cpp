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

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all btp::session checks passed\n";
    return 0;
}
