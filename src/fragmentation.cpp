#include "btp/fragmentation.hpp"

#include "detail.hpp"

#include <cstring>

namespace btp {
namespace {

// Every fragment carries FRAGMENTED, but a fragment of an encrypted message
// also carries ENCRYPTED and its CIPHER_ID sub-field: make_fragment() copies
// the logical flags and only ORs FRAGMENTED in. So this has to be a mask test.
// An exact `flags == kFlagFragmented` rejects every fragment of a wire v2
// message, making encrypted-and-fragmented unreceivable and defeating the AAD
// canonicalization of docs/encryption.md section 5, whose whole purpose is to
// let a fragmented encrypted message reassemble under one message-level tag.
bool valid_fragment_flags(std::uint16_t flags) noexcept {
    if ((flags & kFlagFragmented) == 0U) {
        return false;
    }
    if ((flags & static_cast<std::uint16_t>(~kKnownFlagsMask)) != 0U) {
        return false;
    }
    return detail::valid_cipher_id_for_flags(flags);
}

bool valid_fragment_header(const Header& header) noexcept {
    const std::uint8_t type = static_cast<std::uint8_t>(header.type);
    return type >= static_cast<std::uint8_t>(MessageType::Telemetry) &&
           type <= static_cast<std::uint8_t>(MessageType::Control) &&
           valid_fragment_flags(header.flags) && header.source_id != 0U &&
           header.boot_id != 0U && header.fragment_count >= 2U &&
           header.fragment_index < header.fragment_count;
}

bool same_identity(const Header& left, const Header& right) noexcept {
    return left.source_id == right.source_id &&
           left.boot_id == right.boot_id &&
           left.sequence == right.sequence;
}

bool same_message_invariants(const Header& left,
                             const Header& right) noexcept {
    return left.type == right.type && left.flags == right.flags &&
           left.source_id == right.source_id &&
           left.boot_id == right.boot_id &&
           left.sequence == right.sequence &&
           left.timestamp_us == right.timestamp_us &&
           left.object_id == right.object_id &&
           left.fragment_count == right.fragment_count;
}

}  // namespace

Error fragment_count(std::size_t logical_payload_size,
                     const TransportLimits& transport,
                     std::uint8_t* count_out) noexcept {
    if (!detail::valid_transport(transport) || count_out == nullptr) {
        return Error::InvalidArgument;
    }
    const std::size_t limit = transport.max_payload_size;
    std::size_t count = logical_payload_size / limit;
    if (logical_payload_size % limit != 0U) {
        ++count;
    }
    if (count == 0U) {
        count = 1U;
    }
    if (count > 255U) {
        return Error::PayloadTooLarge;
    }
    *count_out = static_cast<std::uint8_t>(count);
    return Error::Ok;
}

Error make_fragment(const Header& logical_header,
                    ByteView logical_payload,
                    const TransportLimits& transport,
                    std::uint8_t fragment_index,
                    Frame* fragment_out) noexcept {
    if (fragment_out == nullptr ||
        (logical_payload.data == nullptr && logical_payload.size != 0U)) {
        return Error::InvalidArgument;
    }
    const std::uint8_t type = static_cast<std::uint8_t>(logical_header.type);
    if (type < static_cast<std::uint8_t>(MessageType::Telemetry) ||
        type > static_cast<std::uint8_t>(MessageType::Control)) {
        return Error::InvalidType;
    }
    if ((logical_header.flags &
         static_cast<std::uint16_t>(~kKnownFlagsMask)) != 0U) {
        return Error::InvalidFlags;
    }
    if (logical_header.source_id == 0U) {
        return Error::InvalidSourceId;
    }
    if (logical_header.boot_id == 0U) {
        return Error::InvalidBootId;
    }

    std::uint8_t count = 0U;
    const Error count_result =
        fragment_count(logical_payload.size, transport, &count);
    if (count_result != Error::Ok) {
        return count_result;
    }
    if (fragment_index >= count) {
        return Error::InvalidFragmentation;
    }

    const std::size_t limit = transport.max_payload_size;
    const std::size_t offset = static_cast<std::size_t>(fragment_index) * limit;
    const std::size_t remaining = logical_payload.size - offset;
    const std::size_t payload_size = remaining < limit ? remaining : limit;

    Frame result;
    result.header = logical_header;
    result.header.flags = static_cast<std::uint16_t>(
        logical_header.flags & static_cast<std::uint16_t>(~kFlagFragmented));
    result.header.fragment_index = 0U;
    result.header.fragment_count = 1U;
    if (count > 1U) {
        result.header.flags |= kFlagFragmented;
        result.header.fragment_index = fragment_index;
        result.header.fragment_count = count;
    }
    result.payload.data = logical_payload.data == nullptr
                              ? nullptr
                              : logical_payload.data + offset;
    result.payload.size = payload_size;
    *fragment_out = result;
    return Error::Ok;
}

ReassemblySlot::ReassemblySlot() noexcept
    : active_(false),
      complete_(false),
      header_(),
      data_(nullptr),
      capacity_(0U),
      size_(0U),
      fragment_sizes_(),
      received_(),
      received_count_(0U),
      last_activity_ms_(0U) {}

Reassembler::Reassembler(ReassemblySlot* slots,
                         const ReassemblyStorage* storage,
                         std::size_t slot_count,
                         std::uint64_t timeout_ms) noexcept
    : slots_(slots),
      slot_count_(slot_count),
      timeout_ms_(timeout_ms),
      valid_(slots != nullptr && storage != nullptr && slot_count != 0U &&
             timeout_ms != 0U) {
    if (!valid_) {
        return;
    }
    for (std::size_t index = 0U; index < slot_count_; ++index) {
        if (storage[index].data == nullptr || storage[index].capacity == 0U) {
            valid_ = false;
        }
        slots_[index].data_ = storage[index].data;
        slots_[index].capacity_ = storage[index].capacity;
        reset_slot(index);
    }
}

bool Reassembler::valid() const noexcept {
    return valid_;
}

std::size_t Reassembler::slot_count() const noexcept {
    return slot_count_;
}

void Reassembler::reset_slot(std::size_t slot_index) noexcept {
    ReassemblySlot& slot = slots_[slot_index];
    slot.active_ = false;
    slot.complete_ = false;
    slot.header_ = Header();
    slot.size_ = 0U;
    std::memset(slot.fragment_sizes_, 0, sizeof(slot.fragment_sizes_));
    std::memset(slot.received_, 0, sizeof(slot.received_));
    slot.received_count_ = 0U;
    slot.last_activity_ms_ = 0U;
}

bool Reassembler::is_received(const ReassemblySlot& slot,
                              std::uint8_t index) const noexcept {
    return (slot.received_[index / 8U] &
            static_cast<std::uint8_t>(1U << (index % 8U))) != 0U;
}

void Reassembler::mark_received(ReassemblySlot* slot,
                                std::uint8_t index) noexcept {
    slot->received_[index / 8U] |=
        static_cast<std::uint8_t>(1U << (index % 8U));
}

std::size_t Reassembler::fragment_offset(const ReassemblySlot& slot,
                                         std::uint8_t index) const noexcept {
    std::size_t offset = 0U;
    for (std::size_t current = 0U; current < index; ++current) {
        if (is_received(slot, static_cast<std::uint8_t>(current))) {
            offset += slot.fragment_sizes_[current];
        }
    }
    return offset;
}

std::size_t Reassembler::expire(std::uint64_t now_ms) noexcept {
    if (!valid_) {
        return 0U;
    }
    std::size_t released = 0U;
    for (std::size_t index = 0U; index < slot_count_; ++index) {
        const ReassemblySlot& slot = slots_[index];
        if (slot.active_ && now_ms >= slot.last_activity_ms_ &&
            now_ms - slot.last_activity_ms_ >= timeout_ms_) {
            reset_slot(index);
            ++released;
        }
    }
    return released;
}

bool Reassembler::release(std::size_t slot_index) noexcept {
    if (!valid_ || slot_index >= slot_count_ || !slots_[slot_index].active_) {
        return false;
    }
    reset_slot(slot_index);
    return true;
}

void Reassembler::clear() noexcept {
    if (!valid_) {
        return;
    }
    for (std::size_t index = 0U; index < slot_count_; ++index) {
        reset_slot(index);
    }
}

ReassemblyEvent Reassembler::push(const Frame& fragment,
                                  std::uint64_t now_ms,
                                  ReassembledMessage* completed) noexcept {
    if (!valid_ || completed == nullptr ||
        (fragment.payload.data == nullptr && fragment.payload.size != 0U)) {
        return ReassemblyEvent::InvalidArgument;
    }
    if (!valid_fragment_header(fragment.header) ||
        fragment.payload.size > kSerialMaxPayloadSize) {
        return ReassemblyEvent::InvalidFragment;
    }

    expire(now_ms);

    std::size_t slot_index = slot_count_;
    for (std::size_t index = 0U; index < slot_count_; ++index) {
        if (slots_[index].active_ &&
            same_identity(slots_[index].header_, fragment.header)) {
            slot_index = index;
            break;
        }
    }

    if (slot_index != slot_count_ &&
        !same_message_invariants(slots_[slot_index].header_, fragment.header)) {
        if (!slots_[slot_index].complete_) {
            reset_slot(slot_index);
        }
        return ReassemblyEvent::Conflict;
    }

    if (slot_index == slot_count_) {
        bool inactive_slot = false;
        for (std::size_t index = 0U; index < slot_count_; ++index) {
            if (!slots_[index].active_) {
                inactive_slot = true;
                if (fragment.payload.size <= slots_[index].capacity_) {
                    slot_index = index;
                    break;
                }
            }
        }
        if (slot_index == slot_count_) {
            return inactive_slot ? ReassemblyEvent::MessageTooLarge
                                 : ReassemblyEvent::NoSlot;
        }
        ReassemblySlot& new_slot = slots_[slot_index];
        new_slot.active_ = true;
        new_slot.complete_ = false;
        new_slot.header_ = fragment.header;
        new_slot.size_ = 0U;
        new_slot.received_count_ = 0U;
        new_slot.last_activity_ms_ = now_ms;
        std::memset(new_slot.fragment_sizes_, 0,
                    sizeof(new_slot.fragment_sizes_));
        std::memset(new_slot.received_, 0, sizeof(new_slot.received_));
    }

    ReassemblySlot& slot = slots_[slot_index];
    const std::uint8_t index = fragment.header.fragment_index;
    const std::size_t offset = fragment_offset(slot, index);
    if (is_received(slot, index)) {
        const std::size_t existing_size = slot.fragment_sizes_[index];
        if (existing_size == fragment.payload.size &&
            (existing_size == 0U ||
             std::memcmp(slot.data_ + offset, fragment.payload.data,
                         existing_size) == 0)) {
            return ReassemblyEvent::Duplicate;
        }
        if (!slot.complete_) {
            reset_slot(slot_index);
        }
        return ReassemblyEvent::Conflict;
    }

    if (fragment.payload.size > slot.capacity_ - slot.size_) {
        reset_slot(slot_index);
        return ReassemblyEvent::MessageTooLarge;
    }

    std::memmove(slot.data_ + offset + fragment.payload.size,
                 slot.data_ + offset, slot.size_ - offset);
    if (fragment.payload.size != 0U) {
        std::memmove(slot.data_ + offset, fragment.payload.data,
                     fragment.payload.size);
    }
    slot.fragment_sizes_[index] =
        static_cast<std::uint16_t>(fragment.payload.size);
    mark_received(&slot, index);
    ++slot.received_count_;
    slot.size_ += fragment.payload.size;
    slot.last_activity_ms_ = now_ms;

    if (slot.received_count_ != slot.header_.fragment_count) {
        return ReassemblyEvent::Accepted;
    }

    slot.complete_ = true;
    ReassembledMessage result;
    result.header = slot.header_;
    result.header.flags = static_cast<std::uint16_t>(
        result.header.flags & static_cast<std::uint16_t>(~kFlagFragmented));
    result.header.fragment_index = 0U;
    result.header.fragment_count = 1U;
    result.payload.data = slot.data_;
    result.payload.size = slot.size_;
    result.slot_index = slot_index;
    *completed = result;
    return ReassemblyEvent::Complete;
}

ReassemblyEvent Reassembler::push(const DecodedFrame& fragment,
                                  std::uint64_t now_ms,
                                  ReassembledMessage* completed) noexcept {
    const Frame frame = {fragment.header, fragment.payload};
    return push(frame, now_ms, completed);
}

const char* reassembly_event_string(ReassemblyEvent event) noexcept {
    switch (event) {
        case ReassemblyEvent::Accepted: return "fragment accepted";
        case ReassemblyEvent::Complete: return "message complete";
        case ReassemblyEvent::Duplicate: return "duplicate fragment";
        case ReassemblyEvent::InvalidFragment: return "invalid fragment";
        case ReassemblyEvent::Conflict: return "conflicting fragment";
        case ReassemblyEvent::MessageTooLarge: return "message too large";
        case ReassemblyEvent::NoSlot: return "no reassembly slot available";
        case ReassemblyEvent::InvalidArgument: return "invalid argument";
    }
    return "unknown reassembly event";
}

}  // namespace btp
