#include "btp/messages.hpp"

#include "messages_detail.hpp"

namespace btp {
namespace {

using detail::Reader;
using detail::Writer;

// docs/session-and-terminal.md section 1.5: role 0x00 and 0x05..0xFF are
// reserved.
bool valid_role(std::uint8_t role) noexcept {
    return role >= static_cast<std::uint8_t>(Role::Producer) &&
           role <= static_cast<std::uint8_t>(Role::DiagnosticTool);
}

// docs/commands.md section 1.4: the seven defined result statuses.
bool valid_result_status(std::uint8_t status) noexcept {
    return status <= static_cast<std::uint8_t>(ResultStatus::Busy);
}

// docs/session-and-terminal.md section 4.1: NORMAL..PROTOCOL_ERROR.
bool valid_close_reason(std::uint8_t reason) noexcept {
    return reason <= static_cast<std::uint8_t>(CloseReason::ProtocolError);
}

bool all_zero(const std::uint8_t* data, std::size_t size) noexcept {
    for (std::size_t index = 0U; index < size; ++index) {
        if (data[index] != 0U) {
            return false;
        }
    }
    return true;
}

// Common decode tail: propagate the reader's first error, else require the
// payload to be fully consumed (docs/commands.md section 7 step 10).
MessageError finish_decode(Reader& reader) noexcept {
    if (!reader.ok()) {
        return reader.error();
    }
    return reader.require_exhausted();
}

// Common encode tail: on success publish the written length, on failure write
// nothing observable.
MessageError finish_encode(Writer& writer, std::size_t* written) noexcept {
    if (!writer.ok()) {
        return writer.error();
    }
    *written = writer.written();
    return MessageError::Ok;
}

// --- request reference (docs/commands.md section 1.3) ---------------------

void read_request_ref(Reader& reader, RequestRef* out) noexcept {
    out->request_source_id = reader.u32();
    out->request_boot_id = reader.u32();
    out->reply_to_sequence = reader.u32();
}

void write_request_ref(Writer& writer, const RequestRef& ref) noexcept {
    writer.u32(ref.request_source_id);
    writer.u32(ref.request_boot_id);
    writer.u32(ref.reply_to_sequence);
}

// --- the shared "request reference + status" result ----------------------
// (SESSION_CLOSE_RESULT, UNSUBSCRIBE_RESULT -- docs/commands.md section 4.4)

MessageError decode_control_result(const std::uint8_t* payload, std::size_t size,
                                   ControlResult* out) noexcept {
    if (payload == nullptr || out == nullptr) {
        return MessageError::InvalidArgument;
    }
    Reader reader(payload, size);
    RequestRef ref = {};
    read_request_ref(reader, &ref);
    const std::uint8_t status = reader.u8();
    reader.expect_zero(1U);
    const std::uint16_t error_code = reader.u16();
    const MessageError tail = finish_decode(reader);
    if (tail != MessageError::Ok) {
        return tail;
    }
    if (!valid_result_status(status)) {
        return MessageError::InvalidValue;
    }
    out->request = ref;
    out->status = status;
    out->error_code = error_code;
    return MessageError::Ok;
}

MessageError encode_control_result(const ControlResult& in, std::uint8_t* out,
                                   std::size_t capacity, std::size_t* written) noexcept {
    if (out == nullptr || written == nullptr) {
        return MessageError::InvalidArgument;
    }
    if (!valid_result_status(in.status)) {
        return MessageError::InvalidValue;
    }
    Writer writer(out, capacity);
    write_request_ref(writer, in.request);
    writer.u8(in.status);
    writer.zeros(1U);
    writer.u16(in.error_code);
    return finish_encode(writer, written);
}

}  // namespace

// ===========================================================================
// message_error_string
// ===========================================================================

const char* message_error_string(MessageError error) noexcept {
    switch (error) {
        case MessageError::Ok:
            return "ok";
        case MessageError::InvalidArgument:
            return "invalid argument";
        case MessageError::BufferTooSmall:
            return "output buffer too small";
        case MessageError::PayloadTooShort:
            return "payload shorter than the fixed layout";
        case MessageError::TrailingBytes:
            return "trailing bytes after the payload";
        case MessageError::ReservedNotZero:
            return "reserved field or flag bit not zero";
        case MessageError::LengthOverflow:
            return "declared length runs past the payload";
        case MessageError::ZeroField:
            return "a field required to be non-zero is zero";
        case MessageError::InvalidValue:
            return "a field holds a value outside its defined set";
        case MessageError::CountTooLarge:
            return "declared count exceeds a protocol limit";
        case MessageError::RecordSizeMismatch:
            return "record_size does not match its content";
        case MessageError::CountMismatch:
            return "a declared count does not match the items present";
        case MessageError::NotAscending:
            return "a list required to ascend is not ascending";
        case MessageError::UnsupportedFormat:
            return "unknown format or status version";
        case MessageError::UnknownObject:
            return "object_id not in the message layer";
        case MessageError::WrongOrder:
            return "reader or writer call out of sequence";
    }
    return "unknown message error";
}

// ===========================================================================
// HELLO / HELLO_RESULT (docs/session-and-terminal.md sections 1-2)
// ===========================================================================

MessageError decode_hello(const std::uint8_t* payload, std::size_t size, Hello* out) noexcept {
    if (payload == nullptr || out == nullptr) {
        return MessageError::InvalidArgument;
    }
    Reader reader(payload, size);

    const std::uint8_t role = reader.u8();
    const std::uint8_t version_count = reader.u8();
    reader.expect_zero(2U);  // flags, zero in wire v2
    const std::uint32_t max_logical_payload = reader.u32();
    const std::uint16_t max_inflight = reader.u16();
    const std::uint16_t max_subscriptions = reader.u16();
    const std::uint32_t max_dedup = reader.u32();
    const std::uint32_t session_timeout_ms = reader.u32();
    const ByteView uuid = reader.raw(16U);
    const std::uint32_t config_revision = reader.u32();
    if (!reader.ok()) {
        return reader.error();
    }
    if (version_count == 0U) {
        return MessageError::ZeroField;
    }
    if (version_count > kMaxAnnouncedVersions) {
        return MessageError::CountTooLarge;
    }

    Hello hello = {};
    std::uint8_t previous = 0U;
    for (std::size_t index = 0U; index < version_count; ++index) {
        const std::uint8_t version = reader.u8();
        if (!reader.ok()) {
            return reader.error();
        }
        if (version == 0U) {
            return MessageError::ZeroField;
        }
        if (index != 0U && version <= previous) {
            return MessageError::NotAscending;  // ascending, no duplicates
        }
        hello.versions[index] = version;
        previous = version;
    }

    const MessageError tail = finish_decode(reader);
    if (tail != MessageError::Ok) {
        return tail;
    }
    if (!valid_role(role)) {
        return MessageError::InvalidValue;
    }
    if (max_logical_payload == 0U || max_inflight == 0U || max_subscriptions == 0U ||
        max_dedup == 0U || session_timeout_ms == 0U) {
        return MessageError::ZeroField;  // section 1.3: all capabilities non-zero
    }
    if (all_zero(uuid.data, 16U)) {
        return MessageError::ZeroField;  // section 1.1: the all-zero UUID is invalid
    }

    hello.role = role;
    hello.version_count = version_count;
    hello.max_logical_payload = max_logical_payload;
    hello.max_inflight_reassemblies = max_inflight;
    hello.max_subscriptions = max_subscriptions;
    hello.max_dedup_entries = max_dedup;
    hello.session_timeout_ms = session_timeout_ms;
    for (std::size_t index = 0U; index < 16U; ++index) {
        hello.peer_uuid[index] = uuid.data[index];
    }
    hello.config_revision = config_revision;
    *out = hello;
    return MessageError::Ok;
}

MessageError encode_hello(const Hello& in, std::uint8_t* out, std::size_t capacity,
                          std::size_t* written) noexcept {
    if (out == nullptr || written == nullptr) {
        return MessageError::InvalidArgument;
    }
    if (!valid_role(in.role)) {
        return MessageError::InvalidValue;
    }
    if (in.version_count == 0U) {
        return MessageError::ZeroField;
    }
    if (in.version_count > kMaxAnnouncedVersions) {
        return MessageError::CountTooLarge;
    }
    std::uint8_t previous = 0U;
    for (std::size_t index = 0U; index < in.version_count; ++index) {
        const std::uint8_t version = in.versions[index];
        if (version == 0U) {
            return MessageError::ZeroField;
        }
        if (index != 0U && version <= previous) {
            return MessageError::NotAscending;
        }
        previous = version;
    }
    if (in.max_logical_payload == 0U || in.max_inflight_reassemblies == 0U ||
        in.max_subscriptions == 0U || in.max_dedup_entries == 0U ||
        in.session_timeout_ms == 0U) {
        return MessageError::ZeroField;
    }
    if (all_zero(in.peer_uuid, 16U)) {
        return MessageError::ZeroField;
    }

    Writer writer(out, capacity);
    writer.u8(in.role);
    writer.u8(in.version_count);
    writer.zeros(2U);  // flags
    writer.u32(in.max_logical_payload);
    writer.u16(in.max_inflight_reassemblies);
    writer.u16(in.max_subscriptions);
    writer.u32(in.max_dedup_entries);
    writer.u32(in.session_timeout_ms);
    writer.raw(in.peer_uuid, 16U);
    writer.u32(in.config_revision);
    for (std::size_t index = 0U; index < in.version_count; ++index) {
        writer.u8(in.versions[index]);
    }
    return finish_encode(writer, written);
}

MessageError decode_hello_result(const std::uint8_t* payload, std::size_t size,
                                 HelloResult* out) noexcept {
    if (payload == nullptr || out == nullptr) {
        return MessageError::InvalidArgument;
    }
    Reader reader(payload, size);

    HelloResult result = {};
    read_request_ref(reader, &result.request);
    const std::uint8_t status = reader.u8();
    result.selected_version = reader.u8();
    result.error_code = reader.u16();
    result.max_logical_payload = reader.u32();
    result.max_inflight_reassemblies = reader.u16();
    result.max_subscriptions = reader.u16();
    result.max_dedup_entries = reader.u32();
    result.session_timeout_ms = reader.u32();
    const ByteView uuid = reader.raw(16U);
    result.config_revision = reader.u32();

    const MessageError tail = finish_decode(reader);
    if (tail != MessageError::Ok) {
        return tail;
    }
    // section 2: the result status is SUCCESS or UNSUPPORTED, nothing else.
    if (status != static_cast<std::uint8_t>(ResultStatus::Success) &&
        status != static_cast<std::uint8_t>(ResultStatus::Unsupported)) {
        return MessageError::InvalidValue;
    }
    result.status = status;
    for (std::size_t index = 0U; index < 16U; ++index) {
        result.peer_uuid[index] = uuid.data[index];
    }
    *out = result;
    return MessageError::Ok;
}

MessageError encode_hello_result(const HelloResult& in, std::uint8_t* out,
                                 std::size_t capacity, std::size_t* written) noexcept {
    if (out == nullptr || written == nullptr) {
        return MessageError::InvalidArgument;
    }
    if (in.status != static_cast<std::uint8_t>(ResultStatus::Success) &&
        in.status != static_cast<std::uint8_t>(ResultStatus::Unsupported)) {
        return MessageError::InvalidValue;
    }
    Writer writer(out, capacity);
    write_request_ref(writer, in.request);
    writer.u8(in.status);
    writer.u8(in.selected_version);
    writer.u16(in.error_code);
    writer.u32(in.max_logical_payload);
    writer.u16(in.max_inflight_reassemblies);
    writer.u16(in.max_subscriptions);
    writer.u32(in.max_dedup_entries);
    writer.u32(in.session_timeout_ms);
    writer.raw(in.peer_uuid, 16U);
    writer.u32(in.config_revision);
    return finish_encode(writer, written);
}

// ===========================================================================
// SESSION_CLOSE / SESSION_CLOSE_RESULT (docs/session-and-terminal.md section 4)
// ===========================================================================

MessageError decode_session_close(const std::uint8_t* payload, std::size_t size,
                                  SessionClose* out) noexcept {
    if (payload == nullptr || out == nullptr) {
        return MessageError::InvalidArgument;
    }
    Reader reader(payload, size);
    const std::uint8_t reason = reader.u8();
    reader.expect_zero(3U);
    const std::uint32_t drain_timeout_ms = reader.u32();
    const MessageError tail = finish_decode(reader);
    if (tail != MessageError::Ok) {
        return tail;
    }
    if (!valid_close_reason(reason)) {
        return MessageError::InvalidValue;
    }
    out->reason = reason;
    out->drain_timeout_ms = drain_timeout_ms;
    return MessageError::Ok;
}

MessageError encode_session_close(const SessionClose& in, std::uint8_t* out,
                                  std::size_t capacity, std::size_t* written) noexcept {
    if (out == nullptr || written == nullptr) {
        return MessageError::InvalidArgument;
    }
    if (!valid_close_reason(in.reason)) {
        return MessageError::InvalidValue;
    }
    Writer writer(out, capacity);
    writer.u8(in.reason);
    writer.zeros(3U);
    writer.u32(in.drain_timeout_ms);
    return finish_encode(writer, written);
}

MessageError decode_session_close_result(const std::uint8_t* payload, std::size_t size,
                                         ControlResult* out) noexcept {
    return decode_control_result(payload, size, out);
}

MessageError encode_session_close_result(const ControlResult& in, std::uint8_t* out,
                                         std::size_t capacity, std::size_t* written) noexcept {
    return encode_control_result(in, out, capacity, written);
}

// ===========================================================================
// COMMAND_REQUEST / COMMAND_RESULT (docs/commands.md section 2)
// ===========================================================================

MessageError decode_command_request(const std::uint8_t* payload, std::size_t size,
                                    CommandRequest* out) noexcept {
    if (payload == nullptr || out == nullptr) {
        return MessageError::InvalidArgument;
    }
    Reader reader(payload, size);
    const std::uint32_t target_source_id = reader.u32();
    const std::uint32_t target_boot_id = reader.u32();
    const std::uint16_t action_id = reader.u16();
    const std::uint16_t action_version = reader.u16();
    reader.expect_zero(2U);  // flags
    reader.expect_zero(2U);  // reserved
    // parameter_size (uint32_le) + parameters is exactly a bytes_u32.
    const ByteView parameters = reader.bytes_u32(kMaxActionBody);
    const MessageError tail = finish_decode(reader);
    if (tail != MessageError::Ok) {
        return tail;
    }
    if (target_source_id == 0U || target_boot_id == 0U || action_id == 0U ||
        action_version == 0U) {
        return MessageError::ZeroField;  // section 2.1: these must be non-zero
    }
    out->target_source_id = target_source_id;
    out->target_boot_id = target_boot_id;
    out->action_id = action_id;
    out->action_version = action_version;
    out->parameters = parameters;
    return MessageError::Ok;
}

MessageError encode_command_request(const CommandRequest& in, std::uint8_t* out,
                                    std::size_t capacity, std::size_t* written) noexcept {
    if (out == nullptr || written == nullptr) {
        return MessageError::InvalidArgument;
    }
    if (in.target_source_id == 0U || in.target_boot_id == 0U || in.action_id == 0U ||
        in.action_version == 0U) {
        return MessageError::ZeroField;
    }
    if (in.parameters.size > kMaxActionBody) {
        return MessageError::CountTooLarge;
    }
    Writer writer(out, capacity);
    writer.u32(in.target_source_id);
    writer.u32(in.target_boot_id);
    writer.u16(in.action_id);
    writer.u16(in.action_version);
    writer.zeros(2U);  // flags
    writer.zeros(2U);  // reserved
    writer.bytes_u32(in.parameters, kMaxActionBody);
    return finish_encode(writer, written);
}

MessageError decode_command_result(const std::uint8_t* payload, std::size_t size,
                                   CommandResult* out) noexcept {
    if (payload == nullptr || out == nullptr) {
        return MessageError::InvalidArgument;
    }
    Reader reader(payload, size);
    CommandResult result = {};
    read_request_ref(reader, &result.request);
    result.action_id = reader.u16();
    result.action_version = reader.u16();
    const std::uint8_t status = reader.u8();
    reader.expect_zero(1U);
    result.error_code = reader.u16();
    result.message = reader.utf8_u16(kMaxResultMessage);
    result.result = reader.bytes_u32(kMaxActionBody);
    const MessageError tail = finish_decode(reader);
    if (tail != MessageError::Ok) {
        return tail;
    }
    if (!valid_result_status(status)) {
        return MessageError::InvalidValue;
    }
    result.status = status;
    *out = result;
    return MessageError::Ok;
}

MessageError encode_command_result(const CommandResult& in, std::uint8_t* out,
                                   std::size_t capacity, std::size_t* written) noexcept {
    if (out == nullptr || written == nullptr) {
        return MessageError::InvalidArgument;
    }
    if (!valid_result_status(in.status)) {
        return MessageError::InvalidValue;
    }
    if (in.message.size > kMaxResultMessage || in.message.size > 0xFFFFU ||
        in.result.size > kMaxActionBody) {
        return MessageError::CountTooLarge;
    }
    Writer writer(out, capacity);
    write_request_ref(writer, in.request);
    writer.u16(in.action_id);
    writer.u16(in.action_version);
    writer.u8(in.status);
    writer.zeros(1U);
    writer.u16(in.error_code);
    writer.utf8_u16(in.message, kMaxResultMessage);
    writer.bytes_u32(in.result, kMaxActionBody);
    return finish_encode(writer, written);
}

// ===========================================================================
// MANIFEST_REQUEST (docs/commands.md section 3.1)
// ===========================================================================

MessageError decode_manifest_request(const std::uint8_t* payload, std::size_t size,
                                     ManifestRequest* out) noexcept {
    if (payload == nullptr || out == nullptr) {
        return MessageError::InvalidArgument;
    }
    Reader reader(payload, size);
    const std::uint32_t target_source_id = reader.u32();
    const std::uint32_t target_boot_id = reader.u32();
    const std::uint32_t known_config_revision = reader.u32();
    const MessageError tail = finish_decode(reader);
    if (tail != MessageError::Ok) {
        return tail;
    }
    out->target_source_id = target_source_id;
    out->target_boot_id = target_boot_id;
    out->known_config_revision = known_config_revision;
    return MessageError::Ok;
}

MessageError encode_manifest_request(const ManifestRequest& in, std::uint8_t* out,
                                     std::size_t capacity, std::size_t* written) noexcept {
    if (out == nullptr || written == nullptr) {
        return MessageError::InvalidArgument;
    }
    Writer writer(out, capacity);
    writer.u32(in.target_source_id);
    writer.u32(in.target_boot_id);
    writer.u32(in.known_config_revision);
    return finish_encode(writer, written);
}

// ===========================================================================
// SUBSCRIBE / SUBSCRIBE_RESULT / UNSUBSCRIBE / UNSUBSCRIBE_RESULT
// (docs/commands.md section 4)
// ===========================================================================

MessageError decode_subscribe(const std::uint8_t* payload, std::size_t size,
                              Subscribe* out) noexcept {
    if (payload == nullptr || out == nullptr) {
        return MessageError::InvalidArgument;
    }
    Reader reader(payload, size);
    const std::uint32_t target_source_id = reader.u32();
    const std::uint32_t target_boot_id = reader.u32();
    const std::uint16_t topic_id = reader.u16();
    reader.expect_zero(2U);  // flags
    const std::uint32_t requested_rate_millihz = reader.u32();
    const std::uint32_t requested_lease_ms = reader.u32();
    const MessageError tail = finish_decode(reader);
    if (tail != MessageError::Ok) {
        return tail;
    }
    if (target_source_id == 0U || target_boot_id == 0U || topic_id == 0U ||
        requested_rate_millihz == 0U || requested_lease_ms == 0U) {
        return MessageError::ZeroField;
    }
    out->target_source_id = target_source_id;
    out->target_boot_id = target_boot_id;
    out->topic_id = topic_id;
    out->requested_rate_millihz = requested_rate_millihz;
    out->requested_lease_ms = requested_lease_ms;
    return MessageError::Ok;
}

MessageError encode_subscribe(const Subscribe& in, std::uint8_t* out,
                              std::size_t capacity, std::size_t* written) noexcept {
    if (out == nullptr || written == nullptr) {
        return MessageError::InvalidArgument;
    }
    if (in.target_source_id == 0U || in.target_boot_id == 0U || in.topic_id == 0U ||
        in.requested_rate_millihz == 0U || in.requested_lease_ms == 0U) {
        return MessageError::ZeroField;
    }
    Writer writer(out, capacity);
    writer.u32(in.target_source_id);
    writer.u32(in.target_boot_id);
    writer.u16(in.topic_id);
    writer.zeros(2U);  // flags
    writer.u32(in.requested_rate_millihz);
    writer.u32(in.requested_lease_ms);
    return finish_encode(writer, written);
}

MessageError decode_subscribe_result(const std::uint8_t* payload, std::size_t size,
                                     SubscribeResult* out) noexcept {
    if (payload == nullptr || out == nullptr) {
        return MessageError::InvalidArgument;
    }
    Reader reader(payload, size);
    SubscribeResult result = {};
    read_request_ref(reader, &result.request);
    const std::uint8_t status = reader.u8();
    reader.expect_zero(1U);
    result.error_code = reader.u16();
    result.subscription_id = reader.u32();
    result.effective_rate_millihz = reader.u32();
    result.granted_lease_ms = reader.u32();
    const MessageError tail = finish_decode(reader);
    if (tail != MessageError::Ok) {
        return tail;
    }
    if (!valid_result_status(status)) {
        return MessageError::InvalidValue;
    }
    result.status = status;
    *out = result;
    return MessageError::Ok;
}

MessageError encode_subscribe_result(const SubscribeResult& in, std::uint8_t* out,
                                     std::size_t capacity, std::size_t* written) noexcept {
    if (out == nullptr || written == nullptr) {
        return MessageError::InvalidArgument;
    }
    if (!valid_result_status(in.status)) {
        return MessageError::InvalidValue;
    }
    Writer writer(out, capacity);
    write_request_ref(writer, in.request);
    writer.u8(in.status);
    writer.zeros(1U);
    writer.u16(in.error_code);
    writer.u32(in.subscription_id);
    writer.u32(in.effective_rate_millihz);
    writer.u32(in.granted_lease_ms);
    return finish_encode(writer, written);
}

MessageError decode_unsubscribe(const std::uint8_t* payload, std::size_t size,
                                Unsubscribe* out) noexcept {
    if (payload == nullptr || out == nullptr) {
        return MessageError::InvalidArgument;
    }
    Reader reader(payload, size);
    const std::uint32_t target_source_id = reader.u32();
    const std::uint32_t target_boot_id = reader.u32();
    const std::uint32_t subscription_id = reader.u32();
    const MessageError tail = finish_decode(reader);
    if (tail != MessageError::Ok) {
        return tail;
    }
    if (target_source_id == 0U || target_boot_id == 0U || subscription_id == 0U) {
        return MessageError::ZeroField;  // section 4.4: all values non-zero
    }
    out->target_source_id = target_source_id;
    out->target_boot_id = target_boot_id;
    out->subscription_id = subscription_id;
    return MessageError::Ok;
}

MessageError encode_unsubscribe(const Unsubscribe& in, std::uint8_t* out,
                                std::size_t capacity, std::size_t* written) noexcept {
    if (out == nullptr || written == nullptr) {
        return MessageError::InvalidArgument;
    }
    if (in.target_source_id == 0U || in.target_boot_id == 0U || in.subscription_id == 0U) {
        return MessageError::ZeroField;
    }
    Writer writer(out, capacity);
    writer.u32(in.target_source_id);
    writer.u32(in.target_boot_id);
    writer.u32(in.subscription_id);
    return finish_encode(writer, written);
}

MessageError decode_unsubscribe_result(const std::uint8_t* payload, std::size_t size,
                                       ControlResult* out) noexcept {
    return decode_control_result(payload, size, out);
}

MessageError encode_unsubscribe_result(const ControlResult& in, std::uint8_t* out,
                                       std::size_t capacity, std::size_t* written) noexcept {
    return encode_control_result(in, out, capacity, written);
}

// ===========================================================================
// STATUS (docs/commands.md section 5)
// ===========================================================================

namespace {

// The 92-octet counter block, identical at the same offsets in both versions
// (docs/commands.md section 5.2). Reads status_version from the wire.
void read_status_block(Reader& reader, StatusV1* out) noexcept {
    out->status_version = reader.u16();
    out->flags = reader.u16();
    out->uptime_us = reader.u64();
    out->frames_rx = reader.u64();
    out->frames_tx = reader.u64();
    out->frames_dropped = reader.u64();
    out->crc_errors = reader.u64();
    out->decode_errors = reader.u64();
    out->reassembly_completed = reader.u64();
    out->reassembly_timeouts = reader.u64();
    out->reassembly_rejected = reader.u64();
    out->command_duplicates = reader.u64();
    out->telemetry_dropped = reader.u64();
}

void write_status_block(Writer& writer, const StatusV1& in,
                        std::uint16_t status_version) noexcept {
    writer.u16(status_version);
    writer.u16(in.flags);
    writer.u64(in.uptime_us);
    writer.u64(in.frames_rx);
    writer.u64(in.frames_tx);
    writer.u64(in.frames_dropped);
    writer.u64(in.crc_errors);
    writer.u64(in.decode_errors);
    writer.u64(in.reassembly_completed);
    writer.u64(in.reassembly_timeouts);
    writer.u64(in.reassembly_rejected);
    writer.u64(in.command_duplicates);
    writer.u64(in.telemetry_dropped);
}

void read_topic_status(Reader& reader, TopicStatusRecord* out) noexcept {
    out->source_id = reader.u32();
    out->topic_id = reader.u16();
    out->subscriber_count = reader.u16();
    out->effective_rate_millihz = reader.u32();
    out->bytes_total = reader.u64();
    out->samples_dropped_total = reader.u64();
}

}  // namespace

MessageError status_topic_count(const std::uint8_t* payload, std::size_t size,
                                std::uint16_t* count) noexcept {
    if (payload == nullptr || count == nullptr) {
        return MessageError::InvalidArgument;
    }
    if (size < kStatusV1Size) {
        return MessageError::PayloadTooShort;
    }
    Reader reader(payload, size);
    const std::uint16_t version = reader.u16();
    if (version != 1U && version != 2U) {
        return MessageError::UnsupportedFormat;
    }
    if (version == 1U) {
        *count = 0U;
        return MessageError::Ok;
    }
    if (size < kStatusV1Size + 2U) {
        return MessageError::PayloadTooShort;
    }
    *count = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(payload[kStatusV1Size]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(payload[kStatusV1Size + 1U]) << 8U));
    return MessageError::Ok;
}

