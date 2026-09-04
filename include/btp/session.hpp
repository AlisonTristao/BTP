#ifndef BTP_SESSION_HPP
#define BTP_SESSION_HPP

// The stateful half of a BTP session -- the mechanisms docs/library.md
// chapter 11 keeps *above* btp::messages because they carry state: command
// deduplication (btp::DedupCache) and the session lifecycle / inactivity
// watchdog (btp::Session). This header starts with the one that matters most
// for safety: command deduplication.
//
// btp::messages turns a COMMAND_REQUEST / COMMAND_RESULT payload into a struct
// and back. It does not remember that it saw a request. docs/commands.md
// sections 2.5-2.6 and docs/session-and-terminal.md section 5.3 define what an
// *executor* must remember: the request identity
// (request_source_id, request_boot_id, request_sequence), the COMMAND_RESULT
// it produced, and the rule that a retransmission of that identity replays the
// stored result instead of executing the action a second time. A different
// request that reuses the identity is a REQUEST_CONFLICT. The cache is bounded
// and scoped to the executor boot -- it outlives a session.
//
// btp::DedupCache is that cache, with the same guarantees as the rest of the
// library:
//
//   * no internal allocation -- the caller owns the slot array, one byte
//     region per slot, and the requester table;
//   * noexcept -- errors are returned, never thrown;
//   * no clock -- the cache is boot-scoped with no time-based expiry, so it
//     needs no time source (unlike btp::Reassembler);
//   * no I/O, no global state;
//   * no partial output on failure.
//
// It adds no wire field: every rule it implements is already in the book.
// This is library 2.6.0 territory.
//
// OUT of scope for btp::DedupCache, on purpose (docs/library.md chapter 11
// still applies):
//   * executing the action, and sealing / sending the COMMAND_RESULT -- the
//     integration owns the transport and the executor;
//   * choosing the capacities -- the caller sizes the slot array, the per-slot
//     storage and the requester table for its own deployment.
//
// btp::Session, at the end of this header, is the second inhabitant: the
// session lifecycle and the inactivity watchdog (docs/session-and-terminal.md
// chapters 3-5, 9). It wraps btp::negotiate / the HELLO_RESULT and
// SESSION_CLOSE_RESULT encoders and adds the responder state machine. Library
// 2.9.0 territory. What it does NOT do: the initiator side (sending ENTER /
// HELLO and awaiting HELLO_RESULT), the plain-ASCII console<->protocol text on
// serial, and the priority scheduler -- all the integration's.

#include "btp/codec.hpp"     // ByteView
#include "btp/messages.hpp"  // MessageError

#include <cstddef>
#include <cstdint>

namespace btp {

// ---------------------------------------------------------------------------
// The request identity (docs/commands.md section 2.5)
// ---------------------------------------------------------------------------
// The COMMAND_REQUEST envelope's (source_id, boot_id, sequence). The caller
// reads these from btp::Header before opening the payload.

struct DedupKey {
    std::uint32_t source_id;
    std::uint32_t boot_id;
    std::uint32_t sequence;
};

// ---------------------------------------------------------------------------
// The verdict of a lookup (docs/commands.md sections 2.5-2.6)
// ---------------------------------------------------------------------------

enum class DedupVerdict : std::uint8_t {
    // Unseen identity. *slot_out holds the reserved slot index. The caller
    // executes the action, then calls record_result() with the COMMAND_RESULT
    // it produced (or release() if the execution is abandoned).
    Fresh,

    // Same identity and same request bytes, but the result is not ready yet
    // (a retransmission arrived while the first copy is still executing). The
    // caller drops it -- the peer will retry, and a later lookup returns
    // DuplicateComplete once record_result() has run.
    DuplicateInFlight,

    // Same identity and same request bytes, already executed. *result_out
    // views the stored COMMAND_RESULT payload; the caller retransmits it
    // verbatim and does NOT execute the action.
    DuplicateComplete,

    // Same identity, DIFFERENT request bytes. docs/commands.md section 2.5: a
    // request sequence cannot name two different commands within one requester
    // boot. The caller replies REJECTED / REQUEST_CONFLICT and does not
    // execute.
    Conflict,

    // The identity was handled and then evicted from the bounded cache (its
    // sequence is at or below this requester's evicted high-water mark and it
    // is no longer in a slot). The action already ran once; the stored result
    // is gone. The caller replies BUSY / CAPACITY_EXHAUSTED and does NOT
    // execute -- this is the ring buffer staying safe under pressure.
    Evicted,

