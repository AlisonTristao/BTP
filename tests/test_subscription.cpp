// Unit tests for btp::SubscriptionTable / btp::SubscriptionClient -- the two
// halves of docs/commands.md section 4 (SUBSCRIBE / SUBSCRIBE_RESULT /
// UNSUBSCRIBE).
//
// Both are behaviour above the wire, not a wire layout, so there is no
// vector tree: the golden sequences the book defines are checked here --
// clamping to a topic's max_rate_millihz, renewal reusing a subscription_id,
// "removing an absent subscription is success", capacity limits, and the
// initiator's correlate / renew / expire cycle (mirroring
// btp::SessionInitiator's own tests).

#include "btp/subscription.hpp"

#include "btp/messages.hpp"

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

using btp::Catalog;
using btp::ClientSubscription;
using btp::ControlResult;
using btp::Header;
using btp::MessageError;
using btp::ResultError;
using btp::ResultStatus;
using btp::Subscribe;
using btp::SubscribeResult;
using btp::SubscriptionClient;
using btp::SubscriptionEvent;
using btp::SubscriptionOutcome;
using btp::SubscriptionRecord;
using btp::SubscriptionTable;
using btp::Unsubscribe;

const btp::FieldRecord kSchema[] = {btp::f32("x")};

// A served catalogue: 0x0101 subscribable, capped at 5000 mHz; 0x0202 not
// subscribable at all.
btp::StaticCatalog<> make_catalog() {
    btp::StaticCatalog<> cat;
    cat.add_topic(0x0101U, 1U, btp::TelemetryEncoding::PackedLe, /*subscribable=*/true,
                 /*max_rate_millihz=*/5000U, "capped", kSchema, 1U);
    cat.add_topic(0x0202U, 1U, btp::TelemetryEncoding::PackedLe, /*subscribable=*/false,
                 0U, "private", kSchema, 1U);
    return cat;
}

// A second served catalogue exercising min_rate_millihz / default_rate_millihz
// (library 2.40.0): 0x0303 is non-periodic (max=0) with a 2000 mHz nominal
// default; 0x0404 has a 5000 mHz floor and no cap at all.
btp::StaticCatalog<> make_catalog_with_rate_policy() {
    btp::StaticCatalog<> cat;
    cat.add_topic(0x0303U, 1U, btp::TelemetryEncoding::PackedLe, /*subscribable=*/true,
                 /*max_rate_millihz=*/0U, "non_periodic", kSchema, 1U,
                 /*min_rate_millihz=*/0U, /*default_rate_millihz=*/2000U);
    cat.add_topic(0x0404U, 1U, btp::TelemetryEncoding::PackedLe, /*subscribable=*/true,
                 /*max_rate_millihz=*/0U, "floored", kSchema, 1U,
                 /*min_rate_millihz=*/5000U, /*default_rate_millihz=*/0U);
    return cat;
}

Header make_header(std::uint32_t source_id, std::uint32_t boot_id,
                   std::uint32_t sequence) {
    Header h = {};
    h.type = btp::MessageType::Control;
    h.source_id = source_id;
    h.boot_id = boot_id;
    h.sequence = sequence;
    h.object_id = btp::object_id::kSubscribe;
    h.fragment_count = 1U;
    return h;
}

Subscribe make_request(std::uint16_t topic_id, std::uint32_t rate_millihz = 10000U,
                       std::uint32_t lease_ms = 60000U) {
    Subscribe req = {};
    req.target_source_id = 0x00CAFE01U;
    req.target_boot_id = 0x0000B001U;
    req.topic_id = topic_id;
    req.requested_rate_millihz = rate_millihz;
    req.requested_lease_ms = lease_ms;
    return req;
}

// A self-owned table: `N` slots.
template <std::size_t N>
struct TableFixture {
    SubscriptionRecord slots[N];
    SubscriptionTable table;
    TableFixture() : slots(), table(slots, N) {}
};

// ===========================================================================
// SubscriptionTable -- the RESPONDER
// ===========================================================================