MessageError decode_status(const std::uint8_t* payload, std::size_t size,
                           StatusV1* base,
                           TopicStatusRecord* topics, std::size_t topics_capacity,
                           std::size_t* topics_written) noexcept {
    if (payload == nullptr || base == nullptr || topics_written == nullptr) {
        return MessageError::InvalidArgument;
    }
    Reader reader(payload, size);
    StatusV1 block = {};
    read_status_block(reader, &block);
    if (!reader.ok()) {
        return reader.error();
    }
    if (block.status_version != 1U && block.status_version != 2U) {
        return MessageError::UnsupportedFormat;
    }

    if (block.status_version == 1U) {
        const MessageError tail = reader.require_exhausted();
        if (tail != MessageError::Ok) {
            return tail;
        }
        *base = block;
        *topics_written = 0U;
        return MessageError::Ok;
    }

    const std::uint16_t declared = reader.u16();
    if (!reader.ok()) {
        return reader.error();
    }
    if (declared != 0U && topics == nullptr) {
        return MessageError::InvalidArgument;
    }
    if (declared > topics_capacity) {
        return MessageError::CountTooLarge;
    }
    // Bounds the record run explicitly before consuming it (section 7 step 3):
    // 92 + 2 + 28 * declared must equal size, no more and no less.
    const std::size_t expected =
        kStatusV1Size + 2U + (static_cast<std::size_t>(declared) * kTopicStatusRecordSize);
    if (size < expected) {
        return MessageError::PayloadTooShort;
    }
    if (size > expected) {
        return MessageError::TrailingBytes;
    }
    for (std::size_t index = 0U; index < declared; ++index) {
        read_topic_status(reader, &topics[index]);
    }
    const MessageError tail = finish_decode(reader);
    if (tail != MessageError::Ok) {
        return tail;
    }
    *base = block;
    *topics_written = declared;
    return MessageError::Ok;
}

