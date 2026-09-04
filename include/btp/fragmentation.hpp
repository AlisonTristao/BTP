#ifndef BTP_FRAGMENTATION_HPP
#define BTP_FRAGMENTATION_HPP

#include "btp/codec.hpp"

#include <cstddef>
#include <cstdint>

namespace btp {

// Computes the number of frames needed for a logical payload. Messages are
// limited by the one-byte fragment_count field (255 fragments).
Error fragment_count(std::size_t logical_payload_size,
                     const TransportLimits& transport,
                     std::uint8_t* count_out) noexcept;

// Creates one zero-copy view over logical_payload. Fragmentation fields are
// normalized; all other header fields are preserved.
Error make_fragment(const Header& logical_header,
                    ByteView logical_payload,
                    const TransportLimits& transport,
                    std::uint8_t fragment_index,
                    Frame* fragment_out) noexcept;

enum class ReassemblyEvent : std::uint8_t {
    Accepted,
    Complete,
    Duplicate,
    InvalidFragment,
    Conflict,
    MessageTooLarge,
    NoSlot,
    InvalidArgument
};

struct ReassembledMessage {
    Header header;
    ByteView payload;
    std::size_t slot_index;
};

struct ReassemblyStorage {
    std::uint8_t* data;
    std::size_t capacity;
};

// Metadata for one bounded reassembly. Treat this type as opaque; storage is
// bound by Reassembler's constructor.
class ReassemblySlot {
public:
    ReassemblySlot() noexcept;

private:
    friend class Reassembler;

    bool active_;
    bool complete_;
    Header header_;
    std::uint8_t* data_;
    std::size_t capacity_;
    std::size_t size_;
    std::uint16_t fragment_sizes_[255];
    std::uint8_t received_[32];
    std::uint16_t received_count_;
    std::uint64_t last_activity_ms_;
};

// Allocation-free, out-of-order reassembly. A completed slot stays owned by
// the consumer until release() or timeout, so its returned ByteView is stable.
class Reassembler {
public:
    // Named slot_array, not slots: Qt's <QObject> (unless QT_NO_KEYWORDS)
    // #defines slots to nothing, and this header is included from Qt-using
    // consumers (TraceView) -- a parameter or member actually named `slots`
    // would silently vanish there. Every BTP header follows this naming for
    // the same reason; see docs/library.md's own note on it.
    Reassembler(ReassemblySlot* slot_array,
                const ReassemblyStorage* storage,
                std::size_t slot_count,
                std::uint64_t timeout_ms) noexcept;

    bool valid() const noexcept;
    std::size_t slot_count() const noexcept;

    ReassemblyEvent push(const Frame& fragment,
                         std::uint64_t now_ms,
                         ReassembledMessage* completed) noexcept;

    ReassemblyEvent push(const DecodedFrame& fragment,
                         std::uint64_t now_ms,
                         ReassembledMessage* completed) noexcept;

    bool release(std::size_t slot_index) noexcept;
    std::size_t expire(std::uint64_t now_ms) noexcept;
    void clear() noexcept;

private:
    void reset_slot(std::size_t slot_index) noexcept;
    bool is_received(const ReassemblySlot& slot,
                     std::uint8_t fragment_index) const noexcept;
    void mark_received(ReassemblySlot* slot,
                       std::uint8_t fragment_index) noexcept;
    std::size_t fragment_offset(const ReassemblySlot& slot,
                                std::uint8_t fragment_index) const noexcept;

    ReassemblySlot* slots_;
    std::size_t slot_count_;
    std::uint64_t timeout_ms_;
    bool valid_;
};

const char* reassembly_event_string(ReassemblyEvent event) noexcept;

}  // namespace btp

#endif  // BTP_FRAGMENTATION_HPP