    // No slot could be freed for a fresh identity (every slot holds an
    // in-flight execution), the request does not fit a slot's storage, or the
    // requester table is full of distinct devices. docs/commands.md
    // section 2.6: the caller replies BUSY / CAPACITY_EXHAUSTED.
    CapacityExhausted,

    // A null pointer, or a request larger than 0xFFFF octets.
    InvalidArgument,
};

const char* dedup_verdict_string(DedupVerdict verdict) noexcept;

// ---------------------------------------------------------------------------
// Caller-owned storage
// ---------------------------------------------------------------------------

// One byte region per slot. DedupCache stores the logical request verbatim at
// the front and, after record_result(), the COMMAND_RESULT payload after it.
// Size each region for the largest request plus the largest result the
// executor will ever see; a request or result that does not fit is refused
// (CapacityExhausted on classify, BufferTooSmall on record_result) rather than
// truncated.
struct DedupStorage {
    std::uint8_t* data;
    std::size_t capacity;
};

// Metadata for one cached command. Opaque: storage is bound by DedupCache's
// constructor, exactly like btp::ReassemblySlot.
class DedupSlot {
public:
    DedupSlot() noexcept;

private:
    friend class DedupCache;

    enum class State : std::uint8_t { Free, Reserved, Complete };

    State state_;
    DedupKey key_;
    std::uint8_t* data_;
    std::size_t capacity_;
    std::size_t request_size_;
    std::size_t result_size_;
    std::uint32_t reserve_tick_;  // arrival order, for oldest-first eviction
};

// One row per (source_id, boot_id) requester the executor has answered. Opaque.
// Holds the highest request sequence seen and the highest that has been
// evicted, so an evicted identity can be told apart from a genuinely new one
// (docs/commands.md section 2.6: an evicted command must never re-execute).
class DedupRequester {
public:
    DedupRequester() noexcept;

private:
    friend class DedupCache;

    bool used_;
    std::uint32_t source_id_;
    std::uint32_t boot_id_;
    std::uint32_t seen_hwm_;
    std::uint32_t evicted_hwm_;
};

// ---------------------------------------------------------------------------
// DedupCache
// ---------------------------------------------------------------------------
//
//   btp::DedupSlot slots[16];
//   std::uint8_t bytes[16][768];
//   btp::DedupStorage storage[16];
//   for (std::size_t i = 0; i < 16; ++i) storage[i] = {bytes[i], sizeof(bytes[i])};
//   btp::DedupRequester requesters[4];
//   btp::DedupCache cache(slots, storage, 16, requesters, 4);
//
//   // on a COMMAND_REQUEST:
//   btp::DedupKey key{h.source_id, h.boot_id, h.sequence};
//   std::size_t slot = 0;
//   btp::ByteView stored{};
//   switch (cache.classify(key, payload.data, payload.size, &slot, &stored)) {
//       case btp::DedupVerdict::Fresh:
//           run_the_action(payload);
//           encode_command_result(..., result, &n);
//           cache.record_result(slot, result, n);
//           send(result, n);
//           break;
//       case btp::DedupVerdict::DuplicateComplete: send(stored.data, stored.size); break;
//       case btp::DedupVerdict::DuplicateInFlight: /* drop */ break;
//       case btp::DedupVerdict::Conflict:          send_reject(REQUEST_CONFLICT); break;
//       case btp::DedupVerdict::Evicted:
//       case btp::DedupVerdict::CapacityExhausted: send_reject(CAPACITY_EXHAUSTED); break;
//       case btp::DedupVerdict::InvalidArgument:   /* drop */ break;
//   }
//
// classify() and record_result() are not internally synchronised. A caller
// that runs RX on more than one thread wraps the pair in its own critical
// section, the same way btp::Reassembler expects a single consumer.

class DedupCache {
public:
    DedupCache(DedupSlot* slots, const DedupStorage* storage,
               std::size_t slot_count, DedupRequester* requesters,
               std::size_t requester_count) noexcept;

    // True when every pointer is non-null, both counts are non-zero, and each
    // storage region is a non-null buffer of non-zero size. Check before use.
    bool valid() const noexcept;

    std::size_t slot_count() const noexcept { return slot_count_; }

    // Classify `key` against the cache. `request` / `request_size` are the
    // complete logical COMMAND_REQUEST payload (post-reassembly, post-open).
    // On Fresh, *slot_out is the reserved slot and the request has been copied
    // in. On DuplicateComplete, *result_out views the stored COMMAND_RESULT
    // and is valid until that slot is evicted. On every other verdict neither
    // out-parameter is written. Both out pointers may be null.
    DedupVerdict classify(const DedupKey& key, const std::uint8_t* request,
                          std::size_t request_size, std::size_t* slot_out,
                          ByteView* result_out) noexcept;

