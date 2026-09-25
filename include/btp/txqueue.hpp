#ifndef BTP_TXQUEUE_HPP
#define BTP_TXQUEUE_HPP

// A priority queue of encoded frames for the SEND side of a link -- the queue
// every integration otherwise writes by hand between btp::Node's send
// callback and the radio / UART / socket (docs/model.md section 8: "which
// queues, how many, how drained" is implementation-dependent; this is one
// implementation, written once).
//
// What it does, and nothing more:
//   * one FIFO per btp::PriorityClass (messages.hpp -- the spec's six
//     classes), all sharing ONE pool of caller-owned fixed-size slots;
//   * when the pool is full, an incoming frame evicts the OLDEST frame of the
//     LOWEST-priority class below its own (telemetry goes first, as the spec
//     asks); if nothing lower is queued, the incoming frame is the one
//     dropped. A session / command frame therefore never waits behind a
//     telemetry backlog, and never loses its slot to one;
//   * drains in Strict order (always the highest non-empty class) or Weighted
//     (each class gets a share per round -- kDefaultTxWeights -- so a steady
//     control stream cannot starve telemetry outright);
//   * front() / pop() are split, so a link that confirms delivery (ESP-NOW's
//     send callback) peeks, sends, and pops only once the frame is really out.
//
// Same guarantees as the rest of the library: no allocation, no exceptions,
// no I/O, no clock, no locking -- header-only, C++11. NOT thread-safe: a
// producer task and a radio task sharing one queue wrap every call in their
// own critical section (a FreeRTOS spinlock / std::mutex).
//
//   btp::StaticTxQueue<16, btp::kEspNowMaxFrameSize> radio_queue;
//
//   // NodeConfig::send() -- enqueue instead of transmitting inline:
//   bool send(const std::uint8_t* frame, std::size_t n) override {
//       return radio_queue.push(frame, n) != btp::TxPush::Dropped; // (lock around it)
//   }
//
//   // the radio task:
//   radio_queue.drain(&esp_now_send_thunk, &peer, /*max_frames=*/4);