void test_table_grants_and_clamps_rate() {
    TableFixture<4> f;
    // auto, not `const Catalog catalog = ...`: Catalog owns none of its
    // storage (btp/catalog.hpp's own top comment) -- it only holds pointers
    // into StaticCatalog<>'s pools. Naming the base type here would slice the
    // StaticCatalog<> temporary make_catalog() returns down to its Catalog
    // subobject, copy those pointers, and then destroy the temporary that
    // actually owned the pools they point into at the end of the full
    // expression -- a dangling-pointer bug that (accidentally) reads back
    // correct bytes on an unoptimized build, because nothing has overwritten
    // that stack slot yet, and reliably fails under -O3 once the compiler
    // reuses it. `auto` keeps the whole StaticCatalog<>, pools included,
    // alive for catalog's own scope; every call below still takes it as
    // `const Catalog&` through the ordinary derived-to-base reference
    // conversion, no slicing involved. Every other make_catalog() call in
    // this file follows the same rule.
    const auto catalog = make_catalog();
    SubscribeResult result = {};
    f.table.handle_subscribe(catalog, make_header(1U, 2U, 1U),
                             make_request(0x0101U, /*rate=*/50000U), 0U, &result);

    CHECK(result.status == static_cast<std::uint8_t>(ResultStatus::Success));
    CHECK(result.subscription_id != 0U);
    CHECK(result.effective_rate_millihz == 5000U);  // clamped to the topic's cap
    CHECK(result.granted_lease_ms == 60000U);
    CHECK(result.request.request_source_id == 1U);
    CHECK(result.request.request_boot_id == 2U);
    CHECK(result.request.reply_to_sequence == 1U);
}

void test_table_rejects_non_subscribable_or_unknown_topic() {
    TableFixture<4> f;
    const auto catalog = make_catalog();

    SubscribeResult not_subscribable = {};
    f.table.handle_subscribe(catalog, make_header(1U, 2U, 1U), make_request(0x0202U),
                             0U, &not_subscribable);
    CHECK(not_subscribable.status != static_cast<std::uint8_t>(ResultStatus::Success));
    CHECK(not_subscribable.subscription_id == 0U);

    SubscribeResult unknown = {};
    f.table.handle_subscribe(catalog, make_header(1U, 2U, 2U), make_request(0x0999U),
                             0U, &unknown);
    CHECK(unknown.status != static_cast<std::uint8_t>(ResultStatus::Success));
}

void test_table_renewal_reuses_the_subscription_id() {
    TableFixture<4> f;
    const auto catalog = make_catalog();

    SubscribeResult first = {};
    f.table.handle_subscribe(catalog, make_header(1U, 2U, 1U), make_request(0x0101U),
                             0U, &first);
    SubscribeResult second = {};
    f.table.handle_subscribe(catalog, make_header(1U, 2U, 2U), make_request(0x0101U),
                             1000U, &second);

    CHECK(first.subscription_id == second.subscription_id);
}

void test_table_capacity_exhausted() {
    TableFixture<2> f;
    const auto catalog = make_catalog();
    SubscribeResult a = {}, b = {}, c = {};
    f.table.handle_subscribe(catalog, make_header(1U, 1U, 1U), make_request(0x0101U), 0U,
                             &a);
    f.table.handle_subscribe(catalog, make_header(2U, 2U, 1U), make_request(0x0101U), 0U,
                             &b);
    f.table.handle_subscribe(catalog, make_header(3U, 3U, 1U), make_request(0x0101U), 0U,
                             &c);  // a third distinct requester -- no free slot

    CHECK(a.status == static_cast<std::uint8_t>(ResultStatus::Success));
    CHECK(b.status == static_cast<std::uint8_t>(ResultStatus::Success));
    CHECK(c.status != static_cast<std::uint8_t>(ResultStatus::Success));
    CHECK(c.subscription_id == 0U);
}