MessageError encode_status_v1(const StatusV1& base, std::uint8_t* out,
                              std::size_t capacity, std::size_t* written) noexcept {
    if (out == nullptr || written == nullptr) {
        return MessageError::InvalidArgument;
    }
    Writer writer(out, capacity);
    write_status_block(writer, base, 1U);
    return finish_encode(writer, written);
}

MessageError encode_status_v2(const StatusV1& base,
                              const TopicStatusRecord* topics, std::size_t topic_count,
                              std::uint8_t* out, std::size_t capacity,
                              std::size_t* written) noexcept {
    if (out == nullptr || written == nullptr ||
        (topics == nullptr && topic_count != 0U)) {
        return MessageError::InvalidArgument;
    }
    if (topic_count > 0xFFFFU) {
        return MessageError::CountTooLarge;
    }
    Writer writer(out, capacity);
    write_status_block(writer, base, 2U);
    writer.u16(static_cast<std::uint16_t>(topic_count));
    for (std::size_t index = 0U; index < topic_count; ++index) {
        const TopicStatusRecord& record = topics[index];
        writer.u32(record.source_id);
        writer.u16(record.topic_id);
        writer.u16(record.subscriber_count);
        writer.u32(record.effective_rate_millihz);
        writer.u64(record.bytes_total);
        writer.u64(record.samples_dropped_total);
    }
    return finish_encode(writer, written);
}