    // Attach the generated COMMAND_RESULT to a slot reserved by a Fresh
    // classify(). Later duplicates of that identity replay these bytes.
    //   Ok               -- stored; the slot is now Complete.
    //   InvalidArgument  -- slot index out of range, or a null result.
    //   WrongOrder       -- the slot is not Reserved (never classified Fresh,
    //                       already completed, or released).
    //   BufferTooSmall   -- request_size + size exceeds the slot's storage.
    // On Ok, *stored_out (when non-null) views the copy held in the slot --
    // the same bytes a later DuplicateComplete returns, valid until that slot
    // is evicted.
    MessageError record_result(std::size_t slot, const std::uint8_t* result,
                               std::size_t size,
                               ByteView* stored_out = nullptr) noexcept;

    // Free a slot reserved by Fresh whose execution will never produce a
    // result (abandoned, crashed). A retransmission of its identity is then
    // treated as Fresh again. Returns false for an out-of-range index or a
    // slot that is not Reserved.
    bool release(std::size_t slot) noexcept;

    // Drop every entry and every requester row. The deduplication state is
    // scoped to the executor boot (docs/commands.md section 2.6), so a running
    // executor never calls this; it is for test isolation and for a caller
    // that reconstructs identity on some explicit event.
    void clear() noexcept;

    struct Stats {
        std::uint32_t reserved;   // Fresh verdicts (slots handed out)
        std::uint32_t completed;  // record_result() calls that stored a result
        std::uint32_t replayed;   // DuplicateComplete verdicts
        std::uint32_t in_flight;  // DuplicateInFlight verdicts
        std::uint32_t conflicts;  // Conflict verdicts
        std::uint32_t evicted;    // completed entries pushed out of the ring
        std::uint32_t exhausted;  // Evicted + CapacityExhausted verdicts
    };
    Stats stats() const noexcept { return stats_; }

private:
    DedupSlot* find_slot(const DedupKey& key) noexcept;
    DedupRequester* find_requester(std::uint32_t source_id,
                                   std::uint32_t boot_id) noexcept;
    // Returns an existing exact row, reuses a stale-boot row for the same
    // source, takes a free row, or returns null when the table is full of
    // distinct sources.
    DedupRequester* acquire_requester(std::uint32_t source_id,
                                      std::uint32_t boot_id) noexcept;
    // A free slot, or the oldest Complete slot evicted to make room (raising
    // its requester's evicted high-water mark). Null when every slot is
    // Reserved.
    DedupSlot* reserve_slot() noexcept;
    static void reset_slot(DedupSlot& slot) noexcept;
    static bool same_key(const DedupKey& a, const DedupKey& b) noexcept;