// ---------------------------------------------------------------------------
// Rate policy (library 2.40.0): default_rate_millihz caps a non-periodic
// topic (max_rate_millihz == 0) the same way max_rate_millihz caps a
// periodic one; min_rate_millihz rejects outright rather than granting a
// rate the client asked to go faster than, never slower.
// ---------------------------------------------------------------------------
void test_table_default_rate_caps_a_non_periodic_topic() {
    TableFixture<4> f;
    const auto catalog = make_catalog_with_rate_policy();

    SubscribeResult result = {};
    f.table.handle_subscribe(catalog, make_header(1U, 2U, 1U),
                             make_request(0x0303U, /*rate=*/50000U), 0U, &result);
    CHECK(result.status == static_cast<std::uint8_t>(ResultStatus::Success));
    CHECK(result.effective_rate_millihz == 2000U);  // clamped to the default, not the ask

    // A request already under the default is granted as asked -- the default
    // is a cap, not a floor.
    SubscribeResult slow = {};
    f.table.handle_subscribe(catalog, make_header(2U, 2U, 1U),
                             make_request(0x0303U, /*rate=*/500U), 0U, &slow);
    CHECK(slow.status == static_cast<std::uint8_t>(ResultStatus::Success));
    CHECK(slow.effective_rate_millihz == 500U);
}

void test_table_rejects_rate_below_minimum() {
    TableFixture<4> f;
    const auto catalog = make_catalog_with_rate_policy();

    SubscribeResult below = {};
    f.table.handle_subscribe(catalog, make_header(1U, 2U, 1U),
                             make_request(0x0404U, /*rate=*/1000U), 0U, &below);
    CHECK(below.status == static_cast<std::uint8_t>(ResultStatus::Rejected));
    CHECK(below.error_code == static_cast<std::uint16_t>(ResultError::InvalidArgument));
    CHECK(below.subscription_id == 0U);
    // The rejection never touched the slot table -- 0x0404 has no subscriber.
    CHECK(f.table.subscriber_count(0x0404U) == 0U);

    // At/above the floor, no cap at all (default_rate_millihz is 0 here) --
    // granted exactly as requested.
    SubscribeResult at_floor = {};
    f.table.handle_subscribe(catalog, make_header(1U, 2U, 2U),
                             make_request(0x0404U, /*rate=*/9000U), 0U, &at_floor);
    CHECK(at_floor.status == static_cast<std::uint8_t>(ResultStatus::Success));
    CHECK(at_floor.effective_rate_millihz == 9000U);
}

// ---------------------------------------------------------------------------
// Introspection (library 2.40.0): subscriber_count() / aggregate_rate_millihz()
// read back the granted slots for a caller's own observability, without a
// parallel table of its own.
// ---------------------------------------------------------------------------
void test_table_subscriber_count_and_aggregate_rate() {
    TableFixture<4> f;
    const auto catalog = make_catalog();

    CHECK(f.table.subscriber_count(0x0101U) == 0U);
    CHECK(f.table.aggregate_rate_millihz(0x0101U) == 0U);

    SubscribeResult a = {}, b = {};
    f.table.handle_subscribe(catalog, make_header(1U, 1U, 1U),
                             make_request(0x0101U, /*rate=*/1000U), 0U, &a);
    f.table.handle_subscribe(catalog, make_header(2U, 2U, 1U),
                             make_request(0x0101U, /*rate=*/3000U), 0U, &b);

    CHECK(f.table.subscriber_count(0x0101U) == 2U);
    // 5000 is the topic's own cap (make_catalog()); both requests -- 1000 and
    // 3000 -- resolve under it, so the aggregate is the faster of the two, not
    // the cap.
    CHECK(f.table.aggregate_rate_millihz(0x0101U) == 3000U);
    CHECK(f.table.subscriber_count(0x0202U) == 0U);  // a different topic
}