// ===========================================================================
// Session negotiation (docs/session-and-terminal.md section 2)
// ===========================================================================

namespace {

template <typename T>
T min_of(T a, T b) noexcept {
    return (a < b) ? a : b;
}

// versions[] is a fixed array of kMaxAnnouncedVersions; a decoded Hello never
// carries more (decode_hello caps version_count there), but negotiate() also
// takes a caller-built local Hello, so clamp before indexing rather than trust
// the count.
std::size_t announced_count(const Hello& hello) noexcept {
    return (hello.version_count < kMaxAnnouncedVersions)
               ? hello.version_count
               : kMaxAnnouncedVersions;
}

bool version_listed(const Hello& hello, std::uint8_t version) noexcept {
    const std::size_t count = announced_count(hello);
    for (std::size_t index = 0U; index < count; ++index) {
        if (hello.versions[index] == version) {
            return true;
        }
    }
    return false;
}

}  // namespace

EffectiveLimits negotiate(const Hello& local, const Hello& remote) noexcept {
    EffectiveLimits limits = {};

    // The highest envelope version both peers announce. versions[] is
    // ascending, so scanning local from the top and taking the first the
    // remote also lists gives the maximum in one pass (section 2.1).
    std::uint8_t selected = 0U;
    for (std::size_t index = announced_count(local); index-- > 0U;) {
        if (version_listed(remote, local.versions[index])) {
            selected = local.versions[index];
            break;
        }
    }
    if (selected == 0U) {
        return limits;  // section 2.3: no common version -> every field zero
    }

    limits.selected_version = selected;
    limits.max_logical_payload =
        min_of(local.max_logical_payload, remote.max_logical_payload);
    limits.max_inflight_reassemblies =
        min_of(local.max_inflight_reassemblies, remote.max_inflight_reassemblies);
    limits.max_subscriptions =
        min_of(local.max_subscriptions, remote.max_subscriptions);
    limits.max_dedup_entries =
        min_of(local.max_dedup_entries, remote.max_dedup_entries);
    limits.session_timeout_ms =
        min_of(local.session_timeout_ms, remote.session_timeout_ms);
    return limits;
}

// ===========================================================================
// is_message_object
// ===========================================================================

bool is_message_object(MessageType type, std::uint16_t object_id) noexcept {
    if (type == MessageType::Command) {
        return object_id == object_id::kCommandRequest ||
               object_id == object_id::kCommandResult;
    }
    if (type == MessageType::Control) {
        return object_id >= object_id::kHello &&
               object_id <= object_id::kSessionCloseResult;
    }
    return false;
}

// ===========================================================================
// MANIFEST_DATA (docs/commands.md section 3)
// ===========================================================================

namespace {

// A double is non-finite iff every exponent bit is set. Avoids <cmath> on the
// embedded target (docs/commands.md section 3.7: scale/offset must be finite).
bool is_finite_f64(double value) noexcept {
    std::uint64_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL;
}

// Reads the content of one field record from `reader` (already bounded to the
// record's content). Leftover octets in the reader are the enum run.
MessageError read_field_content(Reader& reader, FieldRecord* out,
                                ByteView* enum_entries) noexcept {
    out->field_id = reader.u16();
    out->order = reader.u16();
    out->type = reader.u8();
    out->flags = reader.u8();
    out->element_count = reader.u16();
    out->max_element_count = reader.u16();
    out->scale = reader.f64();
    out->offset = reader.f64();
    out->enum_count = reader.u16();
    out->name = reader.utf8_u16(kMaxNameOrUnit);
    out->unit = reader.utf8_u16(kMaxNameOrUnit);
    out->description = reader.utf8_u16(kMaxUtf8Text);
    if (!reader.ok()) {
        return reader.error();
    }
    if (out->field_id == 0U) {
        return MessageError::ZeroField;
    }
    if (out->enum_count > kMaxEnumOrErrorEntries) {
        return MessageError::CountTooLarge;
    }
    if (!is_finite_f64(out->scale) || !is_finite_f64(out->offset)) {
        return MessageError::InvalidValue;
    }
    *enum_entries = ByteView{nullptr, reader.remaining()};  // data patched by caller
    return MessageError::Ok;
}

// Advances `pos` past `count` length-delimited records within [pos, limit].
MessageError skip_records(const std::uint8_t* data, std::size_t& pos,
                          std::size_t limit, std::uint16_t count) noexcept {
    for (std::uint16_t index = 0U; index < count; ++index) {
        if ((pos + 4U) > limit) {
            return MessageError::RecordSizeMismatch;
        }
        const std::uint32_t record_size =
            static_cast<std::uint32_t>(data[pos]) |
            (static_cast<std::uint32_t>(data[pos + 1U]) << 8U) |
            (static_cast<std::uint32_t>(data[pos + 2U]) << 16U) |
            (static_cast<std::uint32_t>(data[pos + 3U]) << 24U);
        pos += 4U;
        if (record_size > (limit - pos)) {
            return MessageError::RecordSizeMismatch;
        }
        pos += record_size;
    }
    return MessageError::Ok;
}

// Manifest reader states.
enum : std::uint8_t {
    kMfFresh = 0U,
    kMfHeader,       // header() done; source_info not yet resolved
    kMfSourceInfo,   // iterating (or about to skip) source_info entries
    kMfTopics,       // between topic records
    kMfActions,      // between action records
    kMfDone,
};

}  // namespace