    DedupSlot* slots_;
    std::size_t slot_count_;
    DedupRequester* requesters_;
    std::size_t requester_count_;
    std::uint32_t tick_;
    bool valid_;
    Stats stats_;
};

// ===========================================================================
// The session state machine (docs/session-and-terminal.md chapters 3-5, 9)
// ===========================================================================
//
// btp::messages already turns HELLO / HELLO_RESULT / SESSION_CLOSE /
// SESSION_CLOSE_RESULT into structs and back and computes the effective limits
// (btp::negotiate). It does not *run* the session: the lifetime, the
// inactivity watchdog and the console<->protocol transition were the
// integration's to keep, hand-rolled once per consumer.
//
// btp::Session is the responder half of that state machine, with the same
// guarantees as the rest of the library, plus one it shares with
// btp::Reassembler: it takes a clock reading, it does not read a clock. Every
// call that cares about time takes a `now_ms` the caller fills from its own
// monotonic millisecond source -- millis() / esp_timer_get_time()/1000 on an
// MCU, QElapsedTimer::elapsed() under Qt, a plain counter in a test. The
// object only ever computes `now_ms >= deadline`; it never includes <ctime>.
//
//   Session on serial:   Idle --arm()--> AwaitingHello --HELLO--> Active
//                          ^                   |                     |
//                          | timeout / reject  | HELLO deadline      | SESSION_CLOSE,
//                          +-------------------<+  or watchdog        | watchdog, reset()
//                          +--------------------------------------<---+
//
// A transport with no console phase (ESP-NOW, USB HID) starts the same way,
// calling arm() on link-up instead of after a console ENTER line.
//
// OUT of scope, on purpose:
//   * sending ENTER, the retry budget around it, and the plain-ASCII
//     "BTP/1 ENTER|READY|CONSOLE" console text on serial
//     (docs/session-and-terminal.md sections 3-4) -- link framing, above this;
//   * everything past HELLO_RESULT for the initiator side (MANIFEST_REQUEST,
//     SUBSCRIBE, COMMAND_REQUEST) -- btp::SessionInitiator, below, is only the
//     handshake; the caller sends those once connected() is true;
//   * routing an accepted frame by object_id, the priority scheduler, and the
//     STATUS counters -- all the integration's.

enum class SessionState : std::uint8_t {
    Idle,           // no session. on_frame() ignores frames -- a serial
                    // "console", or a link not yet armed. arm() leaves this.
    AwaitingHello,  // armed: the HELLO deadline runs, awaiting the peer's HELLO.
    Active,         // HELLO_RESULT SUCCESS sent; the inactivity watchdog runs.
};

const char* session_state_string(SessionState state) noexcept;

enum class SessionEvent : std::uint8_t {
    None,           // nothing to do: a stray frame in Idle, or a non-HELLO
                    // frame while AwaitingHello (which does NOT renew the
                    // deadline -- "no message before HELLO").
    HelloAccepted,  // *reply_out is HELLO_RESULT SUCCESS; state is now Active.
    HelloRejected,  // *reply_out is HELLO_RESULT UNSUPPORTED (malformed HELLO
                    // or no common version); state is back to Idle.
    FrameAccepted,  // a valid frame renewed the watchdog; the caller routes it
                    // by object_id. No reply.
    SessionClosed,  // *reply_out is SESSION_CLOSE_RESULT; state is back to Idle.
    TimedOut,       // the HELLO deadline (AwaitingHello) or the negotiated
                    // session_timeout_ms (Active) expired; state is back to
                    // Idle. Reported once. No reply.
    Abandoned,      // reset() tore down a live session; state is Idle. Reported
                    // once. No reply -- nobody is left to receive one.
};

const char* session_event_string(SessionEvent event) noexcept;

// The largest reply on_frame() writes is HELLO_RESULT: 52 octets
// (docs/session-and-terminal.md section 2). SESSION_CLOSE_RESULT is 16. Size
// the reply_out buffer for the maximum.
static const std::size_t kSessionMaxReplySize = 52U;

struct SessionOutcome {
    SessionEvent event;
    // Octets written to reply_out. Non-zero only for HelloAccepted,
    // HelloRejected and SessionClosed.
    std::size_t reply_size;
};

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------
//
//   btp::Hello local{};
//   local.role = static_cast<std::uint8_t>(btp::Role::Producer);
//   local.version_count = 1; local.versions[0] = 1;
//   local.max_logical_payload = 2048;
//   local.max_inflight_reassemblies = 1;
//   local.max_subscriptions = 8;
//   local.max_dedup_entries = 32;
//   local.session_timeout_ms = 30000;
//   std::memcpy(local.peer_uuid, my_uuid, 16);
//   local.config_revision = manifest_revision;
//
//   btp::Session session(local, /*hello_deadline_ms=*/2000);
//
//   session.arm(now_ms());                        // after the console ENTER line
//
//   // per decoded frame (from btp::decode / btp::Receiver):
//   std::uint8_t reply[btp::kSessionMaxReplySize];
//   btp::SessionOutcome o =
//       session.on_frame(frame, now_ms(), reply, sizeof(reply));
//   switch (o.event) {
//       case btp::SessionEvent::HelloAccepted:
//       case btp::SessionEvent::HelloRejected:
//       case btp::SessionEvent::SessionClosed: send(reply, o.reply_size); break;
//       case btp::SessionEvent::FrameAccepted: route(frame);               break;
//       default: break;
//   }
//
//   // from the main loop / a timer:
//   if (session.poll(now_ms()).event == btp::SessionEvent::TimedOut)
//       back_to_console();
//
// on_frame() and poll() are not internally synchronised -- a caller that runs
// RX and the timer on different threads wraps them in its own critical
// section, the same way btp::Reassembler expects a single consumer.

class Session {
public:
    // `local` is this peer's own HELLO advertisement -- role, announced
    // versions, announced limits, peer_uuid, config_revision. btp::negotiate()
    // mins its limits against the remote HELLO; the HELLO_RESULT reply reuses
    // its peer_uuid and config_revision. Copied in -- the caller need not keep
    // it alive.
    //
    // `hello_deadline_ms` is how long AwaitingHello waits for the peer's HELLO
    // (docs/session-and-terminal.md section 5.1: 2000 for a serial console; a
    // transport with no such bound passes a large value). 0 disables the HELLO
    // deadline entirely -- only the negotiated session watchdog then applies.
    Session(const Hello& local, std::uint64_t hello_deadline_ms) noexcept;

