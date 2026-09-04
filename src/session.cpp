#include "btp/session.hpp"

#include <cstring>

namespace btp {
namespace {

// Envelope sequence and the eviction tick are uint32 counters. Both would take
// billions of commands in one executor boot to wrap, so a plain comparison is
// used and the wrap case is left undefined (documented in the header).
bool seq_greater(std::uint32_t a, std::uint32_t b) noexcept { return a > b; }

}  // namespace

// ---------------------------------------------------------------------------

DedupSlot::DedupSlot() noexcept
    : state_(State::Free),
      key_(),
      data_(nullptr),
      capacity_(0U),
      request_size_(0U),
      result_size_(0U),
      reserve_tick_(0U) {}

DedupRequester::DedupRequester() noexcept
    : used_(false),
      source_id_(0U),
      boot_id_(0U),
      seen_hwm_(0U),
      evicted_hwm_(0U) {}

// ---------------------------------------------------------------------------

DedupCache::DedupCache(DedupSlot* slots, const DedupStorage* storage,
                       std::size_t slot_count, DedupRequester* requesters,
                       std::size_t requester_count) noexcept
    : slots_(slots),
      slot_count_(slot_count),
      requesters_(requesters),
      requester_count_(requester_count),
      tick_(0U),
      valid_(false),
      stats_() {
    if (slots == nullptr || storage == nullptr || slot_count == 0U ||
        requesters == nullptr || requester_count == 0U) {
        return;
    }

    bool ok = true;
    for (std::size_t index = 0U; index < slot_count; ++index) {
        slots_[index] = DedupSlot();
        if (storage[index].data == nullptr || storage[index].capacity == 0U) {
            ok = false;
        }
        slots_[index].data_ = storage[index].data;
        slots_[index].capacity_ = storage[index].capacity;
    }
    for (std::size_t index = 0U; index < requester_count; ++index) {
        requesters_[index] = DedupRequester();
    }
    valid_ = ok;
}

bool DedupCache::valid() const noexcept { return valid_; }

void DedupCache::reset_slot(DedupSlot& slot) noexcept {
    slot.state_ = DedupSlot::State::Free;
    slot.key_ = DedupKey();
    slot.request_size_ = 0U;
    slot.result_size_ = 0U;
    slot.reserve_tick_ = 0U;
    // data_ / capacity_ stay bound to the caller's storage region.
}

bool DedupCache::same_key(const DedupKey& a, const DedupKey& b) noexcept {
    return a.source_id == b.source_id && a.boot_id == b.boot_id &&
           a.sequence == b.sequence;
}

DedupSlot* DedupCache::find_slot(const DedupKey& key) noexcept {
    for (std::size_t index = 0U; index < slot_count_; ++index) {
        DedupSlot& slot = slots_[index];
        if (slot.state_ != DedupSlot::State::Free && same_key(slot.key_, key)) {
            return &slot;
        }
    }
    return nullptr;
}

DedupRequester* DedupCache::find_requester(std::uint32_t source_id,
                                          std::uint32_t boot_id) noexcept {
    for (std::size_t index = 0U; index < requester_count_; ++index) {
        DedupRequester& row = requesters_[index];
        if (row.used_ && row.source_id_ == source_id &&
            row.boot_id_ == boot_id) {
            return &row;
        }
    }
    return nullptr;
}

DedupRequester* DedupCache::acquire_requester(std::uint32_t source_id,
                                              std::uint32_t boot_id) noexcept {
    DedupRequester* exact = find_requester(source_id, boot_id);
    if (exact != nullptr) {
        return exact;
    }

    DedupRequester* free_row = nullptr;
    DedupRequester* stale_same_source = nullptr;
    for (std::size_t index = 0U; index < requester_count_; ++index) {
        DedupRequester& row = requesters_[index];
        if (!row.used_) {
            if (free_row == nullptr) {
                free_row = &row;
            }
            continue;
        }
        // A new boot_id from a source means the old boot is gone; a command
        // for a stale boot is rejected STALE_TARGET_BOOT anyway
        // (docs/commands.md section 2.2), so its dedup rows are safe to reuse.
        if (row.source_id_ == source_id) {
            stale_same_source = &row;
        }
    }

    DedupRequester* row = (free_row != nullptr) ? free_row : stale_same_source;
    if (row == nullptr) {
        return nullptr;  // table full of distinct sources
    }
    row->used_ = true;
    row->source_id_ = source_id;
    row->boot_id_ = boot_id;
    row->seen_hwm_ = 0U;
    row->evicted_hwm_ = 0U;
    return row;
}

DedupSlot* DedupCache::reserve_slot() noexcept {
    DedupSlot* free_slot = nullptr;
    DedupSlot* oldest_complete = nullptr;
    for (std::size_t index = 0U; index < slot_count_; ++index) {
        DedupSlot& slot = slots_[index];
        if (slot.state_ == DedupSlot::State::Free) {
            if (free_slot == nullptr) {
                free_slot = &slot;
            }
            continue;
        }
        if (slot.state_ == DedupSlot::State::Complete) {
            if (oldest_complete == nullptr ||
                slot.reserve_tick_ < oldest_complete->reserve_tick_) {
                oldest_complete = &slot;
            }
        }
    }

    if (free_slot != nullptr) {
        return free_slot;
    }
    if (oldest_complete == nullptr) {
        return nullptr;  // every slot holds an in-flight execution
    }

    // Evict the oldest completed entry and remember that this requester's
    // sequence has been handled up to at least the evicted one, so a later
    // retransmission of it returns Evicted instead of re-executing.
    DedupRequester* row = find_requester(oldest_complete->key_.source_id,
                                         oldest_complete->key_.boot_id);
    if (row != nullptr &&
        seq_greater(oldest_complete->key_.sequence, row->evicted_hwm_)) {
        row->evicted_hwm_ = oldest_complete->key_.sequence;
    }
    reset_slot(*oldest_complete);
    ++stats_.evicted;
    return oldest_complete;
}

DedupVerdict DedupCache::classify(const DedupKey& key,
                                  const std::uint8_t* request,
                                  std::size_t request_size,
                                  std::size_t* slot_out,
                                  ByteView* result_out) noexcept {
    if (!valid_ || (request == nullptr && request_size != 0U) ||
        request_size > 0xFFFFU) {
        return DedupVerdict::InvalidArgument;
    }

    // 1. Already tracked?
    DedupSlot* hit = find_slot(key);
    if (hit != nullptr) {
        const bool identical =
            hit->request_size_ == request_size &&
            (request_size == 0U ||
             std::memcmp(hit->data_, request, request_size) == 0);
        if (!identical) {
            ++stats_.conflicts;
            return DedupVerdict::Conflict;
        }
        if (hit->state_ == DedupSlot::State::Reserved) {
            ++stats_.in_flight;
            return DedupVerdict::DuplicateInFlight;
        }
        ++stats_.replayed;
        if (result_out != nullptr) {
            *result_out = ByteView{hit->data_ + hit->request_size_,
                                   hit->result_size_};
        }
        return DedupVerdict::DuplicateComplete;
    }

    // 2. Handled and evicted?
    DedupRequester* known = find_requester(key.source_id, key.boot_id);
    if (known != nullptr && !seq_greater(key.sequence, known->evicted_hwm_) &&
        known->evicted_hwm_ != 0U) {
        ++stats_.exhausted;
        return DedupVerdict::Evicted;
    }

    // 3. Fresh: needs a slot and a requester row.
    DedupSlot* slot = reserve_slot();
    if (slot == nullptr) {
        ++stats_.exhausted;
        return DedupVerdict::CapacityExhausted;
    }
    if (request_size > slot->capacity_) {
        // Cannot store the request, so a later retransmission could not be
        // recognised -- refuse rather than run an action we cannot protect.
        // reserve_slot() may have evicted for this; that entry stays safely
        // Evicted.
        reset_slot(*slot);
        ++stats_.exhausted;
        return DedupVerdict::CapacityExhausted;
    }
    DedupRequester* row = acquire_requester(key.source_id, key.boot_id);
    if (row == nullptr) {
        reset_slot(*slot);
        ++stats_.exhausted;
        return DedupVerdict::CapacityExhausted;
    }

    slot->state_ = DedupSlot::State::Reserved;
    slot->key_ = key;
    slot->request_size_ = request_size;
    slot->result_size_ = 0U;
    slot->reserve_tick_ = tick_++;
    if (request_size != 0U) {
        std::memcpy(slot->data_, request, request_size);
    }
    if (seq_greater(key.sequence, row->seen_hwm_)) {
        row->seen_hwm_ = key.sequence;
    }
    ++stats_.reserved;
    if (slot_out != nullptr) {
        *slot_out = static_cast<std::size_t>(slot - slots_);
    }
    return DedupVerdict::Fresh;
}

MessageError DedupCache::record_result(std::size_t slot,
                                       const std::uint8_t* result,
                                       std::size_t size,
                                       ByteView* stored_out) noexcept {
    if (!valid_ || slot >= slot_count_ ||
        (result == nullptr && size != 0U)) {
        return MessageError::InvalidArgument;
    }
    DedupSlot& entry = slots_[slot];
    if (entry.state_ != DedupSlot::State::Reserved) {
        return MessageError::WrongOrder;
    }
    if (size > entry.capacity_ - entry.request_size_) {
        return MessageError::BufferTooSmall;
    }
    if (size != 0U) {
        std::memcpy(entry.data_ + entry.request_size_, result, size);
    }
    entry.result_size_ = size;
    entry.state_ = DedupSlot::State::Complete;
    ++stats_.completed;
    if (stored_out != nullptr) {
        *stored_out = ByteView{entry.data_ + entry.request_size_, size};
    }
    return MessageError::Ok;
}

bool DedupCache::release(std::size_t slot) noexcept {
    if (!valid_ || slot >= slot_count_) {
        return false;
    }
    DedupSlot& entry = slots_[slot];
    if (entry.state_ != DedupSlot::State::Reserved) {
        return false;
    }
    reset_slot(entry);
    return true;
}

void DedupCache::clear() noexcept {
    for (std::size_t index = 0U; index < slot_count_; ++index) {
        reset_slot(slots_[index]);
    }
    for (std::size_t index = 0U; index < requester_count_; ++index) {
        requesters_[index] = DedupRequester();
    }
    tick_ = 0U;
    stats_ = Stats();
}

const char* dedup_verdict_string(DedupVerdict verdict) noexcept {
    switch (verdict) {
        case DedupVerdict::Fresh: return "Fresh";
        case DedupVerdict::DuplicateInFlight: return "DuplicateInFlight";
        case DedupVerdict::DuplicateComplete: return "DuplicateComplete";
        case DedupVerdict::Conflict: return "Conflict";
        case DedupVerdict::Evicted: return "Evicted";
        case DedupVerdict::CapacityExhausted: return "CapacityExhausted";
        case DedupVerdict::InvalidArgument: return "InvalidArgument";
    }
    return "Unknown";
}

// ===========================================================================
// Session -- the responder state machine (docs/session-and-terminal.md 3-5, 9)
// ===========================================================================

const char* session_state_string(SessionState state) noexcept {
    switch (state) {
        case SessionState::Idle: return "Idle";
        case SessionState::AwaitingHello: return "AwaitingHello";
        case SessionState::Active: return "Active";
    }
    return "Unknown";
}

const char* session_event_string(SessionEvent event) noexcept {
    switch (event) {
        case SessionEvent::None: return "None";
        case SessionEvent::HelloAccepted: return "HelloAccepted";
        case SessionEvent::HelloRejected: return "HelloRejected";
        case SessionEvent::FrameAccepted: return "FrameAccepted";
        case SessionEvent::SessionClosed: return "SessionClosed";
        case SessionEvent::TimedOut: return "TimedOut";
        case SessionEvent::Abandoned: return "Abandoned";
    }
    return "Unknown";
}

namespace {

// "The local advertisement is a legal HELLO" is exactly what encode_hello
// checks -- a valid role, 1..8 ascending non-zero versions, non-zero limits, a
// non-zero uuid. A HELLO is at most 48 octets (40 fixed + 8 versions), so a
// 64-byte scratch never limits the check.
bool hello_is_well_formed(const Hello& hello) noexcept {
    std::uint8_t scratch[64];
    std::size_t written = 0U;
    return encode_hello(hello, scratch, sizeof(scratch), &written) ==
           MessageError::Ok;
}

}  // namespace

Session::Session(const Hello& local, std::uint64_t hello_deadline_ms) noexcept
    : local_(local),
      effective_{},
      state_(SessionState::Idle),
      hello_deadline_ms_(hello_deadline_ms),
      deadline_ms_(0U),
      peer_source_id_(0U),
      peer_boot_id_(0U),
      valid_(hello_is_well_formed(local)) {}

bool Session::valid() const noexcept { return valid_; }

bool Session::set_local(const Hello& local) noexcept {
    local_ = local;
    valid_ = hello_is_well_formed(local_);
    return valid_;
}
SessionState Session::state() const noexcept { return state_; }
bool Session::active() const noexcept {
    return state_ == SessionState::Active;
}
const EffectiveLimits& Session::effective_limits() const noexcept {
    return effective_;
}
std::uint32_t Session::peer_source_id() const noexcept {
    return peer_source_id_;
}
std::uint32_t Session::peer_boot_id() const noexcept { return peer_boot_id_; }

void Session::arm(std::uint64_t now_ms) noexcept {
    if (state_ != SessionState::Idle) {
        return;  // re-arm a live session through reset() first
    }
    state_ = SessionState::AwaitingHello;
    deadline_ms_ = now_ms + hello_deadline_ms_;
    peer_source_id_ = 0U;
    peer_boot_id_ = 0U;
}

SessionOutcome Session::check_expiry(std::uint64_t now_ms) noexcept {
    if (state_ == SessionState::AwaitingHello) {
        if (hello_deadline_ms_ != 0U && now_ms >= deadline_ms_) {
            state_ = SessionState::Idle;
            return SessionOutcome{SessionEvent::TimedOut, 0U};
        }
    } else if (state_ == SessionState::Active) {
        // session_timeout_ms is a negotiated minimum of two non-zero announced
        // limits, so it is non-zero once Active; the guard is defensive.
        if (effective_.session_timeout_ms != 0U && now_ms >= deadline_ms_) {
            state_ = SessionState::Idle;
            return SessionOutcome{SessionEvent::TimedOut, 0U};
        }
    }
    return SessionOutcome{SessionEvent::None, 0U};
}

SessionOutcome Session::poll(std::uint64_t now_ms) noexcept {
    return check_expiry(now_ms);
}

SessionOutcome Session::reset() noexcept {
    if (state_ == SessionState::Idle) {
        return SessionOutcome{SessionEvent::None, 0U};
    }
    state_ = SessionState::Idle;
    return SessionOutcome{SessionEvent::Abandoned, 0U};
}

std::size_t Session::build_hello_result(const Header& request, bool success,
                                        std::uint8_t* out,
                                        std::size_t capacity) noexcept {
    HelloResult result{};
    result.request.request_source_id = request.source_id;
    result.request.request_boot_id = request.boot_id;
    result.request.reply_to_sequence = request.sequence;
    if (success) {
        result.status = static_cast<std::uint8_t>(ResultStatus::Success);
        result.selected_version = effective_.selected_version;
        result.error_code = static_cast<std::uint16_t>(ResultError::None);
        result.max_logical_payload = effective_.max_logical_payload;
        result.max_inflight_reassemblies = effective_.max_inflight_reassemblies;
        result.max_subscriptions = effective_.max_subscriptions;
        result.max_dedup_entries = effective_.max_dedup_entries;
        result.session_timeout_ms = effective_.session_timeout_ms;
        std::memcpy(result.peer_uuid, local_.peer_uuid, 16U);
        result.config_revision = local_.config_revision;
    } else {
        result.status = static_cast<std::uint8_t>(ResultStatus::Unsupported);
        result.selected_version = 0U;
        result.error_code =
            static_cast<std::uint16_t>(ResultError::UnsupportedVersion);
        // limits, peer_uuid and config_revision stay zero.
    }
    std::size_t written = 0U;
    if (encode_hello_result(result, out, capacity, &written) !=
        MessageError::Ok) {
        return 0U;
    }
    return written;
}

std::size_t Session::build_session_close_result(const Header& request,
                                                bool parsed, std::uint8_t* out,
                                                std::size_t capacity) noexcept {
    ControlResult result{};
    result.request.request_source_id = request.source_id;
    result.request.request_boot_id = request.boot_id;
    result.request.reply_to_sequence = request.sequence;
    result.status = static_cast<std::uint8_t>(parsed ? ResultStatus::Success
                                                     : ResultStatus::Rejected);
    result.error_code = static_cast<std::uint16_t>(
        parsed ? ResultError::None : ResultError::MalformedPayload);
    std::size_t written = 0U;
    if (encode_session_close_result(result, out, capacity, &written) !=
        MessageError::Ok) {
        return 0U;
    }
    return written;
}

SessionOutcome Session::on_frame(const DecodedFrame& frame, std::uint64_t now_ms,
                                 std::uint8_t* reply_out,
                                 std::size_t reply_capacity) noexcept {
    // Sweep the deadline first, the same way btp::Receiver::submit sweeps
    // reassembly timeouts before it looks at the datagram. A frame that
    // arrives after the deadline is a TimedOut, not a renewal.
    const SessionOutcome expiry = check_expiry(now_ms);
    if (expiry.event == SessionEvent::TimedOut) {
        return expiry;
    }

    if (state_ == SessionState::Idle) {
        return SessionOutcome{SessionEvent::None, 0U};
    }

    if (state_ == SessionState::AwaitingHello) {
        const bool is_hello =
            frame.header.type == MessageType::Control &&
            frame.header.object_id == object_id::kHello;
        if (!is_hello) {
            // docs section 1: "no application message before a successful
            // HELLO_RESULT". Anything else is ignored and does NOT renew the
            // HELLO deadline.
            return SessionOutcome{SessionEvent::None, 0U};
        }

        Hello remote{};
        const bool decoded_ok =
            valid_ && decode_hello(frame.payload.data, frame.payload.size,
                                   &remote) == MessageError::Ok;
        EffectiveLimits eff{};
        if (decoded_ok) {
            eff = negotiate(local_, remote);
        }
        if (!decoded_ok || eff.selected_version == 0U) {
            // Malformed HELLO or no common version: fail closed, back to Idle
            // (docs section 2.3).
            const std::size_t n =
                build_hello_result(frame.header, false, reply_out, reply_capacity);
            state_ = SessionState::Idle;
            return SessionOutcome{SessionEvent::HelloRejected, n};
        }

        effective_ = eff;
        const std::size_t n =
            build_hello_result(frame.header, true, reply_out, reply_capacity);
        if (n == 0U) {
            // The SUCCESS reply will not fit the caller's buffer -- a sizing
            // bug. Undo and stay AwaitingHello rather than half-open a session
            // whose HELLO_RESULT never went out.
            effective_ = EffectiveLimits{};
            return SessionOutcome{SessionEvent::None, 0U};
        }
        peer_source_id_ = frame.header.source_id;
        peer_boot_id_ = frame.header.boot_id;
        state_ = SessionState::Active;
        deadline_ms_ = now_ms + effective_.session_timeout_ms;
        return SessionOutcome{SessionEvent::HelloAccepted, n};
    }

    // state_ == Active: every valid frame renews the watchdog, whatever its
    // object turns out to be (docs section 5: "a valid BTP frame").
    deadline_ms_ = now_ms + effective_.session_timeout_ms;

    if (frame.header.type == MessageType::Control &&
        frame.header.object_id == object_id::kSessionClose) {
        SessionClose close{};
        const bool parsed =
            decode_session_close(frame.payload.data, frame.payload.size,
                                 &close) == MessageError::Ok;
        const std::size_t n = build_session_close_result(
            frame.header, parsed, reply_out, reply_capacity);
        state_ = SessionState::Idle;
        return SessionOutcome{SessionEvent::SessionClosed, n};
    }

    // A stray HELLO mid-session, or any application object: the caller routes
    // it. The watchdog is already renewed.
    return SessionOutcome{SessionEvent::FrameAccepted, 0U};
}

// ---------------------------------------------------------------------------

const char* initiator_state_string(InitiatorState state) noexcept {
    switch (state) {
        case InitiatorState::Idle: return "Idle";
        case InitiatorState::AwaitingResult: return "AwaitingResult";
        case InitiatorState::Active: return "Active";
    }
    return "Unknown";
}

const char* initiator_event_string(InitiatorEvent event) noexcept {
    switch (event) {
        case InitiatorEvent::None: return "None";
        case InitiatorEvent::Connected: return "Connected";
        case InitiatorEvent::Rejected: return "Rejected";
        case InitiatorEvent::FrameAccepted: return "FrameAccepted";
        case InitiatorEvent::TimedOut: return "TimedOut";
        case InitiatorEvent::Disconnected: return "Disconnected";
    }
    return "Unknown";
}

SessionInitiator::SessionInitiator() noexcept
    : state_(InitiatorState::Idle),
      effective_(),
      hello_deadline_span_(0U),
      deadline_ms_(0U),
      own_source_id_(0U),
      own_boot_id_(0U),
      own_sequence_(0U),
      peer_source_id_(0U),
      peer_boot_id_(0U),
      peer_config_revision_(0U) {}

InitiatorOutcome SessionInitiator::check_expiry(std::uint64_t now_ms) noexcept {
    if (state_ == InitiatorState::AwaitingResult) {
        if (hello_deadline_span_ != 0U && now_ms >= deadline_ms_) {
            state_ = InitiatorState::Idle;
            return InitiatorOutcome{InitiatorEvent::TimedOut};
        }
    } else if (state_ == InitiatorState::Active) {
        // session_timeout_ms comes straight from the peer's HELLO_RESULT and
        // is spec-required non-zero; the guard is defensive.
        if (effective_.session_timeout_ms != 0U && now_ms >= deadline_ms_) {
            state_ = InitiatorState::Idle;
            return InitiatorOutcome{InitiatorEvent::TimedOut};
        }
    }
    return InitiatorOutcome{InitiatorEvent::None};
}

bool SessionInitiator::connect(const Hello& local, std::uint32_t own_source_id,
                               std::uint32_t own_boot_id,
                               std::uint32_t own_sequence, std::uint64_t now_ms,
                               std::uint64_t deadline_ms, std::uint8_t* out,
                               std::size_t out_capacity,
                               std::size_t* out_size) noexcept {
    if (state_ != InitiatorState::Idle || out_size == nullptr) {
        return false;
    }
    std::size_t written = 0U;
    if (encode_hello(local, out, out_capacity, &written) != MessageError::Ok) {
        return false;
    }
    own_source_id_ = own_source_id;
    own_boot_id_ = own_boot_id;
    own_sequence_ = own_sequence;
    hello_deadline_span_ = deadline_ms;
    deadline_ms_ = now_ms + deadline_ms;
    peer_source_id_ = 0U;
    peer_boot_id_ = 0U;
    effective_ = EffectiveLimits{};
    state_ = InitiatorState::AwaitingResult;
    *out_size = written;
    return true;
}

InitiatorOutcome SessionInitiator::on_frame(const DecodedFrame& frame,
                                            std::uint64_t now_ms) noexcept {
    const InitiatorOutcome expiry = check_expiry(now_ms);
    if (expiry.event == InitiatorEvent::TimedOut) {
        return expiry;
    }

    if (state_ == InitiatorState::Idle) {
        return InitiatorOutcome{InitiatorEvent::None};
    }

    if (state_ == InitiatorState::AwaitingResult) {
        const bool is_hello_result =
            frame.header.type == MessageType::Control &&
            frame.header.object_id == object_id::kHelloResult;
        if (!is_hello_result) {
            // docs section 1: "no application message before a successful
            // HELLO_RESULT". Anything else is ignored and does NOT renew the
            // deadline.
            return InitiatorOutcome{InitiatorEvent::None};
        }

        HelloResult result{};
        const bool decoded_ok =
            decode_hello_result(frame.payload.data, frame.payload.size,
                                &result) == MessageError::Ok;
        const bool correlates =
            decoded_ok &&
            result.request.request_source_id == own_source_id_ &&
            result.request.request_boot_id == own_boot_id_ &&
            result.request.reply_to_sequence == own_sequence_;
        if (!correlates) {
            // Malformed, or an answer to a different (stale) HELLO -- ignore
            // and keep waiting rather than tear down a fresh attempt.
            return InitiatorOutcome{InitiatorEvent::None};
        }

        if (result.status != static_cast<std::uint8_t>(ResultStatus::Success)) {
            state_ = InitiatorState::Idle;
            return InitiatorOutcome{InitiatorEvent::Rejected};
        }

        effective_.selected_version = result.selected_version;
        effective_.max_logical_payload = result.max_logical_payload;
        effective_.max_inflight_reassemblies = result.max_inflight_reassemblies;
        effective_.max_subscriptions = result.max_subscriptions;
        effective_.max_dedup_entries = result.max_dedup_entries;
        effective_.session_timeout_ms = result.session_timeout_ms;
        peer_source_id_ = frame.header.source_id;
        peer_boot_id_ = frame.header.boot_id;
        peer_config_revision_ = result.config_revision;
        state_ = InitiatorState::Active;
        deadline_ms_ = now_ms + effective_.session_timeout_ms;
        return InitiatorOutcome{InitiatorEvent::Connected};
    }

    // state_ == Active: every valid frame renews the watchdog, whatever its
    // object turns out to be (docs section 5: "a valid BTP frame").
    deadline_ms_ = now_ms + effective_.session_timeout_ms;
    return InitiatorOutcome{InitiatorEvent::FrameAccepted};
}

InitiatorOutcome SessionInitiator::poll(std::uint64_t now_ms) noexcept {
    return check_expiry(now_ms);
}

bool SessionInitiator::disconnect(std::uint64_t now_ms, std::uint8_t reason,
                                  std::uint32_t drain_timeout_ms,
                                  std::uint8_t* out, std::size_t out_capacity,
                                  std::size_t* out_size) noexcept {
    (void)now_ms;
    if (state_ == InitiatorState::Idle || out_size == nullptr) {
        return false;
    }
    SessionClose close{};
    close.reason = reason;
    close.drain_timeout_ms = drain_timeout_ms;
    std::size_t written = 0U;
    if (encode_session_close(close, out, out_capacity, &written) !=
        MessageError::Ok) {
        return false;
    }
    state_ = InitiatorState::Idle;
    *out_size = written;
    return true;
}

InitiatorOutcome SessionInitiator::reset() noexcept {
    if (state_ == InitiatorState::Idle) {
        return InitiatorOutcome{InitiatorEvent::None};
    }
    state_ = InitiatorState::Idle;
    return InitiatorOutcome{InitiatorEvent::Disconnected};
}

// ---------------------------------------------------------------------------
// CommandClient
// ---------------------------------------------------------------------------

const char* command_event_string(CommandEvent event) noexcept {
    switch (event) {
        case CommandEvent::None: return "None";
        case CommandEvent::Completed: return "Completed";
        case CommandEvent::TimedOut: return "TimedOut";
    }
    return "Unknown";
}

ClientCommand::ClientCommand() noexcept
    : pending_(false),
      local_id_(0U),
      own_source_id_(0U),
      own_boot_id_(0U),
      own_sequence_(0U),
      deadline_ms_(0U) {}

CommandClient::CommandClient(ClientCommand* slots, std::size_t slot_count) noexcept
    : slots_(slots), slot_count_(slot_count), next_local_id_(1U) {}

ClientCommand* CommandClient::find_pending(std::uint32_t source_id,
                                           std::uint32_t boot_id,
                                           std::uint32_t sequence) noexcept {
    for (std::size_t i = 0U; i < slot_count_; ++i) {
        ClientCommand& s = slots_[i];
        if (s.pending_ && s.own_source_id_ == source_id &&
            s.own_boot_id_ == boot_id && s.own_sequence_ == sequence) {
            return &s;
        }
    }
    return nullptr;
}

std::uint32_t CommandClient::command(
    std::uint32_t target_source_id, std::uint32_t target_boot_id,
    std::uint16_t action_id, std::uint16_t action_version,
    const std::uint8_t* parameters, std::size_t parameters_size,
    std::uint32_t own_source_id, std::uint32_t own_boot_id,
    std::uint32_t own_sequence, std::uint64_t now_ms, std::uint8_t* out,
    std::size_t out_capacity, std::size_t* out_size) noexcept {
    ClientCommand* slot = nullptr;
    for (std::size_t i = 0U; i < slot_count_; ++i) {
        if (!slots_[i].pending_) {
            slot = &slots_[i];
            break;
        }
    }
    if (slot == nullptr) return 0U;

    CommandRequest req = {};
    req.target_source_id = target_source_id;
    req.target_boot_id = target_boot_id;
    req.action_id = action_id;
    req.action_version = action_version;
    req.parameters = ByteView{parameters, parameters_size};
    std::size_t written = 0U;
    if (encode_command_request(req, out, out_capacity, &written) != MessageError::Ok) {
        return 0U;
    }
    if (out_size != nullptr) *out_size = written;

    slot->pending_ = true;
    slot->local_id_ = next_local_id_++;
    slot->own_source_id_ = own_source_id;
    slot->own_boot_id_ = own_boot_id;
    slot->own_sequence_ = own_sequence;
    slot->deadline_ms_ = now_ms + kCommandTimeoutMs;
    return slot->local_id_;
}

CommandOutcome CommandClient::on_result(const CommandResult& result) noexcept {
    ClientCommand* slot =
        find_pending(result.request.request_source_id, result.request.request_boot_id,
                    result.request.reply_to_sequence);
    if (slot == nullptr) {
        return CommandOutcome{CommandEvent::None, 0U, 0U, 0U, ByteView{}, ByteView{}};
    }
    const std::uint32_t local_id = slot->local_id_;
    *slot = ClientCommand();
    return CommandOutcome{CommandEvent::Completed, local_id,       result.status,
                          result.error_code,       result.message, result.result};
}

CommandOutcome CommandClient::expire(std::uint64_t now_ms) noexcept {
    for (std::size_t i = 0U; i < slot_count_; ++i) {
        ClientCommand& s = slots_[i];
        if (s.pending_ && now_ms >= s.deadline_ms_) {
            const std::uint32_t local_id = s.local_id_;
            s = ClientCommand();
            return CommandOutcome{CommandEvent::TimedOut, local_id, 0U, 0U, ByteView{},
                                  ByteView{}};
        }
    }
    return CommandOutcome{CommandEvent::None, 0U, 0U, 0U, ByteView{}, ByteView{}};
}

}  // namespace btp
