#ifndef BTP_SUBSCRIPTION_HPP
#define BTP_SUBSCRIPTION_HPP

// btp::SubscriptionTable / btp::SubscriptionClient -- the two halves of
// docs/commands.md section 4 (SUBSCRIBE / SUBSCRIBE_RESULT / UNSUBSCRIBE),
// mirroring btp::Session / btp::SessionInitiator's split: SubscriptionTable
// is the RESPONDER (a producer granting subscriptions against its own
// btp::Catalog), SubscriptionClient the INITIATOR (a consumer holding
// subscriptions on a peer and renewing them before they lapse). Both are
// state above the wire (docs/library.md chapter 11 lists "the subscription
// aggregator" as out of the library's core) -- this is the per-node piece
// every consumer would otherwise hand-roll once. A HUB's aggregation across
// MULTIPLE SOURCE peers (one robot's subscribers folded into one upstream
// subscription) stays the integration's, same reasoning as the priority
// scheduler; the escape hatch is reaching the table directly
// (Node::subscriptions()) and layering that on top.
//
// Same guarantees as the rest of the library: no internal allocation --
// caller-owned slot arrays; noexcept; no clock of its own (every call that
// cares about time takes a now_ms); no I/O -- both classes hand back PAYLOAD
// bytes for the caller (btp::Node) to frame and send, same contract as
// btp::Session / btp::SessionInitiator.
//
// OUT of scope, on purpose:
//   * the hub aggregation above (one subscription upstream per N downstream
//     subscribers to the same source+topic);
//   * a retry budget for a SUBSCRIBE / UNSUBSCRIBE that gets no answer
//     beyond the renewal cadence itself -- the same "no retry budget"
//     stance as btp::SessionInitiator;
//   * choosing WHEN to actually call publish() -- due() / note_published()
//     answer "is anyone waiting", the caller's loop still calls publish().

#include "btp/catalog.hpp"    // Catalog, CatalogTopic, kTopicSubscribable
#include "btp/codec.hpp"      // Header
#include "btp/messages.hpp"   // Subscribe, SubscribeResult, Unsubscribe, ControlResult

#include <cstddef>
#include <cstdint>

namespace btp {

// ===========================================================================
// SubscriptionTable -- the RESPONDER: subscriptions granted on MY topics
// ===========================================================================
//
//   btp::SubscriptionRecord slots[8];
//   btp::SubscriptionTable table(slots, 8);
//   node.enable_subscriptions(&table);
//
//   // per decoded SUBSCRIBE (Node does this once enable_subscriptions() is set):
//   btp::SubscribeResult result = {};
//   table.handle_subscribe(*served_catalog, frame.header, request, now_ms, &result);
//   // ... Node encodes `result` and sends it ...
//
//   // from the main loop:
//   table.expire(now_ms());
//   if (table.due(0x0101, now_ms()) && node.publish_named(0x0101, &fill, ...)) {
//       table.note_published(0x0101, now_ms());
//   }

// One granted subscription. Opaque -- storage is bound by SubscriptionTable's
// constructor, exactly like btp::DedupSlot / btp::ReassemblySlot.
class SubscriptionRecord {
public:
    SubscriptionRecord() noexcept;

private:
    friend class SubscriptionTable;

    bool used_;
    std::uint32_t subscription_id_;
    std::uint32_t requester_source_id_;
    std::uint32_t requester_boot_id_;
    std::uint16_t topic_id_;
    std::uint32_t effective_rate_millihz_;
    std::uint64_t expires_at_ms_;
    std::uint64_t due_at_ms_;  // due() / note_published() cadence for this slot
};

class SubscriptionTable {
public:
    SubscriptionTable(SubscriptionRecord* slots, std::size_t slot_count) noexcept;

    bool valid() const noexcept { return slots_ != nullptr && slot_count_ != 0U; }
    std::size_t slot_count() const noexcept { return slot_count_; }

    // Handle one SUBSCRIBE against `catalog` (a catalogue this node SERVES --
    // checks the topic is subscribable and clamps effective_rate to its
    // max_rate_millihz, 0 meaning uncapped). `header` is the SUBSCRIBE
    // frame's envelope; its source_id / boot_id / sequence go into the
    // RequestRef, the same way btp::Session::build_hello_result reads its
    // request's header. A second SUBSCRIBE from the same
    // (header.source_id, header.boot_id) for the same topic_id is a RENEWAL
    // -- it reuses the existing subscription_id and does not reset its
    // publish cadence (docs/commands.md 4.3). Always fills *result_out; a
    // rejection (unknown / non-subscribable topic, no free slot) is a normal
    // SUBSCRIBE_RESULT with status != Success, not a different code path --
    // the Node sends it either way.
    void handle_subscribe(const Catalog& catalog, const Header& header,
                          const Subscribe& request, std::uint64_t now_ms,
                          SubscribeResult* result_out) noexcept;