    // True when the current local advertisement is a well-formed HELLO (a
    // valid role, 1..8 ascending non-zero versions, non-zero limits, non-zero
    // uuid). A false here means every HELLO will be rejected -- check it after
    // construction and after set_local().
    bool valid() const noexcept;

    // Replace the local advertisement. A peer whose config_revision or
    // announced limits change (its manifest catalogue moved, say) calls this
    // between sessions so the next HELLO_RESULT reports the current values.
    // Safe in any state -- local_ is only read while negotiating a HELLO, so a
    // call during an active session changes nothing until that session ends.
    // Returns the new valid().
    bool set_local(const Hello& local) noexcept;

    SessionState state() const noexcept;
    bool active() const noexcept;   // state() == SessionState::Active

    // Meaningful once a HELLO has been accepted (state() has reached Active at
    // least once). Before that, zero-initialised.
    const EffectiveLimits& effective_limits() const noexcept;
    std::uint32_t peer_source_id() const noexcept;
    std::uint32_t peer_boot_id() const noexcept;

    // Idle -> AwaitingHello, arming the HELLO deadline at now_ms. A serial
    // integration calls this once it has answered the console ENTER line; a
    // transport with no console phase calls it when the link comes up. A no-op
    // in any other state -- call reset() first to re-arm a live session.
    void arm(std::uint64_t now_ms) noexcept;

    // Feed every frame btp::decode() / btp::Receiver accepted. `frame` must be
    // a fully decoded envelope -- a CRC or COBS failure never reaches here and
    // must not renew the watchdog (docs section 5.2). Expiry is checked first
    // (like btp::Receiver::submit sweeping timeouts), so a frame that arrives
    // after the deadline is reported as TimedOut and dropped. In Active, any
    // valid frame renews the watchdog before it is classified.
    //
    // reply_out / reply_capacity receive HELLO_RESULT or SESSION_CLOSE_RESULT
    // when the event carries one; size reply_out >= kSessionMaxReplySize. A
    // reply that will not fit leaves the state unchanged and returns None.
    SessionOutcome on_frame(const DecodedFrame& frame, std::uint64_t now_ms,
                            std::uint8_t* reply_out,
                            std::size_t reply_capacity) noexcept;

    // Call from the main loop or a timer. Returns TimedOut exactly once when
    // the HELLO deadline (AwaitingHello) or the negotiated session_timeout_ms
    // (Active) has passed at now_ms; None otherwise. The cadence of poll()
    // only bounds how late a dead session is noticed -- 100..500 ms is ample
    // against a 2 s / 30 s deadline.
    SessionOutcome poll(std::uint64_t now_ms) noexcept;

    // The underlying transport is gone (serial DTR dropped, an ESP-NOW peer
    // forgotten), or a deliberate teardown. Returns Abandoned once if a
    // session was live (AwaitingHello or Active), None otherwise. Produces no
    // reply. Doubles as test isolation.
    SessionOutcome reset() noexcept;

private:
    SessionOutcome check_expiry(std::uint64_t now_ms) noexcept;
    std::size_t build_hello_result(const Header& request, bool success,
                                   std::uint8_t* out,
                                   std::size_t capacity) noexcept;
    std::size_t build_session_close_result(const Header& request, bool parsed,
                                           std::uint8_t* out,
                                           std::size_t capacity) noexcept;