// --- ManifestReader ------------------------------------------------------

ManifestReader::ManifestReader(const std::uint8_t* payload, std::size_t size) noexcept
    : payload_(payload),
      size_(size),
      cursor_(0U),
      error_(MessageError::Ok),
      state_(kMfFresh),
      manifest_format_version_(0U),
      topic_count_(0U),
      action_count_(0U),
      topics_seen_(0U),
      actions_seen_(0U),
      source_info_left_(0U) {}

namespace {

// Shared by ManifestReader: read one length-delimited record's size and
// bounds-check it against the payload, returning the content span.
MessageError open_record(const std::uint8_t* payload, std::size_t size,
                         std::size_t cursor, std::size_t* content_start,
                         std::size_t* content_end) noexcept {
    if ((cursor + 4U) > size) {
        return MessageError::PayloadTooShort;
    }
    Reader head(payload + cursor, 4U);
    const std::uint32_t record_size = head.u32();
    *content_start = cursor + 4U;
    if (record_size > (size - *content_start)) {
        return MessageError::LengthOverflow;
    }
    *content_end = *content_start + record_size;
    return MessageError::Ok;
}

}  // namespace

MessageError ManifestReader::header(ManifestHeader* out) noexcept {
    if (payload_ == nullptr || out == nullptr) {
        return MessageError::InvalidArgument;
    }
    if (state_ != kMfFresh) {
        error_ = MessageError::WrongOrder;
        return error_;
    }
    Reader reader(payload_, size_);
    ManifestHeader header = {};
    read_request_ref(reader, &header.request);
    header.status = reader.u8();
    header.flags = reader.u8();
    header.error_code = reader.u16();
    header.manifest_format_version = reader.u16();
    reader.expect_zero(2U);  // reserved
    header.config_revision = reader.u32();
    const ByteView uuid = reader.raw(16U);
    header.described_source_id = reader.u32();
    header.described_boot_id = reader.u32();
    header.source_role = reader.u8();
    header.source_flags = reader.u8();
    header.catalog_index = reader.u16();
    header.catalog_count = reader.u16();
    header.topic_count = reader.u16();
    header.action_count = reader.u16();
    header.source_name = reader.utf8_u16(kMaxUtf8Text);
    if (!reader.ok()) {
        error_ = reader.error();
        return error_;
    }
    if (header.manifest_format_version != 1U && header.manifest_format_version != 2U) {
        error_ = MessageError::UnsupportedFormat;
        return error_;
    }
    if (!valid_result_status(header.status)) {
        error_ = MessageError::InvalidValue;
        return error_;
    }
    // A non-SUCCESS response (REJECTED / NOT_FOUND / STALE_TARGET_BOOT) describes
    // no source: source_role and the identity fields are "don't care",
    // conventionally zero. Only a SUCCESS descriptor must name a valid role.
    if (header.status == static_cast<std::uint8_t>(ResultStatus::Success) &&
        !valid_role(header.source_role)) {
        error_ = MessageError::InvalidValue;
        return error_;
    }
    if (header.topic_count > kMaxTopicsPerManifest ||
        header.action_count > kMaxActionsPerManifest ||
        header.catalog_count > kMaxSourcesPerCatalog) {
        error_ = MessageError::CountTooLarge;
        return error_;
    }
    for (std::size_t index = 0U; index < 16U; ++index) {
        header.source_uuid[index] = uuid.data[index];
    }
    *out = header;
    cursor_ = reader.consumed();
    manifest_format_version_ = header.manifest_format_version;
    topic_count_ = header.topic_count;
    action_count_ = header.action_count;
    state_ = kMfHeader;
    return MessageError::Ok;
}

void ManifestReader::skip_source_info() noexcept {
    if (error_ != MessageError::Ok) {
        return;
    }
    if (state_ == kMfHeader) {
        if (manifest_format_version_ == 1U) {
            state_ = kMfTopics;
            return;
        }
        if ((cursor_ + 2U) > size_) {
            error_ = MessageError::PayloadTooShort;
            return;
        }
        Reader head(payload_ + cursor_, 2U);
        source_info_left_ = head.u16();
        cursor_ += 2U;
        if (source_info_left_ > kMaxSourceInfoEntries) {
            error_ = MessageError::CountTooLarge;
            return;
        }
        state_ = kMfSourceInfo;
    }
    while (state_ == kMfSourceInfo && source_info_left_ != 0U) {
        Reader reader(payload_ + cursor_, size_ - cursor_);
        (void)reader.utf8_u16(kMaxSourceInfoKey);
        (void)reader.utf8_u16(kMaxSourceInfoLabelOrValue);
        (void)reader.utf8_u16(kMaxSourceInfoLabelOrValue);
        if (!reader.ok()) {
            error_ = reader.error();
            return;
        }
        cursor_ += reader.consumed();
        --source_info_left_;
    }
    if (state_ == kMfSourceInfo) {
        state_ = kMfTopics;
    }
}

void ManifestReader::skip_topics() noexcept {
    skip_source_info();
    while (error_ == MessageError::Ok && state_ == kMfTopics &&
           topics_seen_ != topic_count_) {
        std::size_t content_start = 0U;
        std::size_t content_end = 0U;
        error_ = open_record(payload_, size_, cursor_, &content_start, &content_end);
        if (error_ != MessageError::Ok) {
            return;
        }
        cursor_ = content_end;
        ++topics_seen_;
    }
    if (error_ == MessageError::Ok && state_ == kMfTopics) {
        state_ = kMfActions;
    }
}

ManifestReader::Step ManifestReader::next_source_info(SourceInfoEntry* out) noexcept {
    if (error_ != MessageError::Ok) {
        return Step::Error;
    }
    if (out == nullptr) {
        error_ = MessageError::InvalidArgument;
        return Step::Error;
    }
    if (state_ == kMfHeader) {
        if (manifest_format_version_ == 1U) {
            state_ = kMfTopics;
            return Step::End;
        }
        if ((cursor_ + 2U) > size_) {
            error_ = MessageError::PayloadTooShort;
            return Step::Error;
        }
        Reader head(payload_ + cursor_, 2U);
        source_info_left_ = head.u16();
        cursor_ += 2U;
        if (source_info_left_ > kMaxSourceInfoEntries) {
            error_ = MessageError::CountTooLarge;
            return Step::Error;
        }
        state_ = kMfSourceInfo;
    }
    if (state_ != kMfSourceInfo) {
        error_ = MessageError::WrongOrder;
        return Step::Error;
    }
    if (source_info_left_ == 0U) {
        state_ = kMfTopics;
        return Step::End;
    }
    Reader reader(payload_ + cursor_, size_ - cursor_);
    out->key = reader.utf8_u16(kMaxSourceInfoKey);
    out->label = reader.utf8_u16(kMaxSourceInfoLabelOrValue);
    out->value = reader.utf8_u16(kMaxSourceInfoLabelOrValue);
    if (!reader.ok()) {
        error_ = reader.error();
        return Step::Error;
    }
    cursor_ += reader.consumed();
    --source_info_left_;
    return Step::Item;
}