    // Handle one UNSUBSCRIBE. Removing an absent subscription, or one that
    // belongs to a different requester, is still reported success
    // (docs/commands.md 4.4: "removing an already absent subscription is
    // treated as successful"; the wire gives no way to distinguish "gone"
    // from "never yours", and the spec does not ask for one).
    void handle_unsubscribe(const Header& header, const Unsubscribe& request,
                            ControlResult* result_out) noexcept;

    // Frees every slot whose lease has passed at now_ms. Call once per
    // tick(); no reply is sent (nobody asked).
    void expire(std::uint64_t now_ms) noexcept;

    // True when `topic_id` has at least one active (unexpired) subscription
    // whose cadence is due at now_ms -- the fastest granted rate among this
    // topic's local subscribers wins when more than one wants it. Call
    // before publish() / publish_named() in the main loop.
    bool due(std::uint16_t topic_id, std::uint64_t now_ms) const noexcept;

    // Resets the cadence for every active subscription of `topic_id` -- call
    // right after a publish() the subscribers were due() for. One physical
    // publish satisfies every local subscriber of that topic at once,
    // regardless of each one's own requested rate.
    void note_published(std::uint16_t topic_id, std::uint64_t now_ms) noexcept;

private:
    SubscriptionRecord* find(std::uint32_t source_id, std::uint32_t boot_id,
                             std::uint16_t topic_id) noexcept;
    SubscriptionRecord* find_by_id(std::uint32_t subscription_id) noexcept;
    SubscriptionRecord* allocate() noexcept;

    SubscriptionRecord* slots_;
    std::size_t slot_count_;
    std::uint32_t next_id_;
};

// ===========================================================================
// SubscriptionClient -- the INITIATOR: subscriptions I hold on a peer
// ===========================================================================
//
//   btp::ClientSubscription slots[4];
//   btp::SubscriptionClient client(slots, 4);
//   node.enable_subscription_client(&client);
//
//   const std::uint32_t id = node.subscribe(robot_source_id, robot_boot_id,
//                                           0x0101, 10000 /*mHz*/, 60000 /*ms*/);
//   // ... Node feeds every SUBSCRIBE_RESULT to client.on_result() and calls
//   //     tick(), which renews before the lease runs out ...
//   node.unsubscribe(id);

enum class SubscriptionEvent : std::uint8_t {
    None,      // nothing to act on
    Granted,   // SUBSCRIBE_RESULT SUCCESS, correlated -- Active now
    Rejected,  // SUBSCRIBE_RESULT failure, correlated -- Idle now
    Expired,   // the lease ran out before a renewal was granted -- Idle now
};

const char* subscription_event_string(SubscriptionEvent event) noexcept;

struct SubscriptionOutcome {
    SubscriptionEvent event;
    std::uint32_t local_id;  // which slot -- 0 when event == None
};

// How long a Pending subscribe() / renew() waits for SUBSCRIBE_RESULT before
// expire() silently frees its slot -- generous for a multi-hop / lossy link.
// A caller wanting a tighter bound calls subscribe() again itself, the same
// "no retry budget" stance as btp::SessionInitiator.
static const std::uint64_t kSubscriptionPendingTimeoutMs = 5000U;

// One subscription this node holds on a peer. Opaque -- storage is bound by
// SubscriptionClient's constructor.
class ClientSubscription {
public:
    ClientSubscription() noexcept;

private:
    friend class SubscriptionClient;

    enum class State : std::uint8_t { Idle, Pending, Active };

    State state_;
    std::uint32_t local_id_;  // stable handle across renewals; != 0 while used
    std::uint32_t peer_source_id_;
    std::uint32_t peer_boot_id_;
    std::uint16_t topic_id_;
    std::uint32_t requested_rate_millihz_;
    std::uint32_t requested_lease_ms_;
    std::uint32_t own_source_id_;   // identity of the OUTSTANDING request
    std::uint32_t own_boot_id_;
    std::uint32_t own_sequence_;
    std::uint32_t subscription_id_;        // 0 until Granted
    std::uint32_t effective_rate_millihz_;  // 0 until Granted
    std::uint64_t expires_at_ms_;
    std::uint64_t renew_at_ms_;  // docs/commands.md 4.3: renewal is just another SUBSCRIBE
    std::uint64_t pending_deadline_ms_;  // Pending only -- see kSubscriptionPendingTimeoutMs
};

class SubscriptionClient {
public:
    SubscriptionClient(ClientSubscription* slots, std::size_t slot_count) noexcept;