    Hello local_;
    EffectiveLimits effective_;
    SessionState state_;
    std::uint64_t hello_deadline_ms_;
    std::uint64_t deadline_ms_;
    std::uint32_t peer_source_id_;
    std::uint32_t peer_boot_id_;
    bool valid_;
};

// ===========================================================================
// The session initiator (docs/session-and-terminal.md chapters 1-2, 5, 9)
// ===========================================================================
//
// The other end of a session: the peer that connects, mirroring btp::Session.
// Sends HELLO, awaits HELLO_RESULT within a deadline, and -- once Active --
// runs the same inactivity watchdog Session does (any valid frame from the
// peer renews it; docs/session-and-terminal.md section 5: "a valid BTP
// frame"). A second attempt at this piece: an earlier btp::SessionInitiator
// (library 2.11.0) was tried and reverted because it mixed the serial-console
// ENTER retry -- a one-link gambit, not spec -- into the handshake. This one
// stays deliberately narrow:
//
//   * no ENTER / READY text -- link framing, above this (docs/session-and-
//     terminal.md sections 3-4 stay a valid transport profile; this class
//     just never speaks it);
//   * no retry budget for a HELLO that gets no answer -- one attempt, one
//     deadline; a caller that wants retries calls connect() again after
//     TimedOut / Rejected;
//   * no MANIFEST_REQUEST / SUBSCRIBE / COMMAND_REQUEST follow-up -- the
//     caller sends those once connected() is true, same as any other message;
//   * an incoming SESSION_CLOSE from the peer is not specially recognised --
//     it comes back as an ordinary FrameAccepted for the caller to notice by
//     object_id and call reset().
//
// Same guarantees as btp::Session: no clock of its own (every call that cares
// about time takes a now_ms the caller supplies), not internally
// synchronised, no I/O -- connect() / disconnect() hand back the PAYLOAD
// bytes to send; the caller (btp::Node) frames and transmits them, exactly
// like Session::on_frame hands back a HELLO_RESULT / SESSION_CLOSE_RESULT
// payload rather than sending it itself.
//
//   btp::Hello local{...};
//   btp::SessionInitiator initiator;
//   std::uint8_t hello[btp::kSessionMaxHelloSize];
//   std::size_t n = 0;
//   if (initiator.connect(local, my_source_id, my_boot_id, my_sequence,
//                         now_ms(), /*deadline_ms=*/2000, hello, sizeof(hello),
//                         &n))
//       send(hello, n);
//
//   // per decoded frame:
//   btp::InitiatorOutcome o = initiator.on_frame(frame, now_ms());
//   switch (o.event) {
//       case btp::InitiatorEvent::Connected:     /* effective_limits() set */ break;
//       case btp::InitiatorEvent::Rejected:
//       case btp::InitiatorEvent::TimedOut:      /* retry: connect() again */ break;
//       case btp::InitiatorEvent::FrameAccepted: route(frame);                break;
//       default: break;
//   }
//
//   // from the main loop / a timer:
//   initiator.poll(now_ms());   // TimedOut when the deadline / watchdog expires
//
// on_frame() and poll() are not internally synchronised -- the same one-
// context rule as btp::Session / btp::Reassembler.

enum class InitiatorState : std::uint8_t {
    Idle,            // no outstanding HELLO. on_frame() ignores every frame.
    AwaitingResult,  // connect() sent a HELLO; the deadline runs.
    Active,          // HELLO_RESULT SUCCESS arrived; the inactivity watchdog runs.
};

const char* initiator_state_string(InitiatorState state) noexcept;

enum class InitiatorEvent : std::uint8_t {
    None,           // nothing to act on: Idle, or a frame that is not the
                    // awaited HELLO_RESULT (which does NOT renew the
                    // deadline), or one whose RequestRef does not match the
                    // outstanding HELLO (stale noise -- kept waiting).
    Connected,      // HELLO_RESULT SUCCESS, correlated; state is now Active,
                    // effective_limits() / peer_source_id() / peer_boot_id()
                    // are set.
    Rejected,       // HELLO_RESULT UNSUPPORTED, correlated; state is back to
                    // Idle.
    FrameAccepted,  // Active: a valid frame renewed the watchdog; the caller
                    // routes it by object_id, same as btp::Session. No reply.
    TimedOut,       // the connect() deadline, or the negotiated
                    // session_timeout_ms, expired; state is back to Idle.
                    // Reported once.
    Disconnected,   // disconnect() or reset() tore down a live session
                    // (AwaitingResult or Active); state is Idle. Reported once.
};

const char* initiator_event_string(InitiatorEvent event) noexcept;

struct InitiatorOutcome {
    InitiatorEvent event;
};

// HELLO is the larger of the two payloads this class ever encodes (40 fixed
// octets plus up to 8 announced versions -- docs/session-and-terminal.md
// section 1). SESSION_CLOSE fits easily (5 octets). Size connect() /
// disconnect()'s out buffer for the maximum.
static const std::size_t kSessionMaxHelloSize = 48U;

class SessionInitiator {
public:
    SessionInitiator() noexcept;

    InitiatorState state() const noexcept { return state_; }
    bool connected() const noexcept { return state_ == InitiatorState::Active; }

