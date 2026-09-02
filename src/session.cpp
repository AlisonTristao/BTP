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

}  // namespace btp