    bool valid() const noexcept { return slots_ != nullptr && slot_count_ != 0U; }
    std::size_t slot_count() const noexcept { return slot_count_; }

    // Idle -> Pending. Encodes SUBSCRIBE into `out`; the caller sends it with
    // `own_source_id` / `own_boot_id` / `own_sequence` -- whatever identity
    // and sequence the endpoint puts on that frame, since that is the triple
    // SUBSCRIBE_RESULT's RequestRef must echo back, same correlation rule as
    // btp::SessionInitiator::connect(). Returns a local id (!= 0) that names
    // this subscription across renewals, or 0 (no free slot, or a malformed
    // request -- encode_subscribe()'s errors).
    std::uint32_t subscribe(std::uint32_t peer_source_id, std::uint32_t peer_boot_id,
                            std::uint16_t topic_id, std::uint32_t rate_millihz,
                            std::uint32_t lease_ms, std::uint32_t own_source_id,
                            std::uint32_t own_boot_id, std::uint32_t own_sequence,
                            std::uint64_t now_ms, std::uint8_t* out,
                            std::size_t out_capacity, std::size_t* out_size) noexcept;

    // Encodes UNSUBSCRIBE for `local_id` into `out` and frees the slot right
    // away -- does not wait for UNSUBSCRIBE_RESULT, the same v1 simplicity as
    // btp::SessionInitiator::disconnect(). False on an unknown / already-idle
    // id, a capacity error, or a still-Pending id (no subscription_id yet to
    // name on the wire -- wait for on_result() first, or just let expire()
    // reclaim it after kSubscriptionPendingTimeoutMs).
    bool unsubscribe(std::uint32_t local_id, std::uint32_t own_source_id,
                     std::uint32_t own_boot_id, std::uint32_t own_sequence,
                     std::uint8_t* out, std::size_t out_capacity,
                     std::size_t* out_size) noexcept;

    // Feed a decoded SUBSCRIBE_RESULT payload. Finds the Pending slot whose
    // outstanding request correlates (RequestRef match); a Success result
    // moves it to Active and arms the renewal deadline, anything else frees
    // it. event.local_id is 0 (event None) when nothing correlated -- a
    // stale / unrelated result is ignored, the same "keep waiting" rule as
    // btp::SessionInitiator::on_frame.
    SubscriptionOutcome on_result(const SubscribeResult& result,
                                  std::uint64_t now_ms) noexcept;

    // Frees every Active slot whose lease has passed at now_ms without a
    // renewal landing, and every Pending slot past kSubscriptionPendingTimeoutMs
    // with no SUBSCRIBE_RESULT. Silent, like btp::Receiver::expire() -- no
    // event is reported (a caller that needs to know checks whether its
    // local_id is still Active some time later). Call once per tick(),
    // before draining renewals below.
    void expire(std::uint64_t now_ms) noexcept;

    // The local id of an Active slot within its renewal margin (docs/
    // commands.md 4.3: renewal is just another SUBSCRIBE) that has not
    // already had one sent, or 0 if none is due. A pure query -- does not
    // change state. Call renew() with the id it returns.
    std::uint32_t next_renewal_due(std::uint64_t now_ms) const noexcept;

    // Re-sends SUBSCRIBE for `local_id` (as returned by next_renewal_due())
    // with a freshly reserved identity and marks it Pending again -- same
    // subscription_id, a new correlation triple. Same peer / topic / rate /
    // lease as the original subscribe() call. False on an id not currently
    // due for renewal.
    bool renew(std::uint32_t local_id, std::uint32_t own_source_id,
              std::uint32_t own_boot_id, std::uint32_t own_sequence,
              std::uint64_t now_ms, std::uint8_t* out, std::size_t out_capacity,
              std::size_t* out_size) noexcept;

private:
    ClientSubscription* find_by_local_id(std::uint32_t local_id) noexcept;
    ClientSubscription* find_pending(std::uint32_t source_id, std::uint32_t boot_id,
                                     std::uint32_t sequence) noexcept;
    bool build_subscribe(const ClientSubscription& slot, std::uint8_t* out,
                        std::size_t out_capacity, std::size_t* out_size) const noexcept;

    ClientSubscription* slots_;
    std::size_t slot_count_;
    std::uint32_t next_local_id_;
};

}  // namespace btp

#endif  // BTP_SUBSCRIPTION_HPP