// ---------------------------------------------------------------------------
// Reboot awareness (library 2.40.0): a SUBSCRIBE from a source_id already
// holding subscriptions under a DIFFERENT boot_id evicts those first -- a
// rebooted peer has no session left to keep publishing for.
// ---------------------------------------------------------------------------
void test_table_reboot_evicts_subscriptions_under_the_old_boot_id() {
    TableFixture<2> f;
    const auto catalog = make_catalog();

    SubscribeResult first_boot = {};
    f.table.handle_subscribe(catalog, make_header(1U, /*boot=*/2U, 1U),
                             make_request(0x0101U), 0U, &first_boot);
    SubscribeResult other_peer = {};
    f.table.handle_subscribe(catalog, make_header(9U, 9U, 1U),
                             make_request(0x0101U), 0U, &other_peer);
    CHECK(first_boot.status == static_cast<std::uint8_t>(ResultStatus::Success));
    CHECK(other_peer.status == static_cast<std::uint8_t>(ResultStatus::Success));
    CHECK(f.table.subscriber_count(0x0101U) == 2U);  // the table is now full (2 slots)

    // source_id 1 comes back with a NEW boot_id: its old subscription is
    // evicted before this one is even looked up, so there IS a free slot for
    // it despite the table's capacity never growing.
    SubscribeResult new_boot = {};
    f.table.handle_subscribe(catalog, make_header(1U, /*boot=*/3U, 1U),
                             make_request(0x0101U), 0U, &new_boot);
    CHECK(new_boot.status == static_cast<std::uint8_t>(ResultStatus::Success));
    CHECK(new_boot.subscription_id != first_boot.subscription_id);
    // Still 2: the old (source=1, boot=2) slot is gone, replaced by
    // (source=1, boot=3); (source=9, boot=9) is untouched.
    CHECK(f.table.subscriber_count(0x0101U) == 2U);
}

void test_table_unsubscribe_absent_or_foreign_is_success_but_a_no_op() {
    TableFixture<4> f;
    const auto catalog = make_catalog();
    SubscribeResult granted = {};
    f.table.handle_subscribe(catalog, make_header(1U, 2U, 1U), make_request(0x0101U), 0U,
                             &granted);

    // A different requester "removing" someone else's subscription: success,
    // but the original subscription is untouched (still due()).
    ControlResult foreign = {};
    Unsubscribe foreign_req = {};
    foreign_req.target_source_id = 0x00CAFE01U;
    foreign_req.target_boot_id = 0x0000B001U;
    foreign_req.subscription_id = granted.subscription_id;
    f.table.handle_unsubscribe(make_header(9U, 9U, 1U), foreign_req, &foreign);
    CHECK(foreign.status == static_cast<std::uint8_t>(ResultStatus::Success));
    CHECK(f.table.due(0x0101U, 0U));  // still active

    // An unknown id: also success.
    ControlResult absent = {};
    Unsubscribe absent_req = foreign_req;
    absent_req.subscription_id = granted.subscription_id + 1000U;
    f.table.handle_unsubscribe(make_header(1U, 2U, 2U), absent_req, &absent);
    CHECK(absent.status == static_cast<std::uint8_t>(ResultStatus::Success));

    // The actual owner removing it: gone.
    ControlResult owned = {};
    f.table.handle_unsubscribe(make_header(1U, 2U, 3U), foreign_req, &owned);
    CHECK(owned.status == static_cast<std::uint8_t>(ResultStatus::Success));
    CHECK(!f.table.due(0x0101U, 1000000U));
}

void test_table_expire_frees_lapsed_leases() {
    TableFixture<4> f;
    const auto catalog = make_catalog();
    SubscribeResult result = {};
    f.table.handle_subscribe(catalog, make_header(1U, 2U, 1U),
                             make_request(0x0101U, 10000U, /*lease_ms=*/500U), 0U,
                             &result);
    CHECK(f.table.due(0x0101U, 0U));

    f.table.expire(499U);
    CHECK(f.table.due(0x0101U, 499U));  // not yet
    f.table.expire(500U);
    CHECK(!f.table.due(0x0101U, 500U));  // gone
}