    // Meaningful once Connected has fired at least once.
    const EffectiveLimits& effective_limits() const noexcept { return effective_; }
    std::uint32_t peer_source_id() const noexcept { return peer_source_id_; }
    std::uint32_t peer_boot_id() const noexcept { return peer_boot_id_; }
    // HELLO_RESULT's own config_revision (session-and-terminal.md section 2,
    // offset 48) -- the responder's manifest-catalogue revision, reported
    // as-is, never negotiated (unlike everything in EffectiveLimits, which is
    // a field-by-field minimum of both sides -- this is why it lives here and
    // not there). A caller that skips a redundant MANIFEST_REQUEST enumeration
    // when this has not changed since the last session (ManifestClient's own
    // job) needs it; nothing in this class reads it back.
    std::uint32_t peer_config_revision() const noexcept { return peer_config_revision_; }

    // Idle -> AwaitingResult. Encodes `local` as a HELLO into `out`; the
    // caller sends it framed with `own_source_id` / `own_boot_id` /
    // `own_sequence` -- whatever identity and sequence the endpoint puts on
    // that frame, since that is the triple HELLO_RESULT's RequestRef must
    // echo back for on_frame() to accept it. `deadline_ms` is how long to
    // wait for HELLO_RESULT (docs/session-and-terminal.md section 5.1: 2000
    // for a serial console; a transport with no such bound passes a large
    // value; 0 disables the deadline entirely, mirroring btp::Session's
    // hello_deadline_ms). A no-op (returns false, writes nothing) outside
    // Idle -- call disconnect() or wait for TimedOut / Rejected first, or on
    // a malformed `local` or an `out` too small (encode_hello()'s errors).
    bool connect(const Hello& local, std::uint32_t own_source_id,
                std::uint32_t own_boot_id, std::uint32_t own_sequence,
                std::uint64_t now_ms, std::uint64_t deadline_ms,
                std::uint8_t* out, std::size_t out_capacity,
                std::size_t* out_size) noexcept;

    // Feed every frame btp::decode() / btp::Receiver accepted, same contract
    // as btp::Session::on_frame. Expiry is checked first. In AwaitingResult,
    // only a HELLO_RESULT that decodes and whose RequestRef matches the
    // outstanding connect() progresses the state; anything else is None and
    // does NOT renew the deadline (docs section 1: "no application message
    // before a successful HELLO_RESULT"). In Active, every valid frame
    // renews the watchdog before it is classified as FrameAccepted.
    InitiatorOutcome on_frame(const DecodedFrame& frame,
                              std::uint64_t now_ms) noexcept;

    // Call from the main loop or a timer. Returns TimedOut exactly once when
    // the connect() deadline (AwaitingResult) or the negotiated
    // session_timeout_ms (Active) has passed at now_ms; None otherwise.
    InitiatorOutcome poll(std::uint64_t now_ms) noexcept;

    // Encodes a SESSION_CLOSE into `out` and tears the session down locally
    // right away -- it does not wait for SESSION_CLOSE_RESULT, keeping this
    // symmetric with the deliberately narrow HELLO -> HELLO_RESULT scope
    // above. A no-op (returns false) in Idle.
    bool disconnect(std::uint64_t now_ms, std::uint8_t reason,
                    std::uint32_t drain_timeout_ms, std::uint8_t* out,
                    std::size_t out_capacity, std::size_t* out_size) noexcept;

    // The underlying transport is gone, or a deliberate teardown with no
    // SESSION_CLOSE sent. Returns Disconnected once if a session was live
    // (AwaitingResult or Active), None otherwise. Doubles as test isolation.
    InitiatorOutcome reset() noexcept;

private:
    InitiatorOutcome check_expiry(std::uint64_t now_ms) noexcept;