ManifestReader::Step ManifestReader::next_topic(TopicRecord* out,
                                                ByteView* field_records) noexcept {
    if (error_ != MessageError::Ok) {
        return Step::Error;
    }
    if (out == nullptr || field_records == nullptr) {
        error_ = MessageError::InvalidArgument;
        return Step::Error;
    }
    skip_source_info();
    if (error_ != MessageError::Ok) {
        return Step::Error;
    }
    if (state_ != kMfTopics) {
        error_ = MessageError::WrongOrder;
        return Step::Error;
    }
    if (topics_seen_ == topic_count_) {
        state_ = kMfActions;
        return Step::End;
    }
    std::size_t content_start = 0U;
    std::size_t content_end = 0U;
    error_ = open_record(payload_, size_, cursor_, &content_start, &content_end);
    if (error_ != MessageError::Ok) {
        return Step::Error;
    }
    Reader reader(payload_ + content_start, content_end - content_start);
    out->topic_id = reader.u16();
    out->schema_version = reader.u16();
    out->encoding = reader.u8();
    out->flags = reader.u8();
    out->field_count = reader.u16();
    out->max_rate_millihz = reader.u32();
    out->name = reader.utf8_u16(kMaxNameOrUnit);
    out->description = reader.utf8_u16(kMaxUtf8Text);
    if (!reader.ok()) {
        error_ = reader.error();
        return Step::Error;
    }
    if (out->topic_id == 0U || out->schema_version == 0U) {
        error_ = MessageError::ZeroField;
        return Step::Error;
    }
    if (out->field_count > kMaxFieldsPerSide) {
        error_ = MessageError::CountTooLarge;
        return Step::Error;
    }
    const std::size_t consumed = reader.consumed();
    *field_records = ByteView{payload_ + content_start + consumed,
                              (content_end - content_start) - consumed};
    cursor_ = content_end;
    ++topics_seen_;
    return Step::Item;
}

ManifestReader::Step ManifestReader::next_action(ActionRecord* out,
                                                 ByteView* parameter_records,
                                                 ByteView* result_records,
                                                 ByteView* error_entries) noexcept {
    if (error_ != MessageError::Ok) {
        return Step::Error;
    }
    if (out == nullptr || parameter_records == nullptr ||
        result_records == nullptr || error_entries == nullptr) {
        error_ = MessageError::InvalidArgument;
        return Step::Error;
    }
    skip_topics();
    if (error_ != MessageError::Ok) {
        return Step::Error;
    }
    if (state_ != kMfActions) {
        error_ = MessageError::WrongOrder;
        return Step::Error;
    }
    if (actions_seen_ == action_count_) {
        state_ = kMfDone;
        return Step::End;
    }
    std::size_t content_start = 0U;
    std::size_t content_end = 0U;
    error_ = open_record(payload_, size_, cursor_, &content_start, &content_end);
    if (error_ != MessageError::Ok) {
        return Step::Error;
    }
    Reader reader(payload_ + content_start, content_end - content_start);
    out->action_id = reader.u16();
    out->action_version = reader.u16();
    out->flags = reader.u16();
    out->parameter_encoding = reader.u8();
    out->result_encoding = reader.u8();
    out->parameter_field_count = reader.u16();
    out->result_field_count = reader.u16();
    out->execution_timeout_ms = reader.u32();
    out->name = reader.utf8_u16(kMaxNameOrUnit);
    out->description = reader.utf8_u16(kMaxUtf8Text);
    out->confirmation_text = reader.utf8_u16(kMaxUtf8Text);
    if (!reader.ok()) {
        error_ = reader.error();
        return Step::Error;
    }
    if (out->action_id == 0U || out->action_version == 0U) {
        error_ = MessageError::ZeroField;
        return Step::Error;
    }
    if (out->parameter_field_count > kMaxFieldsPerSide ||
        out->result_field_count > kMaxFieldsPerSide) {
        error_ = MessageError::CountTooLarge;
        return Step::Error;
    }

    // Partition the action record body: parameter records, result records,
    // then error_count and the error entries that run to the record end.
    std::size_t pos = content_start + reader.consumed();
    const std::size_t param_start = pos;
    error_ = skip_records(payload_, pos, content_end, out->parameter_field_count);
    if (error_ != MessageError::Ok) {
        return Step::Error;
    }
    *parameter_records = ByteView{payload_ + param_start, pos - param_start};

    const std::size_t result_start = pos;
    error_ = skip_records(payload_, pos, content_end, out->result_field_count);
    if (error_ != MessageError::Ok) {
        return Step::Error;
    }
    *result_records = ByteView{payload_ + result_start, pos - result_start};

    if ((pos + 2U) > content_end) {
        error_ = MessageError::RecordSizeMismatch;
        return Step::Error;
    }
    Reader error_count_reader(payload_ + pos, 2U);
    const std::uint16_t error_count = error_count_reader.u16();
    pos += 2U;
    if (error_count > kMaxEnumOrErrorEntries) {
        error_ = MessageError::CountTooLarge;
        return Step::Error;
    }
    out->error_count = error_count;
    *error_entries = ByteView{payload_ + pos, content_end - pos};

    cursor_ = content_end;
    ++actions_seen_;
    return Step::Item;
}

MessageError ManifestReader::finish() noexcept {
    skip_topics();
    while (error_ == MessageError::Ok && state_ == kMfActions &&
           actions_seen_ != action_count_) {
        std::size_t content_start = 0U;
        std::size_t content_end = 0U;
        error_ = open_record(payload_, size_, cursor_, &content_start, &content_end);
        if (error_ != MessageError::Ok) {
            return error_;
        }
        cursor_ = content_end;
        ++actions_seen_;
    }
    if (error_ != MessageError::Ok) {
        return error_;
    }
    if (cursor_ != size_) {
        error_ = MessageError::TrailingBytes;
        return error_;
    }
    state_ = kMfDone;
    return MessageError::Ok;
}

// --- FieldRecordReader / EnumEntryReader / ActionErrorReader -------------

FieldRecordReader::FieldRecordReader(ByteView run, std::uint16_t count) noexcept
    : data_(run.data), size_(run.size), cursor_(0U), left_(count),
      error_(MessageError::Ok) {}

FieldRecordReader::Step FieldRecordReader::next(FieldRecord* out,
                                               ByteView* enum_entries) noexcept {
    if (error_ != MessageError::Ok) {
        return Step::Error;
    }
    if (out == nullptr || enum_entries == nullptr) {
        error_ = MessageError::InvalidArgument;
        return Step::Error;
    }
    if (left_ == 0U) {
        if (cursor_ != size_) {
            error_ = MessageError::RecordSizeMismatch;
            return Step::Error;
        }
        return Step::End;
    }
    if ((cursor_ + 4U) > size_) {
        error_ = MessageError::RecordSizeMismatch;
        return Step::Error;
    }
    Reader head(data_ + cursor_, 4U);
    const std::uint32_t record_size = head.u32();
    const std::size_t content_start = cursor_ + 4U;
    if (record_size > (size_ - content_start)) {
        error_ = MessageError::RecordSizeMismatch;
        return Step::Error;
    }
    Reader reader(data_ + content_start, record_size);
    ByteView enums = {nullptr, 0U};
    const MessageError rc = read_field_content(reader, out, &enums);
    if (rc != MessageError::Ok) {
        error_ = rc;
        return Step::Error;
    }
    *enum_entries = ByteView{data_ + content_start + reader.consumed(),
                             record_size - reader.consumed()};
    cursor_ = content_start + record_size;
    --left_;
    return Step::Item;
}

EnumEntryReader::EnumEntryReader(ByteView run, std::uint16_t count) noexcept
    : data_(run.data), size_(run.size), cursor_(0U), left_(count),
      error_(MessageError::Ok) {}

EnumEntryReader::Step EnumEntryReader::next(EnumEntry* out) noexcept {
    if (error_ != MessageError::Ok) {
        return Step::Error;
    }
    if (out == nullptr) {
        error_ = MessageError::InvalidArgument;
        return Step::Error;
    }
    if (left_ == 0U) {
        if (cursor_ != size_) {
            error_ = MessageError::CountMismatch;
            return Step::Error;
        }
        return Step::End;
    }
    Reader reader(data_ + cursor_, size_ - cursor_);
    out->value = reader.u16();
    out->label = reader.utf8_u16(kMaxUtf8Text);
    if (!reader.ok()) {
        error_ = reader.error();
        return Step::Error;
    }
    cursor_ += reader.consumed();
    --left_;
    return Step::Item;
}

ActionErrorReader::ActionErrorReader(ByteView run, std::uint16_t count) noexcept
    : data_(run.data), size_(run.size), cursor_(0U), left_(count),
      error_(MessageError::Ok) {}

ActionErrorReader::Step ActionErrorReader::next(ActionError* out) noexcept {
    if (error_ != MessageError::Ok) {
        return Step::Error;
    }
    if (out == nullptr) {
        error_ = MessageError::InvalidArgument;
        return Step::Error;
    }
    if (left_ == 0U) {
        if (cursor_ != size_) {
            error_ = MessageError::CountMismatch;
            return Step::Error;
        }
        return Step::End;
    }
    Reader reader(data_ + cursor_, size_ - cursor_);
    out->error_code = reader.u16();
    out->label = reader.utf8_u16(kMaxUtf8Text);
    if (!reader.ok()) {
        error_ = reader.error();
        return Step::Error;
    }
    cursor_ += reader.consumed();
    --left_;
    return Step::Item;
}