void test_table_due_cadence_and_fastest_subscriber_wins() {
    TableFixture<4> f;
    const auto catalog = make_catalog();
    // 1000 mHz -> a 1000 ms period; 5000 mHz (clamped from higher) -> 200 ms.
    SubscribeResult slow = {}, fast = {};
    f.table.handle_subscribe(catalog, make_header(1U, 1U, 1U),
                             make_request(0x0101U, 1000U, 60000U), 0U, &slow);
    f.table.handle_subscribe(catalog, make_header(2U, 2U, 1U),
                             make_request(0x0101U, 50000U, 60000U), 0U, &fast);
    CHECK(slow.effective_rate_millihz == 1000U);
    CHECK(fast.effective_rate_millihz == 5000U);  // clamped

    CHECK(f.table.due(0x0101U, 0U));  // both due right away
    f.table.note_published(0x0101U, 0U);
    CHECK(!f.table.due(0x0101U, 199U));
    CHECK(f.table.due(0x0101U, 200U));  // the fast subscriber's cadence fires first
}

// ===========================================================================
// SubscriptionClient -- the INITIATOR
// ===========================================================================

const std::uint32_t kOwnSource = 0x00CAFE01U;
const std::uint32_t kOwnBoot = 0x0000B001U;
const std::uint32_t kPeerSource = 0x00B0B0FEU;
const std::uint32_t kPeerBoot = 0x0000C0DEU;

template <std::size_t N>
struct ClientFixture {
    ClientSubscription slots[N];
    SubscriptionClient client;
    ClientFixture() : slots(), client(slots, N) {}
};

std::uint32_t do_subscribe(SubscriptionClient& client, std::uint32_t own_sequence,
                           std::uint64_t now_ms = 0U, std::uint16_t topic_id = 0x0101U,
                           std::uint32_t rate_millihz = 10000U,
                           std::uint32_t lease_ms = 1000U) {
    std::uint8_t out[24];
    std::size_t n = 0U;
    return client.subscribe(kPeerSource, kPeerBoot, topic_id, rate_millihz, lease_ms,
                            kOwnSource, kOwnBoot, own_sequence, now_ms, out, sizeof(out),
                            &n);
}

SubscribeResult make_success_result(std::uint32_t own_sequence,
                                    std::uint32_t subscription_id = 77U,
                                    std::uint32_t effective_rate_millihz = 10000U,
                                    std::uint32_t granted_lease_ms = 1000U) {
    SubscribeResult r = {};
    r.request.request_source_id = kOwnSource;
    r.request.request_boot_id = kOwnBoot;
    r.request.reply_to_sequence = own_sequence;
    r.status = static_cast<std::uint8_t>(ResultStatus::Success);
    r.subscription_id = subscription_id;
    r.effective_rate_millihz = effective_rate_millihz;
    r.granted_lease_ms = granted_lease_ms;
    return r;
}

void test_client_subscribe_encodes_a_real_subscribe() {
    ClientFixture<4> f;
    std::uint8_t out[24];
    std::size_t n = 0U;
    const std::uint32_t id =
        f.client.subscribe(kPeerSource, kPeerBoot, 0x0101U, 10000U, 60000U, kOwnSource,
                           kOwnBoot, 5U, 0U, out, sizeof(out), &n);
    CHECK(id != 0U);

    Subscribe decoded = {};
    CHECK(btp::decode_subscribe(out, n, &decoded) == MessageError::Ok);
    CHECK(decoded.target_source_id == kPeerSource);
    CHECK(decoded.topic_id == 0x0101U);
    CHECK(decoded.requested_rate_millihz == 10000U);
    CHECK(decoded.requested_lease_ms == 60000U);
}

void test_client_subscribe_fails_without_a_free_slot() {
    ClientFixture<1> f;
    CHECK(do_subscribe(f.client, 1U) != 0U);
    CHECK(do_subscribe(f.client, 2U) == 0U);  // the one slot is still Pending
}