    InitiatorState state_;
    EffectiveLimits effective_;
    std::uint64_t hello_deadline_span_;  // the connect() argument, kept for
                                         // the "0 disables" check on re-arm
    std::uint64_t deadline_ms_;
    std::uint32_t own_source_id_;
    std::uint32_t own_boot_id_;
    std::uint32_t own_sequence_;
    std::uint32_t peer_source_id_;
    std::uint32_t peer_boot_id_;
    std::uint32_t peer_config_revision_;
};

// ===========================================================================
// CommandClient -- the INITIATOR half of commands (docs/commands.md section 2)
// ===========================================================================
//
// btp::DedupCache above is the RESPONDER's half: execute once, remember the
// result, replay a retransmission. CommandClient is the other end: send
// COMMAND_REQUEST, correlate the eventual COMMAND_RESULT against it -- the
// same RequestRef-matching rule btp::SessionInitiator / a
// btp::SubscriptionClient slot use for HELLO_RESULT / SUBSCRIBE_RESULT.
//
// No I/O, no buffering of the result: on_result() hands back the DECODED
// CommandResult's own `message` / `result` ByteViews unchanged (they already
// view the caller's reassembly buffer) rather than copying them into a
// second buffer of its own -- valid for exactly as long as that buffer is
// (docs/library.md section 2.5's zero-copy rule), which in practice means
// "until the next receive()", same as any other decoded payload. No retry
// budget -- a command that gets no answer within kCommandTimeoutMs reports
// TimedOut and the caller decides whether to call command() again, the same
// stance as btp::SessionInitiator / btp::SubscriptionClient.

enum class CommandEvent : std::uint8_t {
    None,       // nothing to act on: no free slot, or a result that did not correlate
    Completed,  // a correlated COMMAND_RESULT arrived -- status / error_code /
                // message / result below are it, whatever the action decided
                // (Success is not implied -- check status).
    TimedOut,   // no COMMAND_RESULT within kCommandTimeoutMs -- slot freed.
};

const char* command_event_string(CommandEvent event) noexcept;

// How long command() waits for COMMAND_RESULT before expire() frees the slot.
static const std::uint64_t kCommandTimeoutMs = 5000U;

struct CommandOutcome {
    CommandEvent event;
    std::uint32_t local_id;   // which slot -- 0 when event == None
    std::uint8_t status;      // ResultStatus -- meaningful only on Completed
    std::uint16_t error_code; // ResultError  -- meaningful only on Completed
    ByteView message;         // meaningful only on Completed; see the zero-copy note above
    ByteView result;          // ditto
};

// One outstanding COMMAND_REQUEST. Opaque -- storage is bound by
// CommandClient's constructor.
class ClientCommand {
public:
    ClientCommand() noexcept;

private:
    friend class CommandClient;

    bool pending_;
    std::uint32_t local_id_;
    std::uint32_t own_source_id_;
    std::uint32_t own_boot_id_;
    std::uint32_t own_sequence_;
    std::uint64_t deadline_ms_;
};

class CommandClient {
public:
    CommandClient(ClientCommand* slots, std::size_t slot_count) noexcept;

    bool valid() const noexcept { return slots_ != nullptr && slot_count_ != 0U; }
    std::size_t slot_count() const noexcept { return slot_count_; }

    // Encodes COMMAND_REQUEST into `out`; the caller sends it with
    // `own_source_id` / `own_boot_id` / `own_sequence` -- the identity and
    // sequence the endpoint puts on that frame, since that is the triple
    // COMMAND_RESULT's RequestRef must echo back. Returns a local id (!= 0)
    // to recognise the eventual on_result() outcome by, or 0 (no free slot,
    // or a malformed request -- encode_command_request()'s errors).
    std::uint32_t command(std::uint32_t target_source_id, std::uint32_t target_boot_id,
                          std::uint16_t action_id, std::uint16_t action_version,
                          const std::uint8_t* parameters, std::size_t parameters_size,
                          std::uint32_t own_source_id, std::uint32_t own_boot_id,
                          std::uint32_t own_sequence, std::uint64_t now_ms,
                          std::uint8_t* out, std::size_t out_capacity,
                          std::size_t* out_size) noexcept;

    // Feed a decoded COMMAND_RESULT. Finds the slot whose outstanding
    // request correlates (RequestRef match) and frees it either way --
    // there is no further state to keep once the result is in the caller's
    // hands. event.local_id is 0 (event None) when nothing correlated (a
    // stale / unrelated result -- any slot still waiting is untouched).
    CommandOutcome on_result(const CommandResult& result) noexcept;

    // Frees ONE Pending slot past kCommandTimeoutMs at now_ms and reports it
    // (TimedOut), or None if none is due. Call in a loop
    // (`while (client.expire(now_ms).event != CommandEvent::None) {}`) to
    // drain every timed-out slot in one tick().
    CommandOutcome expire(std::uint64_t now_ms) noexcept;

private:
    ClientCommand* find_pending(std::uint32_t source_id, std::uint32_t boot_id,
                               std::uint32_t sequence) noexcept;

    ClientCommand* slots_;
    std::size_t slot_count_;
    std::uint32_t next_local_id_;
};

}  // namespace btp

#endif  // BTP_SESSION_HPP