// --- ManifestWriter ----------------------------------------------------

namespace {
enum : std::uint8_t {
    kMwFresh = 0U,
    kMwHeader,        // begin() done; source_info open (fmt 2) or ready for topics
    kMwTopic,         // between begin_topic() and end_topic()
    kMwActionParams,  // begin_action() .. first add_action_result()/error()
    kMwActionResults,
    kMwActionErrors,
    kMwDone,
};
}  // namespace

ManifestWriter::ManifestWriter(std::uint8_t* out, std::size_t capacity) noexcept
    : out_(out),
      capacity_(capacity),
      cursor_(0U),
      error_(MessageError::Ok),
      state_(kMwFresh),
      manifest_format_version_(0U),
      topic_count_(0U),
      action_count_(0U),
      topics_written_(0U),
      actions_written_(0U),
      fields_written_(0U),
      action_params_written_(0U),
      action_results_written_(0U),
      action_errors_written_(0U),
      record_start_(0U),
      field_record_start_(0U),
      error_count_slot_(0U),
      field_open_(false),
      source_info_open_(false),
      source_info_slot_(0U),
      source_info_written_(0U),
      field_enum_count_(0U),
      enums_written_(0U) {}

MessageError ManifestWriter::begin(const ManifestHeader& header) noexcept {
    if (out_ == nullptr) {
        return MessageError::InvalidArgument;
    }
    if (state_ != kMwFresh) {
        error_ = MessageError::WrongOrder;
        return error_;
    }
    if (header.manifest_format_version != 1U && header.manifest_format_version != 2U) {
        error_ = MessageError::UnsupportedFormat;
        return error_;
    }
    if (!valid_result_status(header.status)) {
        error_ = MessageError::InvalidValue;
        return error_;
    }
    // A non-SUCCESS response (REJECTED / NOT_FOUND / STALE_TARGET_BOOT) describes
    // no source: source_role and the identity fields are "don't care",
    // conventionally zero. Only a SUCCESS descriptor must name a valid role.
    if (header.status == static_cast<std::uint8_t>(ResultStatus::Success) &&
        !valid_role(header.source_role)) {
        error_ = MessageError::InvalidValue;
        return error_;
    }
    if (header.topic_count > kMaxTopicsPerManifest ||
        header.action_count > kMaxActionsPerManifest ||
        header.catalog_count > kMaxSourcesPerCatalog) {
        error_ = MessageError::CountTooLarge;
        return error_;
    }
    Writer writer(out_, capacity_);
    write_request_ref(writer, header.request);
    writer.u8(header.status);
    writer.u8(header.flags);
    writer.u16(header.error_code);
    writer.u16(header.manifest_format_version);
    writer.zeros(2U);  // reserved
    writer.u32(header.config_revision);
    writer.raw(header.source_uuid, 16U);
    writer.u32(header.described_source_id);
    writer.u32(header.described_boot_id);
    writer.u8(header.source_role);
    writer.u8(header.source_flags);
    writer.u16(header.catalog_index);
    writer.u16(header.catalog_count);
    writer.u16(header.topic_count);
    writer.u16(header.action_count);
    writer.utf8_u16(header.source_name, kMaxUtf8Text);
    if (!writer.ok()) {
        error_ = writer.error();
        return error_;
    }
    cursor_ = writer.written();
    manifest_format_version_ = header.manifest_format_version;
    topic_count_ = header.topic_count;
    action_count_ = header.action_count;
    if (manifest_format_version_ == 2U) {
        source_info_slot_ = cursor_;
        Writer slot(out_ + cursor_, capacity_ - cursor_);
        slot.u16(0U);  // info_count placeholder
        if (!slot.ok()) {
            error_ = slot.error();
            return error_;
        }
        cursor_ += 2U;
        source_info_open_ = true;
    }
    state_ = kMwHeader;
    return MessageError::Ok;
}

MessageError ManifestWriter::add_source_info(const SourceInfoEntry& entry) noexcept {
    if (error_ != MessageError::Ok) {
        return error_;
    }
    if (state_ != kMwHeader || !source_info_open_) {
        error_ = MessageError::WrongOrder;
        return error_;
    }
    if (source_info_written_ >= kMaxSourceInfoEntries) {
        error_ = MessageError::CountTooLarge;
        return error_;
    }
    Writer writer(out_ + cursor_, capacity_ - cursor_);
    writer.utf8_u16(entry.key, kMaxSourceInfoKey);
    writer.utf8_u16(entry.label, kMaxSourceInfoLabelOrValue);
    writer.utf8_u16(entry.value, kMaxSourceInfoLabelOrValue);
    if (!writer.ok()) {
        error_ = writer.error();
        return error_;
    }
    cursor_ += writer.written();
    ++source_info_written_;
    return MessageError::Ok;
}

void ManifestWriter::close_source_info() noexcept {
    if (source_info_open_) {
        Writer patch(out_ + source_info_slot_, 2U);
        patch.u16(source_info_written_);
        source_info_open_ = false;
    }
}

MessageError ManifestWriter::begin_topic(const TopicRecord& topic) noexcept {
    if (error_ != MessageError::Ok) {
        return error_;
    }
    if (state_ != kMwHeader) {
        error_ = MessageError::WrongOrder;
        return error_;
    }
    close_source_info();
    if (topics_written_ >= topic_count_) {
        error_ = MessageError::CountMismatch;
        return error_;
    }
    if (topic.topic_id == 0U || topic.schema_version == 0U) {
        error_ = MessageError::ZeroField;
        return error_;
    }
    if (topic.field_count > kMaxFieldsPerSide) {
        error_ = MessageError::CountTooLarge;
        return error_;
    }
    record_start_ = cursor_;
    Writer writer(out_ + cursor_, capacity_ - cursor_);
    writer.u32(0U);  // record_size placeholder
    writer.u16(topic.topic_id);
    writer.u16(topic.schema_version);
    writer.u8(topic.encoding);
    writer.u8(topic.flags);
    writer.u16(topic.field_count);
    writer.u32(topic.max_rate_millihz);
    writer.utf8_u16(topic.name, kMaxNameOrUnit);
    writer.utf8_u16(topic.description, kMaxUtf8Text);
    if (!writer.ok()) {
        error_ = writer.error();
        return error_;
    }
    cursor_ += writer.written();
    fields_written_ = 0U;
    field_open_ = false;
    topic_field_count_ = topic.field_count;
    state_ = kMwTopic;
    return MessageError::Ok;
}

void ManifestWriter::close_field() noexcept {
    if (!field_open_) {
        return;
    }
    if (enums_written_ != field_enum_count_) {
        error_ = MessageError::CountMismatch;
        field_open_ = false;
        return;
    }
    const std::size_t record_size = cursor_ - (field_record_start_ + 4U);
    Writer patch(out_ + field_record_start_, 4U);
    patch.u32(static_cast<std::uint32_t>(record_size));
    field_open_ = false;
}

MessageError ManifestWriter::add_field_common(const FieldRecord& field) noexcept {
    close_field();
    if (error_ != MessageError::Ok) {
        return error_;
    }
    if (field.field_id == 0U) {
        error_ = MessageError::ZeroField;
        return error_;
    }
    if (field.enum_count > kMaxEnumOrErrorEntries) {
        error_ = MessageError::CountTooLarge;
        return error_;
    }
    if (!is_finite_f64(field.scale) || !is_finite_f64(field.offset)) {
        error_ = MessageError::InvalidValue;
        return error_;
    }
    field_record_start_ = cursor_;
    Writer writer(out_ + cursor_, capacity_ - cursor_);
    writer.u32(0U);  // field record_size placeholder
    // enum_count slot is written as the real declared value so the reader
    // sees a consistent record; close_field checks the add_enum call count.
    writer.u16(field.field_id);
    writer.u16(field.order);
    writer.u8(field.type);
    writer.u8(field.flags);
    writer.u16(field.element_count);
    writer.u16(field.max_element_count);
    writer.f64(field.scale);
    writer.f64(field.offset);
    writer.u16(field.enum_count);
    writer.utf8_u16(field.name, kMaxNameOrUnit);
    writer.utf8_u16(field.unit, kMaxNameOrUnit);
    writer.utf8_u16(field.description, kMaxUtf8Text);
    if (!writer.ok()) {
        error_ = writer.error();
        return error_;
    }
    cursor_ += writer.written();
    field_open_ = true;
    field_enum_count_ = field.enum_count;
    enums_written_ = 0U;
    return MessageError::Ok;
}