#include "btp/codec.hpp"     // decode, DecodedFrame, TransportLimits
#include "btp/messages.hpp"  // PriorityClass, priority_class
#include "btp/endpoint.hpp"  // EndpointSendFn

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace btp {

static const std::size_t kTxClassCount = 6U;

// PriorityClass (1..6) <-> array index (0..5).
inline std::size_t tx_class_index(PriorityClass priority) noexcept {
    const std::size_t value = static_cast<std::size_t>(priority);
    return (value >= 1U && value <= kTxClassCount) ? value - 1U : kTxClassCount - 1U;
}

// The priority class of an already-encoded frame, read from its header (the
// header is cleartext even on a sealed frame). False for octets that do not
// decode as a BTP frame. STATUS is classed as periodic (Bulk): the DEGRADED
// bit lives in the payload, which a sealed frame hides -- push() with an
// explicit class when that distinction matters.
inline bool frame_priority(const std::uint8_t* frame, std::size_t size,
                           PriorityClass* out) noexcept {
    if (frame == nullptr || out == nullptr) return false;
    DecodedFrame decoded;
    const TransportLimits any_size = {size, true};
    if (decode(frame, size, any_size, &decoded) != Error::Ok) return false;
    *out = priority_class(decoded.header.type, decoded.header.object_id);
    return true;
}

enum class TxDrainPolicy : std::uint8_t {
    Strict,    // always the highest-priority non-empty class
    Weighted,  // per-round shares (weights), highest first within a round
};

// Frames each class may send per Weighted round, Session first. A class with
// nothing queued gives its share away; an empty round starts the next.
static const std::uint8_t kDefaultTxWeights[kTxClassCount] = {8U, 4U, 4U, 2U, 2U, 1U};

enum class TxPush : std::uint8_t {
    Queued,          // stored, nothing lost
    QueuedEvicting,  // stored by evicting an older lower-priority frame
    Dropped,         // pool full of equal/higher priority -- THIS frame was dropped
    TooLarge,        // larger than one slot
    Invalid,         // null / empty, or (auto-classified push) not a BTP frame
};

struct TxQueueStats {
    std::uint32_t queued[kTxClassCount];   // accepted into the queue
    std::uint32_t sent[kTxClassCount];     // left via pop() / drain()
    std::uint32_t dropped[kTxClassCount];  // refused on push, or evicted later
};

class TxQueue {
public:
    // `slot_data` holds `slot_count` slots of `slot_bytes` octets each (one
    // encoded frame per slot); `slot_sizes` / `slot_next` are `slot_count`
    // entries of bookkeeping. All caller-owned, all outliving the queue.
    // slot_count must stay below 0xFFFF.
    TxQueue(std::uint8_t* slot_data, std::size_t slot_bytes, std::uint16_t* slot_sizes,
            std::uint16_t* slot_next, std::size_t slot_count,
            TxDrainPolicy policy = TxDrainPolicy::Strict) noexcept
        : data_(slot_data),
          slot_bytes_(slot_bytes),
          sizes_(slot_sizes),
          next_(slot_next),
          capacity_(slot_count < kNone ? slot_count : 0U),
          policy_(policy),
          selected_(kNoClass) {
        for (std::size_t i = 0; i < kTxClassCount; ++i) weights_[i] = kDefaultTxWeights[i];
        clear();
        std::memset(&stats_, 0, sizeof(stats_));
    }

    TxQueue(const TxQueue&) = delete;
    TxQueue& operator=(const TxQueue&) = delete;

    bool valid() const noexcept {
        return data_ != nullptr && sizes_ != nullptr && next_ != nullptr && capacity_ != 0U &&
               slot_bytes_ != 0U;
    }

    // Drops everything queued (stats are kept).
    void clear() noexcept {
        for (std::size_t c = 0; c < kTxClassCount; ++c) {
            head_[c] = kNone;
            tail_[c] = kNone;
            count_[c] = 0U;
            credit_[c] = weights_[c];
        }
        free_ = kNone;
        for (std::size_t i = capacity_; i > 0U; --i) {
            next_[i - 1U] = free_;
            free_ = static_cast<std::uint16_t>(i - 1U);
        }
        size_ = 0U;
        selected_ = kNoClass;
    }

    void set_policy(TxDrainPolicy policy) noexcept {
        policy_ = policy;
        selected_ = kNoClass;
    }

    // Weighted shares, Session first; 0 is treated as 1 (every class keeps
    // some share, or telemetry could starve for good).
    void set_weights(const std::uint8_t weights[kTxClassCount]) noexcept {
        for (std::size_t c = 0; c < kTxClassCount; ++c) {
            weights_[c] = weights[c] == 0U ? 1U : weights[c];
            credit_[c] = weights_[c];
        }
    }

    // Classifies `frame` from its own header (frame_priority()) and queues it.
    TxPush push(const std::uint8_t* frame, std::size_t size) noexcept {
        PriorityClass priority;
        if (!frame_priority(frame, size, &priority)) return TxPush::Invalid;
        return push(frame, size, priority);
    }

    // Queues `frame` under an explicit class (copies it into a slot).
    TxPush push(const std::uint8_t* frame, std::size_t size, PriorityClass priority) noexcept {
        if (!valid() || frame == nullptr || size == 0U) return TxPush::Invalid;
        const std::size_t cls = tx_class_index(priority);
        if (size > slot_bytes_ || size > 0xFFFFU) {
            ++stats_.dropped[cls];
            return TxPush::TooLarge;
        }

        TxPush result = TxPush::Queued;
        if (free_ == kNone) {
            // Evict the oldest frame of the lowest class strictly below this
            // one -- never the frame front() already handed out.
            std::size_t victim = kNoClass;
            for (std::size_t c = kTxClassCount; c > cls + 1U; --c) {
                const std::size_t candidate = c - 1U;
                if (count_[candidate] > (candidate == selected_ ? 1U : 0U)) {
                    victim = candidate;
                    break;
                }
            }
            if (victim == kNoClass) {
                ++stats_.dropped[cls];
                return TxPush::Dropped;
            }
            evict_oldest(victim);
            result = TxPush::QueuedEvicting;
        }

        const std::uint16_t slot = free_;
        free_ = next_[slot];
        std::memcpy(data_ + static_cast<std::size_t>(slot) * slot_bytes_, frame, size);
        sizes_[slot] = static_cast<std::uint16_t>(size);
        next_[slot] = kNone;
        if (tail_[cls] == kNone) {
            head_[cls] = slot;
        } else {
            next_[tail_[cls]] = slot;
        }
        tail_[cls] = slot;
        ++count_[cls];
        ++size_;
        ++stats_.queued[cls];
        return result;
    }

    // The frame the drain policy sends next, without removing it; the same
    // one until pop() (so a delivery-confirmed link can retry it). False when
    // empty.
    bool front(const std::uint8_t** frame, std::size_t* size,
               PriorityClass* priority = nullptr) noexcept {
        if (frame == nullptr || size == nullptr || size_ == 0U) return false;
        if (selected_ == kNoClass) selected_ = choose();
        const std::uint16_t slot = head_[selected_];
        *frame = data_ + static_cast<std::size_t>(slot) * slot_bytes_;
        *size = sizes_[slot];
        if (priority != nullptr) *priority = static_cast<PriorityClass>(selected_ + 1U);
        return true;
    }

    // Removes the frame front() returned (counted as sent). No-op when empty.
    void pop() noexcept {
        if (size_ == 0U) return;
        if (selected_ == kNoClass) selected_ = choose();
        const std::size_t cls = selected_;
        release_head(cls);
        ++stats_.sent[cls];
        if (policy_ == TxDrainPolicy::Weighted && credit_[cls] != 0U) --credit_[cls];
        selected_ = kNoClass;
    }

    // front() -> send -> pop(), up to `max_frames` frames (0 = until empty).
    // Stops at the first `send` that returns false and leaves that frame at
    // the front for the next call. Returns how many frames went out.
    std::size_t drain(EndpointSendFn send, void* send_context, std::size_t max_frames = 0U) noexcept {
        if (send == nullptr) return 0U;
        std::size_t sent = 0U;
        const std::uint8_t* frame = nullptr;
        std::size_t size = 0U;
        while ((max_frames == 0U || sent < max_frames) && front(&frame, &size)) {
            if (!send(send_context, frame, size)) break;
            pop();
            ++sent;
        }
        return sent;
    }

    std::size_t size() const noexcept { return size_; }
    std::size_t size(PriorityClass priority) const noexcept {
        return count_[tx_class_index(priority)];
    }
    bool empty() const noexcept { return size_ == 0U; }
    std::size_t capacity() const noexcept { return capacity_; }
    const TxQueueStats& stats() const noexcept { return stats_; }

private:
    static const std::uint16_t kNone = 0xFFFFU;
    static const std::size_t kNoClass = kTxClassCount;

    std::size_t choose() noexcept {
        std::size_t highest = kNoClass;
        for (std::size_t c = 0; c < kTxClassCount; ++c) {
            if (count_[c] != 0U) {
                highest = c;
                break;
            }
        }
        if (policy_ == TxDrainPolicy::Strict || highest == kNoClass) return highest;
        for (int round = 0; round < 2; ++round) {
            for (std::size_t c = 0; c < kTxClassCount; ++c) {
                if (count_[c] != 0U && credit_[c] != 0U) return c;
            }
            for (std::size_t c = 0; c < kTxClassCount; ++c) credit_[c] = weights_[c];
        }
        return highest;
    }

    void release_head(std::size_t cls) noexcept {
        const std::uint16_t slot = head_[cls];
        head_[cls] = next_[slot];
        if (head_[cls] == kNone) tail_[cls] = kNone;
        next_[slot] = free_;
        free_ = slot;
        --count_[cls];
        --size_;
    }

    // The oldest frame of `cls` -- skipping the head when front() already
    // handed it out, so a frame mid-transmission is never pulled from under
    // the link.
    void evict_oldest(std::size_t cls) noexcept {
        ++stats_.dropped[cls];
        if (cls != selected_) {
            release_head(cls);
            return;
        }
        const std::uint16_t keep = head_[cls];
        const std::uint16_t slot = next_[keep];
        next_[keep] = next_[slot];
        if (tail_[cls] == slot) tail_[cls] = keep;
        next_[slot] = free_;
        free_ = slot;
        --count_[cls];
        --size_;
    }

    std::uint8_t* data_;
    std::size_t slot_bytes_;
    std::uint16_t* sizes_;
    std::uint16_t* next_;
    std::size_t capacity_;
    TxDrainPolicy policy_;
    std::size_t selected_;  // class front() picked, until pop()

    std::uint16_t head_[kTxClassCount];
    std::uint16_t tail_[kTxClassCount];
    std::size_t count_[kTxClassCount];
    std::uint8_t weights_[kTxClassCount];
    std::uint8_t credit_[kTxClassCount];
    std::uint16_t free_;
    std::size_t size_;
    TxQueueStats stats_;
};

namespace detail {

template <std::size_t Slots, std::size_t SlotBytes>
struct TxQueueStorage {
    std::uint8_t data[Slots * SlotBytes];
    std::uint16_t sizes[Slots];
    std::uint16_t next[Slots];
};

}  // namespace detail

// TxQueue that owns its storage: Slots frames of up to SlotBytes octets
// (kEspNowMaxFrameSize for a radio; one Node frame never exceeds it -- see
// Endpoint::send_logical()).
template <std::size_t Slots, std::size_t SlotBytes = kEspNowMaxFrameSize>
class StaticTxQueue : private detail::TxQueueStorage<Slots, SlotBytes>, public TxQueue {
    typedef detail::TxQueueStorage<Slots, SlotBytes> Storage;

public:
    explicit StaticTxQueue(TxDrainPolicy policy = TxDrainPolicy::Strict) noexcept
        : Storage(),
          TxQueue(Storage::data, SlotBytes, Storage::sizes, Storage::next, Slots, policy) {}
};

}  // namespace btp

#endif  // BTP_TXQUEUE_HPP