void test_client_on_result_success_activates() {
    ClientFixture<4> f;
    const std::uint32_t id = do_subscribe(f.client, 5U, 0U, 0x0101U, 10000U, 1000U);
    const SubscriptionOutcome o =
        f.client.on_result(make_success_result(5U, 77U, 10000U, 1000U), 0U);
    CHECK(o.event == SubscriptionEvent::Granted);
    CHECK(o.local_id == id);
    // The outcome names what was granted without a second lookup -- straight
    // off the slot (peer/topic/requested_rate) and the wire reply (effective_rate).
    CHECK(o.peer_source_id == kPeerSource);
    CHECK(o.peer_boot_id == kPeerBoot);
    CHECK(o.topic_id == 0x0101U);
    CHECK(o.requested_rate_millihz == 10000U);
    CHECK(o.effective_rate_millihz == 10000U);

    // Not yet due for renewal right after granting (80% of 1000 ms left).
    CHECK(f.client.next_renewal_due(799U) == 0U);
    CHECK(f.client.next_renewal_due(800U) == id);  // 20% margin
}

void test_client_on_result_rejected_frees_the_slot() {
    ClientFixture<1> f;
    do_subscribe(f.client, 5U);
    SubscribeResult reject = {};
    reject.request.request_source_id = kOwnSource;
    reject.request.request_boot_id = kOwnBoot;
    reject.request.reply_to_sequence = 5U;
    reject.status = static_cast<std::uint8_t>(ResultStatus::Rejected);
    reject.error_code = static_cast<std::uint16_t>(btp::ResultError::CapacityExhausted);
    const SubscriptionOutcome o = f.client.on_result(reject, 0U);
    CHECK(o.event == SubscriptionEvent::Rejected);
    // Named even though the slot is gone by the time the caller sees this --
    // captured before the reset, not read back from the (now Idle) slot.
    CHECK(o.peer_source_id == kPeerSource);
    CHECK(o.topic_id == 0x0101U);
    CHECK(o.status == static_cast<std::uint8_t>(ResultStatus::Rejected));
    CHECK(o.error_code == static_cast<std::uint16_t>(btp::ResultError::CapacityExhausted));

    // The slot is free again.
    CHECK(do_subscribe(f.client, 6U) != 0U);
}

void test_client_on_result_uncorrelated_is_ignored() {
    ClientFixture<4> f;
    const std::uint32_t id = do_subscribe(f.client, 5U);
    const SubscriptionOutcome o = f.client.on_result(make_success_result(/*seq=*/6U), 0U);
    CHECK(o.event == SubscriptionEvent::None);
    CHECK(o.local_id == 0U);

    // The real result still lands afterwards.
    CHECK(f.client.on_result(make_success_result(5U), 0U).event ==
          SubscriptionEvent::Granted);
    (void)id;
}

void test_client_renewal_keeps_the_same_local_id() {
    ClientFixture<4> f;
    const std::uint32_t id = do_subscribe(f.client, 5U, 0U, 0x0101U, 10000U, 1000U);
    f.client.on_result(make_success_result(5U, 77U, 10000U, 1000U), 0U);

    CHECK(f.client.next_renewal_due(800U) == id);
    std::uint8_t out[24];
    std::size_t n = 0U;
    CHECK(f.client.renew(id, kOwnSource, kOwnBoot, /*own_sequence=*/6U, 800U, out,
                         sizeof(out), &n));
    Subscribe decoded = {};
    CHECK(btp::decode_subscribe(out, n, &decoded) == MessageError::Ok);
    CHECK(decoded.topic_id == 0x0101U);
    CHECK(decoded.requested_rate_millihz == 10000U);  // same as the original ask

    // Pending again -- not due a second time until the new result lands.
    CHECK(f.client.next_renewal_due(900U) == 0U);
    const SubscriptionOutcome o =
        f.client.on_result(make_success_result(6U, 77U, 10000U, 1000U), 800U);
    CHECK(o.event == SubscriptionEvent::Granted);
    CHECK(o.local_id == id);  // still the SAME subscription
}