MessageError ManifestWriter::add_field(const FieldRecord& field) noexcept {
    if (error_ != MessageError::Ok) {
        return error_;
    }
    if (state_ != kMwTopic) {
        error_ = MessageError::WrongOrder;
        return error_;
    }
    if (fields_written_ >= topic_field_count_) {
        error_ = MessageError::CountMismatch;
        return error_;
    }
    const MessageError rc = add_field_common(field);
    if (rc == MessageError::Ok) {
        ++fields_written_;
    }
    return rc;
}

MessageError ManifestWriter::add_enum(const EnumEntry& entry) noexcept {
    if (error_ != MessageError::Ok) {
        return error_;
    }
    if (!field_open_) {
        error_ = MessageError::WrongOrder;
        return error_;
    }
    if (enums_written_ >= field_enum_count_) {
        error_ = MessageError::CountMismatch;
        return error_;
    }
    Writer writer(out_ + cursor_, capacity_ - cursor_);
    writer.u16(entry.value);
    writer.utf8_u16(entry.label, kMaxUtf8Text);
    if (!writer.ok()) {
        error_ = writer.error();
        return error_;
    }
    cursor_ += writer.written();
    ++enums_written_;
    return MessageError::Ok;
}

MessageError ManifestWriter::end_topic() noexcept {
    if (error_ != MessageError::Ok) {
        return error_;
    }
    if (state_ != kMwTopic) {
        error_ = MessageError::WrongOrder;
        return error_;
    }
    close_field();
    if (error_ != MessageError::Ok) {
        return error_;
    }
    if (fields_written_ != topic_field_count_) {
        error_ = MessageError::CountMismatch;
        return error_;
    }
    const std::size_t record_size = cursor_ - (record_start_ + 4U);
    Writer patch(out_ + record_start_, 4U);
    patch.u32(static_cast<std::uint32_t>(record_size));
    ++topics_written_;
    state_ = kMwHeader;
    return MessageError::Ok;
}

MessageError ManifestWriter::begin_action(const ActionRecord& action) noexcept {
    if (error_ != MessageError::Ok) {
        return error_;
    }
    if (state_ != kMwHeader) {
        error_ = MessageError::WrongOrder;
        return error_;
    }
    close_source_info();
    if (topics_written_ != topic_count_) {
        error_ = MessageError::CountMismatch;
        return error_;
    }
    if (actions_written_ >= action_count_) {
        error_ = MessageError::CountMismatch;
        return error_;
    }
    if (action.action_id == 0U || action.action_version == 0U) {
        error_ = MessageError::ZeroField;
        return error_;
    }
    if (action.parameter_field_count > kMaxFieldsPerSide ||
        action.result_field_count > kMaxFieldsPerSide) {
        error_ = MessageError::CountTooLarge;
        return error_;
    }
    record_start_ = cursor_;
    Writer writer(out_ + cursor_, capacity_ - cursor_);
    writer.u32(0U);  // record_size placeholder
    writer.u16(action.action_id);
    writer.u16(action.action_version);
    writer.u16(action.flags);
    writer.u8(action.parameter_encoding);
    writer.u8(action.result_encoding);
    writer.u16(action.parameter_field_count);
    writer.u16(action.result_field_count);
    writer.u32(action.execution_timeout_ms);
    writer.utf8_u16(action.name, kMaxNameOrUnit);
    writer.utf8_u16(action.description, kMaxUtf8Text);
    writer.utf8_u16(action.confirmation_text, kMaxUtf8Text);
    if (!writer.ok()) {
        error_ = writer.error();
        return error_;
    }
    cursor_ += writer.written();
    action_params_written_ = 0U;
    action_results_written_ = 0U;
    action_errors_written_ = 0U;
    field_open_ = false;
    action_param_count_ = action.parameter_field_count;
    action_result_count_ = action.result_field_count;
    state_ = kMwActionParams;
    return MessageError::Ok;
}

MessageError ManifestWriter::add_action_param(const FieldRecord& field) noexcept {
    if (error_ != MessageError::Ok) {
        return error_;
    }
    if (state_ != kMwActionParams) {
        error_ = MessageError::WrongOrder;
        return error_;
    }
    if (action_params_written_ >= action_param_count_) {
        error_ = MessageError::CountMismatch;
        return error_;
    }
    const MessageError rc = add_field_common(field);
    if (rc == MessageError::Ok) {
        ++action_params_written_;
    }
    return rc;
}

MessageError ManifestWriter::add_action_result(const FieldRecord& field) noexcept {
    if (error_ != MessageError::Ok) {
        return error_;
    }
    if (state_ == kMwActionParams) {
        close_field();
        if (error_ != MessageError::Ok) {
            return error_;
        }
        if (action_params_written_ != action_param_count_) {
            error_ = MessageError::CountMismatch;
            return error_;
        }
        state_ = kMwActionResults;
    }
    if (state_ != kMwActionResults) {
        error_ = MessageError::WrongOrder;
        return error_;
    }
    if (action_results_written_ >= action_result_count_) {
        error_ = MessageError::CountMismatch;
        return error_;
    }
    const MessageError rc = add_field_common(field);
    if (rc == MessageError::Ok) {
        ++action_results_written_;
    }
    return rc;
}

void ManifestWriter::open_action_errors() noexcept {
    close_field();
    if (error_ != MessageError::Ok) {
        return;
    }
    if (state_ == kMwActionParams && action_params_written_ != action_param_count_) {
        error_ = MessageError::CountMismatch;
        return;
    }
    if (state_ != kMwActionErrors) {
        if (state_ == kMwActionParams) {
            state_ = kMwActionResults;
        }
        if (action_results_written_ != action_result_count_) {
            error_ = MessageError::CountMismatch;
            return;
        }
        error_count_slot_ = cursor_;
        Writer slot(out_ + cursor_, capacity_ - cursor_);
        slot.u16(0U);  // error_count placeholder
        if (!slot.ok()) {
            error_ = slot.error();
            return;
        }
        cursor_ += 2U;
        state_ = kMwActionErrors;
    }
}

MessageError ManifestWriter::add_action_error(const ActionError& entry) noexcept {
    if (error_ != MessageError::Ok) {
        return error_;
    }
    if (state_ != kMwActionParams && state_ != kMwActionResults &&
        state_ != kMwActionErrors) {
        error_ = MessageError::WrongOrder;
        return error_;
    }
    open_action_errors();
    if (error_ != MessageError::Ok) {
        return error_;
    }
    if (action_errors_written_ >= kMaxEnumOrErrorEntries) {
        error_ = MessageError::CountTooLarge;
        return error_;
    }
    Writer writer(out_ + cursor_, capacity_ - cursor_);
    writer.u16(entry.error_code);
    writer.utf8_u16(entry.label, kMaxUtf8Text);
    if (!writer.ok()) {
        error_ = writer.error();
        return error_;
    }
    cursor_ += writer.written();
    ++action_errors_written_;
    return MessageError::Ok;
}

MessageError ManifestWriter::end_action() noexcept {
    if (error_ != MessageError::Ok) {
        return error_;
    }
    if (state_ != kMwActionParams && state_ != kMwActionResults &&
        state_ != kMwActionErrors) {
        error_ = MessageError::WrongOrder;
        return error_;
    }
    open_action_errors();  // also writes error_count slot if not open yet
    if (error_ != MessageError::Ok) {
        return error_;
    }
    Writer patch_count(out_ + error_count_slot_, 2U);
    patch_count.u16(action_errors_written_);
    const std::size_t record_size = cursor_ - (record_start_ + 4U);
    Writer patch_size(out_ + record_start_, 4U);
    patch_size.u32(static_cast<std::uint32_t>(record_size));
    ++actions_written_;
    state_ = kMwHeader;
    return MessageError::Ok;
}

MessageError ManifestWriter::finish(std::size_t* written) noexcept {
    if (written == nullptr) {
        return MessageError::InvalidArgument;
    }
    if (error_ != MessageError::Ok) {
        return error_;
    }
    if (state_ != kMwHeader) {
        error_ = MessageError::WrongOrder;
        return error_;
    }
    close_source_info();
    if (topics_written_ != topic_count_ || actions_written_ != action_count_) {
        error_ = MessageError::CountMismatch;
        return error_;
    }
    if (cursor_ > kMaxLogicalManifest) {
        error_ = MessageError::CountTooLarge;
        return error_;
    }
    *written = cursor_;
    state_ = kMwDone;
    return MessageError::Ok;
}

}  // namespace btp
