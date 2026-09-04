#include "btp/subscription.hpp"

namespace btp {

namespace {

// 1e6 / rate_millihz -- the publish period in milliseconds a rate expressed
// in thousandths of a Hertz implies. rate_millihz is spec-required non-zero
// (docs/commands.md 4.1); the zero guard is defensive, same reasoning as
// btp::Session's session_timeout_ms checks.
std::uint64_t period_ms(std::uint32_t rate_millihz) noexcept {
    return rate_millihz != 0U ? (1000000ULL / static_cast<std::uint64_t>(rate_millihz))
                              : ~static_cast<std::uint64_t>(0U);
}

}  // namespace

// ===========================================================================
// SubscriptionTable -- the RESPONDER
// ===========================================================================

SubscriptionRecord::SubscriptionRecord() noexcept
    : used_(false),
      subscription_id_(0U),
      requester_source_id_(0U),
      requester_boot_id_(0U),
      topic_id_(0U),
      effective_rate_millihz_(0U),
      expires_at_ms_(0U),
      due_at_ms_(0U) {}

SubscriptionTable::SubscriptionTable(SubscriptionRecord* slots,
                                     std::size_t slot_count) noexcept
    : slots_(slots), slot_count_(slot_count), next_id_(1U) {}

SubscriptionRecord* SubscriptionTable::find(std::uint32_t source_id,
                                            std::uint32_t boot_id,
                                            std::uint16_t topic_id) noexcept {
    for (std::size_t i = 0U; i < slot_count_; ++i) {
        SubscriptionRecord& s = slots_[i];
        if (s.used_ && s.requester_source_id_ == source_id &&
            s.requester_boot_id_ == boot_id && s.topic_id_ == topic_id) {
            return &s;
        }
    }
    return nullptr;
}

SubscriptionRecord* SubscriptionTable::find_by_id(
    std::uint32_t subscription_id) noexcept {
    for (std::size_t i = 0U; i < slot_count_; ++i) {
        if (slots_[i].used_ && slots_[i].subscription_id_ == subscription_id) {
            return &slots_[i];
        }
    }
    return nullptr;
}

SubscriptionRecord* SubscriptionTable::allocate() noexcept {
    for (std::size_t i = 0U; i < slot_count_; ++i) {
        if (!slots_[i].used_) return &slots_[i];
    }
    return nullptr;
}

void SubscriptionTable::evict_other_boots(std::uint32_t source_id,
                                          std::uint32_t boot_id) noexcept {
    for (std::size_t i = 0U; i < slot_count_; ++i) {
        SubscriptionRecord& s = slots_[i];
        if (s.used_ && s.requester_source_id_ == source_id &&
            s.requester_boot_id_ != boot_id) {
            s = SubscriptionRecord();
        }
    }
}

void SubscriptionTable::handle_subscribe(const Catalog& catalog,
                                         const Header& header,
                                         const Subscribe& request,
                                         std::uint64_t now_ms,
                                         SubscribeResult* result_out) noexcept {
    if (result_out == nullptr) return;
    *result_out = SubscribeResult{};
    result_out->request.request_source_id = header.source_id;
    result_out->request.request_boot_id = header.boot_id;
    result_out->request.reply_to_sequence = header.sequence;

    // A rebooted peer has no session left to keep publishing for -- drop
    // every OTHER boot_id's subscription of this source_id before granting
    // or renewing this one, independent of this request's own outcome.
    evict_other_boots(header.source_id, header.boot_id);

    const CatalogTopic* topic = catalog.topic(request.topic_id);
    if (topic == nullptr || (topic->flags & kTopicSubscribable) == 0U) {
        result_out->status = static_cast<std::uint8_t>(ResultStatus::Rejected);
        result_out->error_code = static_cast<std::uint16_t>(ResultError::NotFound);
        return;
    }

    // A periodic topic is capped by its own max_rate_millihz; a non-periodic
    // one (max == 0) by the local default_rate_millihz, if any -- either way
    // never above what the client asked for (commands.md section 4's MUST
    // NOT). Resolved BEFORE touching the slot table: a request landing under
    // min_rate_millihz -- when set -- is rejected outright rather than
    // granted at a rate slower than the client can use, the same "reject,
    // don't clamp up" rule cap application already follows in the other
    // direction.
    const std::uint32_t cap = (topic->max_rate_millihz != 0U)
                                  ? topic->max_rate_millihz
                                  : topic->default_rate_millihz;
    std::uint32_t effective_rate = request.requested_rate_millihz;
    if (cap != 0U && effective_rate > cap) effective_rate = cap;
    if (topic->min_rate_millihz != 0U && effective_rate < topic->min_rate_millihz) {
        result_out->status = static_cast<std::uint8_t>(ResultStatus::Rejected);
        result_out->error_code =
            static_cast<std::uint16_t>(ResultError::InvalidArgument);
        return;
    }

    // A second SUBSCRIBE from the same requester for the same topic is a
    // renewal (docs/commands.md 4.3) -- reuse the slot and its subscription_id
    // rather than allocating a second one.
    SubscriptionRecord* slot = find(header.source_id, header.boot_id, request.topic_id);
    if (slot == nullptr) {
        slot = allocate();
        if (slot == nullptr) {
            result_out->status = static_cast<std::uint8_t>(ResultStatus::Busy);
            result_out->error_code =
                static_cast<std::uint16_t>(ResultError::CapacityExhausted);
            return;
        }
        slot->subscription_id_ = next_id_++;
        slot->due_at_ms_ = now_ms;  // due right away; a renewal leaves this alone
    }

    slot->used_ = true;
    slot->requester_source_id_ = header.source_id;
    slot->requester_boot_id_ = header.boot_id;
    slot->topic_id_ = request.topic_id;
    slot->effective_rate_millihz_ = effective_rate;
    slot->expires_at_ms_ = now_ms + request.requested_lease_ms;

    result_out->status = static_cast<std::uint8_t>(ResultStatus::Success);
    result_out->subscription_id = slot->subscription_id_;
    result_out->effective_rate_millihz = effective_rate;
    result_out->granted_lease_ms = request.requested_lease_ms;
}

void SubscriptionTable::handle_unsubscribe(const Header& header,
                                           const Unsubscribe& request,
                                           ControlResult* result_out) noexcept {
    if (result_out == nullptr) return;
    *result_out = ControlResult{};
    result_out->request.request_source_id = header.source_id;
    result_out->request.request_boot_id = header.boot_id;
    result_out->request.reply_to_sequence = header.sequence;
    // docs/commands.md 4.4: removing an already-absent subscription (or one
    // that never belonged to this requester) is still success.
    result_out->status = static_cast<std::uint8_t>(ResultStatus::Success);

    SubscriptionRecord* slot = find_by_id(request.subscription_id);
    if (slot != nullptr && slot->requester_source_id_ == header.source_id &&
        slot->requester_boot_id_ == header.boot_id) {
        *slot = SubscriptionRecord();
    }
}

void SubscriptionTable::expire(std::uint64_t now_ms) noexcept {
    for (std::size_t i = 0U; i < slot_count_; ++i) {
        if (slots_[i].used_ && now_ms >= slots_[i].expires_at_ms_) {
            slots_[i] = SubscriptionRecord();
        }
    }
}

bool SubscriptionTable::due(std::uint16_t topic_id, std::uint64_t now_ms) const noexcept {
    for (std::size_t i = 0U; i < slot_count_; ++i) {
        const SubscriptionRecord& s = slots_[i];
        if (s.used_ && s.topic_id_ == topic_id && now_ms >= s.due_at_ms_) return true;
    }
    return false;
}

void SubscriptionTable::note_published(std::uint16_t topic_id,
                                       std::uint64_t now_ms) noexcept {
    for (std::size_t i = 0U; i < slot_count_; ++i) {
        SubscriptionRecord& s = slots_[i];
        if (s.used_ && s.topic_id_ == topic_id) {
            s.due_at_ms_ = now_ms + period_ms(s.effective_rate_millihz_);
        }
    }
}

std::size_t SubscriptionTable::subscriber_count(std::uint16_t topic_id) const noexcept {
    std::size_t count = 0U;
    for (std::size_t i = 0U; i < slot_count_; ++i) {
        if (slots_[i].used_ && slots_[i].topic_id_ == topic_id) ++count;
    }
    return count;
}

std::uint32_t SubscriptionTable::aggregate_rate_millihz(
    std::uint16_t topic_id) const noexcept {
    std::uint32_t fastest = 0U;
    for (std::size_t i = 0U; i < slot_count_; ++i) {
        const SubscriptionRecord& s = slots_[i];
        if (s.used_ && s.topic_id_ == topic_id &&
            s.effective_rate_millihz_ > fastest) {
            fastest = s.effective_rate_millihz_;
        }
    }
    return fastest;
}

// ===========================================================================
// SubscriptionClient -- the INITIATOR
// ===========================================================================

const char* subscription_event_string(SubscriptionEvent event) noexcept {
    switch (event) {
        case SubscriptionEvent::None: return "None";
        case SubscriptionEvent::Granted: return "Granted";
        case SubscriptionEvent::Rejected: return "Rejected";
        case SubscriptionEvent::Expired: return "Expired";
    }
    return "Unknown";
}

ClientSubscription::ClientSubscription() noexcept
    : state_(State::Idle),
      local_id_(0U),
      peer_source_id_(0U),
      peer_boot_id_(0U),
      topic_id_(0U),
      requested_rate_millihz_(0U),
      requested_lease_ms_(0U),
      own_source_id_(0U),
      own_boot_id_(0U),
      own_sequence_(0U),
      subscription_id_(0U),
      effective_rate_millihz_(0U),
      expires_at_ms_(0U),
      renew_at_ms_(0U),
      pending_deadline_ms_(0U) {}

SubscriptionClient::SubscriptionClient(ClientSubscription* slots,
                                       std::size_t slot_count) noexcept
    : slots_(slots), slot_count_(slot_count), next_local_id_(1U) {}

ClientSubscription* SubscriptionClient::find_by_local_id(std::uint32_t local_id) noexcept {
    if (local_id == 0U) return nullptr;
    for (std::size_t i = 0U; i < slot_count_; ++i) {
        if (slots_[i].state_ != ClientSubscription::State::Idle &&
            slots_[i].local_id_ == local_id) {
            return &slots_[i];
        }
    }
    return nullptr;
}

ClientSubscription* SubscriptionClient::find_pending(std::uint32_t source_id,
                                                     std::uint32_t boot_id,
                                                     std::uint32_t sequence) noexcept {
    for (std::size_t i = 0U; i < slot_count_; ++i) {
        ClientSubscription& s = slots_[i];
        if (s.state_ == ClientSubscription::State::Pending &&
            s.own_source_id_ == source_id && s.own_boot_id_ == boot_id &&
            s.own_sequence_ == sequence) {
            return &s;
        }
    }
    return nullptr;
}

bool SubscriptionClient::build_subscribe(const ClientSubscription& slot,
                                         std::uint8_t* out, std::size_t out_capacity,
                                         std::size_t* out_size) const noexcept {
    Subscribe req = {};
    req.target_source_id = slot.peer_source_id_;
    req.target_boot_id = slot.peer_boot_id_;
    req.topic_id = slot.topic_id_;
    req.requested_rate_millihz = slot.requested_rate_millihz_;
    req.requested_lease_ms = slot.requested_lease_ms_;
    std::size_t written = 0U;
    if (encode_subscribe(req, out, out_capacity, &written) != MessageError::Ok) {
        return false;
    }
    if (out_size != nullptr) *out_size = written;
    return true;
}

std::uint32_t SubscriptionClient::subscribe(
    std::uint32_t peer_source_id, std::uint32_t peer_boot_id, std::uint16_t topic_id,
    std::uint32_t rate_millihz, std::uint32_t lease_ms, std::uint32_t own_source_id,
    std::uint32_t own_boot_id, std::uint32_t own_sequence, std::uint64_t now_ms,
    std::uint8_t* out, std::size_t out_capacity, std::size_t* out_size) noexcept {
    ClientSubscription* slot = nullptr;
    for (std::size_t i = 0U; i < slot_count_; ++i) {
        if (slots_[i].state_ == ClientSubscription::State::Idle) {
            slot = &slots_[i];
            break;
        }
    }
    if (slot == nullptr) return 0U;

    // Build against a throwaway copy first -- a bad request leaves the real
    // slot untouched (no partial output on failure, the library's own rule).
    ClientSubscription candidate;
    candidate.peer_source_id_ = peer_source_id;
    candidate.peer_boot_id_ = peer_boot_id;
    candidate.topic_id_ = topic_id;
    candidate.requested_rate_millihz_ = rate_millihz;
    candidate.requested_lease_ms_ = lease_ms;
    if (!build_subscribe(candidate, out, out_capacity, out_size)) return 0U;

    *slot = candidate;
    slot->state_ = ClientSubscription::State::Pending;
    slot->local_id_ = next_local_id_++;
    slot->own_source_id_ = own_source_id;
    slot->own_boot_id_ = own_boot_id;
    slot->own_sequence_ = own_sequence;
    slot->pending_deadline_ms_ = now_ms + kSubscriptionPendingTimeoutMs;
    return slot->local_id_;
}

bool SubscriptionClient::unsubscribe(std::uint32_t local_id,
                                     std::uint32_t /*own_source_id*/,
                                     std::uint32_t /*own_boot_id*/,
                                     std::uint32_t /*own_sequence*/, std::uint8_t* out,
                                     std::size_t out_capacity,
                                     std::size_t* out_size) noexcept {
    // Fire-and-forget (no UNSUBSCRIBE_RESULT correlation kept), same as
    // btp::SessionInitiator::disconnect() -- the identity triple is accepted
    // for a symmetric signature with subscribe() / renew() but unused.
    ClientSubscription* slot = find_by_local_id(local_id);
    if (slot == nullptr || slot->state_ != ClientSubscription::State::Active) {
        return false;  // unknown, idle, or still Pending (no subscription_id yet)
    }
    Unsubscribe req = {};
    req.target_source_id = slot->peer_source_id_;
    req.target_boot_id = slot->peer_boot_id_;
    req.subscription_id = slot->subscription_id_;
    std::size_t written = 0U;
    if (encode_unsubscribe(req, out, out_capacity, &written) != MessageError::Ok) {
        return false;
    }
    if (out_size != nullptr) *out_size = written;
    *slot = ClientSubscription();
    return true;
}

SubscriptionOutcome SubscriptionClient::on_result(const SubscribeResult& result,
                                                   std::uint64_t now_ms) noexcept {
    ClientSubscription* slot =
        find_pending(result.request.request_source_id, result.request.request_boot_id,
                    result.request.reply_to_sequence);
    if (slot == nullptr) {
        return SubscriptionOutcome{SubscriptionEvent::None, 0U, 0U, 0U,
                                   0U,  0U, 0U, 0U, 0U};
    }

    // Captured before a Rejected outcome resets the slot -- the caller's own
    // UI message ("rejected topic 0x.. of source 0x..") needs to name what
    // was asked for, not just that something failed.
    SubscriptionOutcome outcome{};
    outcome.local_id = slot->local_id_;
    outcome.peer_source_id = slot->peer_source_id_;
    outcome.peer_boot_id = slot->peer_boot_id_;
    outcome.topic_id = slot->topic_id_;
    outcome.requested_rate_millihz = slot->requested_rate_millihz_;
    outcome.status = result.status;
    outcome.error_code = result.error_code;

    if (result.status != static_cast<std::uint8_t>(ResultStatus::Success)) {
        *slot = ClientSubscription();
        outcome.event = SubscriptionEvent::Rejected;
        return outcome;
    }

    slot->state_ = ClientSubscription::State::Active;
    slot->subscription_id_ = result.subscription_id;
    slot->effective_rate_millihz_ = result.effective_rate_millihz;
    slot->expires_at_ms_ = now_ms + result.granted_lease_ms;
    // docs/commands.md 4.3: renewal is just another SUBSCRIBE. Renew with
    // 20% of the granted lease left.
    const std::uint64_t margin_ms =
        static_cast<std::uint64_t>(result.granted_lease_ms) / 5U;
    slot->renew_at_ms_ =
        slot->expires_at_ms_ > margin_ms ? slot->expires_at_ms_ - margin_ms : 0U;
    outcome.event = SubscriptionEvent::Granted;
    outcome.effective_rate_millihz = result.effective_rate_millihz;
    return outcome;
}

void SubscriptionClient::expire(std::uint64_t now_ms) noexcept {
    for (std::size_t i = 0U; i < slot_count_; ++i) {
        ClientSubscription& s = slots_[i];
        if (s.state_ == ClientSubscription::State::Active && now_ms >= s.expires_at_ms_) {
            s = ClientSubscription();
        } else if (s.state_ == ClientSubscription::State::Pending &&
                   now_ms >= s.pending_deadline_ms_) {
            s = ClientSubscription();
        }
    }
}

std::uint32_t SubscriptionClient::next_renewal_due(std::uint64_t now_ms) const noexcept {
    for (std::size_t i = 0U; i < slot_count_; ++i) {
        const ClientSubscription& s = slots_[i];
        if (s.state_ == ClientSubscription::State::Active && now_ms >= s.renew_at_ms_) {
            return s.local_id_;
        }
    }
    return 0U;
}

bool SubscriptionClient::renew(std::uint32_t local_id, std::uint32_t own_source_id,
                               std::uint32_t own_boot_id, std::uint32_t own_sequence,
                               std::uint64_t now_ms, std::uint8_t* out,
                               std::size_t out_capacity, std::size_t* out_size) noexcept {
    ClientSubscription* slot = find_by_local_id(local_id);
    if (slot == nullptr || slot->state_ != ClientSubscription::State::Active) {
        return false;
    }
    if (!build_subscribe(*slot, out, out_capacity, out_size)) return false;

    slot->state_ = ClientSubscription::State::Pending;
    slot->own_source_id_ = own_source_id;
    slot->own_boot_id_ = own_boot_id;
    slot->own_sequence_ = own_sequence;
    slot->pending_deadline_ms_ = now_ms + kSubscriptionPendingTimeoutMs;
    return true;
}

}  // namespace btp