void test_client_expire_frees_pending_and_active() {
    ClientFixture<4> f;
    const std::uint32_t pending_id = do_subscribe(f.client, 5U, /*now_ms=*/0U);
    f.client.expire(btp::kSubscriptionPendingTimeoutMs - 1U);
    CHECK(f.client.next_renewal_due(1000000U) == 0U);  // still Pending, not Active
    f.client.expire(btp::kSubscriptionPendingTimeoutMs);
    // Freed: subscribing again from scratch gets a NEW local id.
    const std::uint32_t new_id = do_subscribe(f.client, 6U, /*now_ms=*/0U);
    CHECK(new_id != 0U);
    (void)pending_id;

    // An Active slot whose lease lapses with no renewal is freed too.
    f.client.on_result(make_success_result(6U, 77U, 10000U, 1000U), 0U);
    CHECK(f.client.next_renewal_due(1000U) == new_id);
    f.client.expire(1000U);  // the lease itself, not just the renewal margin
    CHECK(f.client.next_renewal_due(1000U) == 0U);
}

void test_client_unsubscribe_active_encodes_and_frees() {
    ClientFixture<2> f;
    const std::uint32_t id = do_subscribe(f.client, 5U);
    f.client.on_result(make_success_result(5U, 77U), 0U);

    std::uint8_t out[16];
    std::size_t n = 0U;
    CHECK(f.client.unsubscribe(id, kOwnSource, kOwnBoot, 9U, out, sizeof(out), &n));
    Unsubscribe decoded = {};
    CHECK(btp::decode_unsubscribe(out, n, &decoded) == MessageError::Ok);
    CHECK(decoded.subscription_id == 77U);

    // Freed -- a second unsubscribe() on the same id fails.
    CHECK(!f.client.unsubscribe(id, kOwnSource, kOwnBoot, 10U, out, sizeof(out), &n));
}

void test_client_unsubscribe_pending_or_unknown_fails() {
    ClientFixture<2> f;
    const std::uint32_t pending_id = do_subscribe(f.client, 5U);  // never granted
    std::uint8_t out[16];
    std::size_t n = 0U;
    CHECK(!f.client.unsubscribe(pending_id, kOwnSource, kOwnBoot, 9U, out, sizeof(out),
                                &n));
    CHECK(!f.client.unsubscribe(0xFFFFU, kOwnSource, kOwnBoot, 9U, out, sizeof(out), &n));
}

void test_subscription_event_strings() {
    CHECK(std::strcmp(btp::subscription_event_string(SubscriptionEvent::Granted),
                      "Granted") == 0);
    CHECK(btp::subscription_event_string(SubscriptionEvent::Expired) != nullptr);
}

}  // namespace

int main() {
    test_table_grants_and_clamps_rate();
    test_table_rejects_non_subscribable_or_unknown_topic();
    test_table_renewal_reuses_the_subscription_id();
    test_table_capacity_exhausted();
    test_table_default_rate_caps_a_non_periodic_topic();
    test_table_rejects_rate_below_minimum();
    test_table_subscriber_count_and_aggregate_rate();
    test_table_reboot_evicts_subscriptions_under_the_old_boot_id();
    test_table_unsubscribe_absent_or_foreign_is_success_but_a_no_op();
    test_table_expire_frees_lapsed_leases();
    test_table_due_cadence_and_fastest_subscriber_wins();

    test_client_subscribe_encodes_a_real_subscribe();
    test_client_subscribe_fails_without_a_free_slot();
    test_client_on_result_success_activates();
    test_client_on_result_rejected_frees_the_slot();
    test_client_on_result_uncorrelated_is_ignored();
    test_client_renewal_keeps_the_same_local_id();
    test_client_expire_frees_pending_and_active();
    test_client_unsubscribe_active_encodes_and_frees();
    test_client_unsubscribe_pending_or_unknown_fails();
    test_subscription_event_strings();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all btp::subscription checks passed\n";
    return 0;
}
