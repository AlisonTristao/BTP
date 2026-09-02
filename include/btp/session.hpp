#ifndef BTP_SESSION_HPP
#define BTP_SESSION_HPP

// The stateful half of a BTP session -- the mechanisms docs/library.md
// chapter 11 keeps *above* btp::messages because they carry state, starting
// with the one that matters most for safety: command deduplication.
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
// OUT of scope, on purpose (docs/library.md chapter 11 still applies):
//   * executing the action, and sealing / sending the COMMAND_RESULT -- the
//     integration owns the transport and the executor;
//   * the session inactivity watchdog (docs/session-and-terminal.md chapter 5)
//     -- a future inhabitant of this header;
//   * choosing the capacities -- the caller sizes the slot array, the per-slot
//     storage and the requester table for its own deployment.

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

}  // namespace btp

#endif  // BTP_SESSION_HPP
