#ifndef BTP_MESSAGES_HPP
#define BTP_MESSAGES_HPP

// The struct <-> bytes half of docs/commands.md and
// docs/session-and-terminal.md, written once.
//
// btp::codec stops at the frame: it turns a Header plus an opaque payload into
// wire octets and back. Every COMMAND and CONTROL object_id, though, has a
// fixed payload layout defined in the book -- HELLO, MANIFEST_DATA, STATUS,
// SUBSCRIBE, COMMAND_REQUEST and the rest. Until now each consumer re-derived
// those offsets from the prose by hand. This header is that layout, once,
// with the same guarantees as btp::codec:
//
//   * no internal allocation -- the caller owns every buffer;
//   * noexcept -- errors are returned, never thrown;
//   * no clock, no I/O, no global state;
//   * no partial output on failure;
//   * decode is zero-copy -- a ByteView an out-struct receives points into the
//     payload buffer and is valid only while that buffer is.
//
// It adds no wire field and changes no octet: minor-version territory (the
// layer landed in 2.2.0), not a wire change. TELEMETRY bodies (PACKED_LE /
// TLV_LE) are deliberately
// NOT here -- decoding a sample needs a parsed schema in hand and is a
// separate layer (a future btp::telemetry). This header does carry the
// manifest field records that DESCRIBE a schema.

#include "btp/codec.hpp"  // ByteView

#include <cstddef>
#include <cstdint>

namespace btp {

// ---------------------------------------------------------------------------
// object_id namespaces (docs/commands.md section 1.5)
// ---------------------------------------------------------------------------
// The interpretation of object_id depends on the envelope type. These are the
// fixed wire values; a consumer that spells them as constants stops copying a
// magic number into three files.

namespace object_id {

// type == COMMAND
static const std::uint16_t kCommandRequest = 0x0001U;
static const std::uint16_t kCommandResult = 0x0002U;

// type == CONTROL
static const std::uint16_t kHello = 0x0001U;
static const std::uint16_t kHelloResult = 0x0002U;
static const std::uint16_t kManifestRequest = 0x0003U;
static const std::uint16_t kManifestData = 0x0004U;
static const std::uint16_t kSubscribe = 0x0005U;
static const std::uint16_t kSubscribeResult = 0x0006U;
static const std::uint16_t kUnsubscribe = 0x0007U;
static const std::uint16_t kUnsubscribeResult = 0x0008U;
static const std::uint16_t kStatus = 0x0009U;
static const std::uint16_t kSessionClose = 0x000AU;
static const std::uint16_t kSessionCloseResult = 0x000BU;

// type == TERMINAL (payload is opaque bytes -- there is no struct for it,
// on purpose; see docs/session-and-terminal.md section 7)
static const std::uint16_t kTerminalIn = 0x0001U;
static const std::uint16_t kTerminalOut = 0x0002U;

}  // namespace object_id

// ---------------------------------------------------------------------------
// Protocol limits (docs/commands.md section 6)
// ---------------------------------------------------------------------------
// A decode rejects a declared length or count above the smallest applicable
// limit here rather than trusting it.

static const std::size_t kMaxLogicalManifest = 49152U;
static const std::size_t kMaxUtf8Text = 1024U;
static const std::size_t kMaxNameOrUnit = 128U;
static const std::size_t kMaxResultMessage = 512U;
static const std::size_t kMaxActionBody = 32768U;
static const std::size_t kMaxFieldsPerSide = 256U;
static const std::size_t kMaxSourcesPerCatalog = 1024U;
static const std::size_t kMaxTopicsPerManifest = 1024U;
static const std::size_t kMaxActionsPerManifest = 1024U;
static const std::size_t kMaxEnumOrErrorEntries = 256U;
static const std::size_t kMaxSourceInfoEntries = 32U;
static const std::size_t kMaxSourceInfoKey = 64U;
static const std::size_t kMaxSourceInfoLabelOrValue = 256U;

// The wire allows HELLO to announce up to 255 envelope versions
// (docs/session-and-terminal.md section 1.4), but only 0x01 and 0x02 are
// defined. The reference library stores the list in a fixed array of this
// size and rejects a longer one with MessageError::CountTooLarge rather than
// sizing for a case that cannot happen.
static const std::size_t kMaxAnnouncedVersions = 8U;

// Fixed logical-payload sizes that other code aligns to.
static const std::size_t kStatusV1Size = 92U;
static const std::size_t kTopicStatusRecordSize = 28U;

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------
// A payload-layer error, distinct from btp::Error (which is about the frame).
// Keeping the two enums apart is deliberate: "the CRC did not match" and "a
// declared utf8_u16 length runs past the payload" are different failures at
// different layers, and one call site should never have to tell them apart by
// value range.

enum class MessageError : std::uint8_t {
    Ok = 0U,
    InvalidArgument,     // null pointer, or an encode struct that cannot be represented
    BufferTooSmall,      // encode: the output capacity is not enough
    PayloadTooShort,     // fewer octets than the fixed portion of the payload needs
    TrailingBytes,       // octets remain after the payload was consumed (section 7 step 10)
    ReservedNotZero,     // a reserved field, or an unassigned flag bit, is not zero
    LengthOverflow,      // a declared length (utf8_u16 / bytes_u32 / record_size) runs past the payload
    ZeroField,           // a field the spec requires to be non-zero is zero
    InvalidValue,        // a field holds a value outside its defined set (a reserved role, an undefined close reason)
    CountTooLarge,       // a declared count exceeds a section 6 limit or a library cap
    RecordSizeMismatch,  // record_size does not equal the octets its content consumes
    CountMismatch,       // a declared count (topic_count, field_count, enum_count) != items written
    NotAscending,        // versions[], a field order, or a TLV field_id is not strictly increasing
    UnsupportedFormat,   // manifest_format_version or status_version is a value this layer does not know
    UnknownObject,       // the object_id does not belong to the COMMAND / CONTROL payload layer
    WrongOrder,          // a ManifestReader / ManifestWriter call was made out of sequence
};

const char* message_error_string(MessageError error) noexcept;

// True when (type, object_id) names a payload this layer has a decoder for.
// TERMINAL is excluded on purpose: its payload is opaque bytes, no struct.
// A router can use this to tell "a message I should decode" from "a frame I
// only relay" before dispatching to the matching decode_*.
bool is_message_object(MessageType type, std::uint16_t object_id) noexcept;

// ---------------------------------------------------------------------------
// Traffic priority (docs/model.md section 8, docs/session-and-terminal.md
// section 8)
// ---------------------------------------------------------------------------
// The six logical priority classes the specification defines for outgoing
// traffic under congestion. Numeric values match the spec table: 1 is the
// highest priority, 6 the lowest, and telemetry is the first class dropped
// when capacity runs short. FIFO order holds within a class.
//
// This is a classification only. Which queues an integration keeps, how many,
// and how it drains them are "implementation-dependent" (section 8) -- a
// dongle that never originates COMMAND_REQUEST collapses classes 1 and 2 into
// one queue; a robot keeps all six. priority_class() puts the *rule* (which
// object_id lands in which class) in one place so those queue sets do not each
// re-derive it from the prose.

enum class PriorityClass : std::uint8_t {
    Session = 1,       // HELLO(_RESULT), SESSION_CLOSE(_RESULT), COMMAND_RESULT
    Command = 2,       // COMMAND_REQUEST and other control results
    Subscription = 3,  // SUBSCRIBE / UNSUBSCRIBE (+ results), STATUS with DEGRADED
    Terminal = 4,      // TERMINAL_IN / TERMINAL_OUT
    Bulk = 5,          // MANIFEST_REQUEST / MANIFEST_DATA, periodic STATUS, LOG
    Telemetry = 6,     // TELEMETRY
};

const char* priority_class_string(PriorityClass priority) noexcept;

// The priority class of a message, from its envelope type and object_id.
// `status_degraded` moves a CONTROL/STATUS message from Bulk (periodic status,
// class 5) to Subscription (degraded status, class 3) -- the DEGRADED bit is
// in the STATUS payload, which this call does not see, so the caller passes
// it. An unrecognised (type, object_id) is classed Telemetry: lowest priority,
// discarded first, never ahead of real control traffic.
PriorityClass priority_class(MessageType type, std::uint16_t object_id,
                             bool status_degraded = false) noexcept;

// ---------------------------------------------------------------------------
// Field-value enums
// ---------------------------------------------------------------------------

// docs/session-and-terminal.md section 1.5
enum class Role : std::uint8_t {
    Producer = 0x01U,
    Gateway = 0x02U,
    Consumer = 0x03U,
    DiagnosticTool = 0x04U,
};

// docs/commands.md section 1.4
enum class ResultStatus : std::uint8_t {
    Success = 0x00U,
    Rejected = 0x01U,
    Failed = 0x02U,
    Timeout = 0x03U,
    Cancelled = 0x04U,
    Unsupported = 0x05U,
    Busy = 0x06U,
};

// docs/commands.md section 1.4. 0x000C..0x7FFF are reserved; 0x8000..0xFFFF are
// action-specific and described by ActionError records in the manifest.
enum class ResultError : std::uint16_t {
    None = 0x0000U,
    MalformedPayload = 0x0001U,
    UnknownObject = 0x0002U,
    InvalidArgument = 0x0003U,
    NotAuthorized = 0x0004U,
    CapacityExhausted = 0x0005U,
    ExecutionTimeout = 0x0006U,
    InternalError = 0x0007U,
    UnsupportedVersion = 0x0008U,
    StaleTargetBoot = 0x0009U,
    RequestConflict = 0x000AU,
    NotFound = 0x000BU,
};

// docs/session-and-terminal.md section 4.1
enum class CloseReason : std::uint8_t {
    Normal = 0x00U,
    VersionMismatch = 0x01U,
    ClientShutdown = 0x02U,
    ProtocolError = 0x03U,
};

// docs/telemetry.md section 8 -- a telemetry topic body encoding, carried
// through by TopicRecord::encoding. This layer never interprets a body.
enum class TelemetryEncoding : std::uint8_t {
    Invalid = 0x00U,
    OpaqueBytes = 0x01U,
    Utf8 = 0x02U,
    JsonUtf8 = 0x03U,
    CsvUtf8 = 0x04U,
    PackedLe = 0x05U,
    TlvLe = 0x06U,
};

// docs/commands.md section 3.9 -- an action parameter or result body encoding,
// carried through by ActionRecord::parameter_encoding / result_encoding. It
// shares wire values with TelemetryEncoding except 0x00, which is EMPTY here
// (a valid "no fields" body) and INVALID there.
enum class ActionEncoding : std::uint8_t {
    Empty = 0x00U,
    PackedLe = 0x05U,
    TlvLe = 0x06U,
};

// Manifest / source / topic / field / action flag bits.
static const std::uint8_t kManifestNotModified = 0x01U;       // MANIFEST_DATA flags bit 0
static const std::uint8_t kManifestCatalogComplete = 0x02U;   // MANIFEST_DATA flags bit 1
static const std::uint8_t kSourceOnline = 0x01U;              // source_flags bit 0
static const std::uint8_t kTopicSubscribable = 0x01U;         // topic flags bit 0
static const std::uint8_t kFieldNullable = 0x01U;             // field flags bit 0
static const std::uint8_t kFieldVariableCount = 0x02U;        // field flags bit 1
static const std::uint16_t kActionIdempotent = 0x0001U;       // action flags bit 0
static const std::uint16_t kActionDangerous = 0x0002U;        // action flags bit 1
static const std::uint16_t kStatusDegraded = 0x0001U;         // STATUS flags bit 0

// ---------------------------------------------------------------------------
// Shared payload pieces
// ---------------------------------------------------------------------------

// docs/commands.md section 1.3 -- how a response names the request it answers.
struct RequestRef {
    std::uint32_t request_source_id;
    std::uint32_t request_boot_id;
    std::uint32_t reply_to_sequence;
};

// The "request reference + status" result shape shared by
// SESSION_CLOSE_RESULT and UNSUBSCRIBE_RESULT (docs/commands.md section 4.4:
// "the corresponding result uses the common request-reference and status
// model"). The one-octet reserved field between status and error_code is
// validated as zero and not surfaced.
struct ControlResult {
    RequestRef request;
    std::uint8_t status;       // ResultStatus
    std::uint16_t error_code;  // ResultError
};

// ---------------------------------------------------------------------------
// Session: HELLO / HELLO_RESULT (docs/session-and-terminal.md sections 1-2)
// ---------------------------------------------------------------------------

struct Hello {
    std::uint8_t role;                              // Role
    std::uint8_t version_count;                     // valid entries in versions[]
    std::uint8_t versions[kMaxAnnouncedVersions];   // ascending, no zero, no repeats
    std::uint32_t max_logical_payload;
    std::uint16_t max_inflight_reassemblies;
    std::uint16_t max_subscriptions;
    std::uint32_t max_dedup_entries;
    std::uint32_t session_timeout_ms;
    std::uint8_t peer_uuid[16];                     // all-zero is invalid
    std::uint32_t config_revision;                  // 0 = peer publishes no manifest
};

// Builds a Hello with sane defaults for every field a deployment rarely
// needs to touch -- one protocol version (1), max_logical_payload 2048,
// max_inflight_reassemblies 4, max_subscriptions 8, max_dedup_entries 32,
// session_timeout_ms 30000, config_revision 0 (no manifest advertised).
// Override only what your deployment actually needs; `Hello h = {}; h.role
// = ...; h.version_count = ...` field by field is still there for the rest.
//
// `role` and `peer_uuid` have no safe default -- peer_uuid all-zero is
// explicitly invalid on the wire (Hello's own field comment above) -- so
// both are constructor arguments, not chain calls: a HelloBuilder always
// builds a wire-valid Hello, never a half-filled one waiting on a call you
// forgot.
//
//   btp::Hello h = btp::HelloBuilder(btp::Role::Producer, my_uuid).build();
//
//   btp::Hello h = btp::HelloBuilder(btp::Role::Consumer, my_uuid)
//                      .session_timeout_ms(15000U)
//                      .max_logical_payload(4096U)
//                      .build();
class HelloBuilder {
public:
    HelloBuilder(Role role, const std::uint8_t peer_uuid[16]) noexcept : hello_() {
        hello_.role = static_cast<std::uint8_t>(role);
        hello_.version_count = 1U;
        hello_.versions[0] = 1U;
        hello_.max_logical_payload = 2048U;
        hello_.max_inflight_reassemblies = 4U;
        hello_.max_subscriptions = 8U;
        hello_.max_dedup_entries = 32U;
        hello_.session_timeout_ms = 30000U;
        for (int i = 0; i < 16; ++i) hello_.peer_uuid[i] = peer_uuid[i];
        hello_.config_revision = 0U;
    }

    // Overrides the single announced version the constructor already set
    // (the common case -- more than one is still reachable off build()'s
    // own Hello by hand, same as any other field this builder does not
    // wrap: version_count / versions[] stay plain data).
    HelloBuilder& version(std::uint8_t v) noexcept {
        hello_.versions[0] = v;
        return *this;
    }
    HelloBuilder& max_logical_payload(std::uint32_t bytes) noexcept {
        hello_.max_logical_payload = bytes;
        return *this;
    }
    HelloBuilder& max_inflight_reassemblies(std::uint16_t n) noexcept {
        hello_.max_inflight_reassemblies = n;
        return *this;
    }
    HelloBuilder& max_subscriptions(std::uint16_t n) noexcept {
        hello_.max_subscriptions = n;
        return *this;
    }
    HelloBuilder& max_dedup_entries(std::uint32_t n) noexcept {
        hello_.max_dedup_entries = n;
        return *this;
    }
    HelloBuilder& session_timeout_ms(std::uint32_t ms) noexcept {
        hello_.session_timeout_ms = ms;
        return *this;
    }
    // Non-zero advertises "I serve a manifest, and this is its current
    // revision" (docs/commands.md section 3) -- 0 (the constructor's
    // default) means this peer publishes none.
    HelloBuilder& config_revision(std::uint32_t revision) noexcept {
        hello_.config_revision = revision;
        return *this;
    }

    Hello build() const noexcept { return hello_; }

private:
    Hello hello_;
};

struct HelloResult {
    RequestRef request;
    std::uint8_t status;            // ResultStatus: Success or Unsupported
    std::uint8_t selected_version;  // 0 when status != Success
    std::uint16_t error_code;       // ResultError
    std::uint32_t max_logical_payload;
    std::uint16_t max_inflight_reassemblies;
    std::uint16_t max_subscriptions;
    std::uint32_t max_dedup_entries;
    std::uint32_t session_timeout_ms;
    std::uint8_t peer_uuid[16];
    std::uint32_t config_revision;
};

// ---------------------------------------------------------------------------
// Session: SESSION_CLOSE / SESSION_CLOSE_RESULT
// (docs/session-and-terminal.md section 4)
// ---------------------------------------------------------------------------

struct SessionClose {
    std::uint8_t reason;             // CloseReason
    std::uint32_t drain_timeout_ms;  // the port owner caps this at 2000 ms
};

// ---------------------------------------------------------------------------
// Commands: COMMAND_REQUEST / COMMAND_RESULT (docs/commands.md section 2)
// ---------------------------------------------------------------------------

struct CommandRequest {
    std::uint32_t target_source_id;   // non-zero
    std::uint32_t target_boot_id;     // non-zero
    std::uint16_t action_id;          // non-zero
    std::uint16_t action_version;     // non-zero
    // flags and reserved are zero in wire v2; validated, not surfaced.
    ByteView parameters;              // exactly parameter_size octets; opaque here
};

struct CommandResult {
    RequestRef request;
    std::uint16_t action_id;          // may be 0 if the request could not be parsed that far
    std::uint16_t action_version;
    std::uint8_t status;              // ResultStatus
    std::uint16_t error_code;         // ResultError
    ByteView message;                 // utf8_u16, <= kMaxResultMessage
    ByteView result;                  // bytes_u32; opaque here (action-defined)
};

// ---------------------------------------------------------------------------
// Discovery: MANIFEST_REQUEST (docs/commands.md section 3.1)
// ---------------------------------------------------------------------------

struct ManifestRequest {
    std::uint32_t target_source_id;      // 0 = full catalog request
    std::uint32_t target_boot_id;        // 0 accepts the current boot
    std::uint32_t known_config_revision; // 0 requests the complete manifest
};

// ---------------------------------------------------------------------------
// Discovery: MANIFEST_DATA (docs/commands.md section 3)
// ---------------------------------------------------------------------------
// Walked with ManifestReader / built with ManifestWriter (below), not decoded
// into one struct: a manifest can carry 1024 topics and 1024 actions, each
// with up to 256 fields, and btp::messages never allocates.

// The fixed head, up to and including source_name.
struct ManifestHeader {
    RequestRef request;
    std::uint8_t status;                     // ResultStatus
    std::uint8_t flags;                       // kManifestNotModified | kManifestCatalogComplete
    std::uint16_t error_code;                 // ResultError
    std::uint16_t manifest_format_version;    // 1 or 2 (2 adds the source_info block)
    std::uint32_t config_revision;
    std::uint8_t source_uuid[16];
    std::uint32_t described_source_id;
    std::uint32_t described_boot_id;
    std::uint8_t source_role;                 // Role
    std::uint8_t source_flags;                // kSourceOnline
    std::uint16_t catalog_index;
    std::uint16_t catalog_count;
    std::uint16_t topic_count;
    std::uint16_t action_count;
    ByteView source_name;                     // utf8_u16
};

// docs/commands.md section 3.12 -- one informational key/label/value triple.
struct SourceInfoEntry {
    ByteView key;    // utf8_u16, <= kMaxSourceInfoKey, stable machine id (e.g. "fw_version")
    ByteView label;  // utf8_u16, may be empty, <= kMaxSourceInfoLabelOrValue
    ByteView value;  // utf8_u16, <= kMaxSourceInfoLabelOrValue, always textual
};

// docs/commands.md section 3.6
struct TopicRecord {
    std::uint16_t topic_id;          // non-zero
    std::uint16_t schema_version;    // non-zero
    std::uint8_t encoding;           // TelemetryEncoding
    std::uint8_t flags;              // kTopicSubscribable
    std::uint16_t field_count;
    std::uint32_t max_rate_millihz;
    ByteView name;                   // utf8_u16
    ByteView description;            // utf8_u16
};

// docs/commands.md section 3.7. `type` is the one-octet wire type from
// docs/telemetry.md section 13; that table names the types but does not pin
// their octet values, so this layer carries `type` as a raw octet for now
// (see PLANO_MESSAGES.txt -- a spec addition to number them belongs with the
// future btp::telemetry work).
struct FieldRecord {
    std::uint16_t field_id;          // non-zero
    std::uint16_t order;             // contiguous from zero, ascending
    std::uint8_t type;
    std::uint8_t flags;              // kFieldNullable | kFieldVariableCount
    std::uint16_t element_count;
    std::uint16_t max_element_count;
    double scale;                    // float64_le, finite
    double offset;                   // float64_le, finite
    std::uint16_t enum_count;
    ByteView name;                   // utf8_u16
    ByteView unit;                   // utf8_u16, "1" means dimensionless
    ByteView description;            // utf8_u16
};

// docs/commands.md section 3.8
struct EnumEntry {
    std::uint16_t value;
    ByteView label;                  // utf8_u16
};

// docs/commands.md section 3.9
struct ActionRecord {
    std::uint16_t action_id;         // non-zero
    std::uint16_t action_version;    // non-zero
    std::uint16_t flags;             // kActionIdempotent | kActionDangerous
    std::uint8_t parameter_encoding; // ActionEncoding
    std::uint8_t result_encoding;    // ActionEncoding
    std::uint16_t parameter_field_count;
    std::uint16_t result_field_count;
    std::uint32_t execution_timeout_ms;
    std::uint16_t error_count;       // action-specific error entries that follow the fields
    ByteView name;                   // utf8_u16
    ByteView description;            // utf8_u16
    ByteView confirmation_text;      // utf8_u16
};

// docs/commands.md section 3.10 -- error_code is in 0x8000..0xFFFF.
struct ActionError {
    std::uint16_t error_code;
    ByteView label;                  // utf8_u16
};

// ---------------------------------------------------------------------------
// Subscriptions (docs/commands.md section 4)
// ---------------------------------------------------------------------------

struct Subscribe {
    std::uint32_t target_source_id;       // non-zero
    std::uint32_t target_boot_id;         // non-zero
    std::uint16_t topic_id;               // non-zero
    // flags is zero in wire v2; validated, not surfaced.
    std::uint32_t requested_rate_millihz; // non-zero
    std::uint32_t requested_lease_ms;     // non-zero
};

struct SubscribeResult {
    RequestRef request;
    std::uint8_t status;                  // ResultStatus
    std::uint16_t error_code;             // ResultError
    std::uint32_t subscription_id;        // non-zero on success, zero on failure
    std::uint32_t effective_rate_millihz; // <= requested and <= topic max_rate_millihz
    std::uint32_t granted_lease_ms;
};

struct Unsubscribe {
    std::uint32_t target_source_id;  // non-zero
    std::uint32_t target_boot_id;    // non-zero
    std::uint32_t subscription_id;   // non-zero
};

// UNSUBSCRIBE_RESULT uses ControlResult.

// ---------------------------------------------------------------------------
// Status (docs/commands.md section 5)
// ---------------------------------------------------------------------------

// The 92-octet counter block. Identical layout at the same offsets in
// status_version 1 and 2 (section 5.2). Counters are monotonic within one
// source boot and saturate at UINT64_MAX.
struct StatusV1 {
    std::uint16_t status_version;     // 1 or 2
    std::uint16_t flags;             // kStatusDegraded
    std::uint64_t uptime_us;
    std::uint64_t frames_rx;
    std::uint64_t frames_tx;
    std::uint64_t frames_dropped;
    std::uint64_t crc_errors;
    std::uint64_t decode_errors;
    std::uint64_t reassembly_completed;
    std::uint64_t reassembly_timeouts;
    std::uint64_t reassembly_rejected;
    std::uint64_t command_duplicates;
    std::uint64_t telemetry_dropped;
};

// docs/commands.md section 5.2 -- one per (source_id, topic_id) tracked.
struct TopicStatusRecord {
    std::uint32_t source_id;
    std::uint16_t topic_id;
    std::uint16_t subscriber_count;
    std::uint32_t effective_rate_millihz;
    std::uint64_t bytes_total;
    std::uint64_t samples_dropped_total;
};

// ---------------------------------------------------------------------------
// Session negotiation (docs/session-and-terminal.md section 2)
// ---------------------------------------------------------------------------

struct EffectiveLimits {
    std::uint8_t selected_version;   // 0 = no common envelope version
    std::uint32_t max_logical_payload;
    std::uint16_t max_inflight_reassemblies;
    std::uint16_t max_subscriptions;
    std::uint32_t max_dedup_entries;
    std::uint32_t session_timeout_ms;
};

// Field-by-field minimum of `local` and `remote`, and the highest envelope
// version both announce. Pure: no state, no I/O. When the two share no
// version, selected_version is 0 and every limit is 0 (section 2.3).
//
// This is the peer-to-peer step only. A gateway on a path with a tighter
// limit than either peer (an ESP-NOW hop caps max_logical_payload) clamps the
// result further itself -- negotiate() has no view of the path.
EffectiveLimits negotiate(const Hello& local, const Hello& remote) noexcept;

// ===========================================================================
// Fixed-layout messages
// ===========================================================================
// decode_*: `payload` / `size` are the complete logical payload after
//   reassembly. On success `*out` is fully populated and any ByteView member
//   points into `payload`. On failure `*out` is untouched.
// encode_*: writes the logical payload for `in` into `out[0..capacity)` and
//   sets `*written`. Nothing is written on failure.
// Validation follows the order of docs/commands.md section 7: fixed portion,
// reserved fields, declared lengths, identifiers / versions / counts, nested
// records, protocol limits, then exact consumption of the payload.

MessageError decode_hello(const std::uint8_t* payload, std::size_t size, Hello* out) noexcept;
MessageError encode_hello(const Hello& in, std::uint8_t* out, std::size_t capacity, std::size_t* written) noexcept;

MessageError decode_hello_result(const std::uint8_t* payload, std::size_t size, HelloResult* out) noexcept;
MessageError encode_hello_result(const HelloResult& in, std::uint8_t* out, std::size_t capacity, std::size_t* written) noexcept;

MessageError decode_session_close(const std::uint8_t* payload, std::size_t size, SessionClose* out) noexcept;
MessageError encode_session_close(const SessionClose& in, std::uint8_t* out, std::size_t capacity, std::size_t* written) noexcept;

MessageError decode_session_close_result(const std::uint8_t* payload, std::size_t size, ControlResult* out) noexcept;
MessageError encode_session_close_result(const ControlResult& in, std::uint8_t* out, std::size_t capacity, std::size_t* written) noexcept;

MessageError decode_command_request(const std::uint8_t* payload, std::size_t size, CommandRequest* out) noexcept;
MessageError encode_command_request(const CommandRequest& in, std::uint8_t* out, std::size_t capacity, std::size_t* written) noexcept;

MessageError decode_command_result(const std::uint8_t* payload, std::size_t size, CommandResult* out) noexcept;
MessageError encode_command_result(const CommandResult& in, std::uint8_t* out, std::size_t capacity, std::size_t* written) noexcept;

MessageError decode_manifest_request(const std::uint8_t* payload, std::size_t size, ManifestRequest* out) noexcept;
MessageError encode_manifest_request(const ManifestRequest& in, std::uint8_t* out, std::size_t capacity, std::size_t* written) noexcept;

MessageError decode_subscribe(const std::uint8_t* payload, std::size_t size, Subscribe* out) noexcept;
MessageError encode_subscribe(const Subscribe& in, std::uint8_t* out, std::size_t capacity, std::size_t* written) noexcept;

MessageError decode_subscribe_result(const std::uint8_t* payload, std::size_t size, SubscribeResult* out) noexcept;
MessageError encode_subscribe_result(const SubscribeResult& in, std::uint8_t* out, std::size_t capacity, std::size_t* written) noexcept;

MessageError decode_unsubscribe(const std::uint8_t* payload, std::size_t size, Unsubscribe* out) noexcept;
MessageError encode_unsubscribe(const Unsubscribe& in, std::uint8_t* out, std::size_t capacity, std::size_t* written) noexcept;

MessageError decode_unsubscribe_result(const std::uint8_t* payload, std::size_t size, ControlResult* out) noexcept;
MessageError encode_unsubscribe_result(const ControlResult& in, std::uint8_t* out, std::size_t capacity, std::size_t* written) noexcept;

// STATUS (docs/commands.md section 5).
//
// decode_status fills `*base` from the 92-octet counter block. When the
// message is status_version 2, up to `topics_capacity` per-topic records are
// written to `topics` and `*topics_written` is set; a status_version 1
// message sets `*topics_written` to 0. A status_version 1 payload must be
// exactly 92 octets (section 5.1: the decoder stops at 92, trailing octets
// are not topic records). A status_version 2 payload must be exactly
// 92 + 2 + 28 * topic_status_count.
//
// When the message declares more records than `topics_capacity` holds,
// decode_status returns CountTooLarge and writes nothing (no partial output);
// size the buffer first with status_topic_count. The section 5.2
// "(source_id, topic_id) must not repeat" rule is a cross-record semantic
// check left to the caller, the same line drawn for cross-field semantics in
// the fixed messages above.
MessageError status_topic_count(const std::uint8_t* payload, std::size_t size,
                                std::uint16_t* count) noexcept;

MessageError decode_status(const std::uint8_t* payload, std::size_t size,
                           StatusV1* base,
                           TopicStatusRecord* topics, std::size_t topics_capacity,
                           std::size_t* topics_written) noexcept;

// encode_status_v1 writes status_version = 1, encode_status_v2 writes
// status_version = 2; the base.status_version field is ignored on encode.
MessageError encode_status_v1(const StatusV1& base, std::uint8_t* out,
                              std::size_t capacity, std::size_t* written) noexcept;
MessageError encode_status_v2(const StatusV1& base,
                              const TopicStatusRecord* topics, std::size_t topic_count,
                              std::uint8_t* out, std::size_t capacity,
                              std::size_t* written) noexcept;

// ===========================================================================
// MANIFEST_DATA: reader
// ===========================================================================
// A manifest can carry 1024 topics and 1024 actions, each with up to 256
// fields, so it is walked, not decoded into one struct. The top-level reader
// stays shallow -- header, source_info, topics, actions -- and hands each
// topic and action back with the RAW BYTES of its nested records, which the
// caller walks with the sub-readers below. Nesting is explicit in the types
// (a FieldRecordReader over a topic's fields vs an action's parameters),
// never in hidden reader state.
//
//   ManifestReader r(payload, size);
//   ManifestHeader h;  r.header(&h);
//   SourceInfoEntry si;
//   while (r.next_source_info(&si) == Step::Item) { ... }        // format 2
//   TopicRecord t;  ByteView field_bytes;
//   while (r.next_topic(&t, &field_bytes) == Step::Item) {
//       FieldRecordReader fr(field_bytes);
//       FieldRecord f;  ByteView enum_bytes;
//       while (fr.next(&f, &enum_bytes) == Step::Item) {
//           EnumEntryReader er(enum_bytes);
//           EnumEntry e;
//           while (er.next(&e) == Step::Item) { ... }
//       }
//   }
//   ActionRecord a;  ByteView params, results, errors;
//   while (r.next_action(&a, &params, &results, &errors) == Step::Item) { ... }
//   r.finish();                                        // exact consumption
//
// Every ByteView points into the payload buffer and is valid only while it
// is. next_source_info is optional: next_topic / next_action / finish skip
// over any source_info the caller did not iterate.
//
// A relay that caches a source's catalog and re-emits it unchanged (the
// dongle hub) never needs the decoded records -- it forwards the bytes.
// ManifestReader::raw_source_info / raw_records hand back the still-framed
// spans instead of the struct walk, and ManifestWriter::put_raw_source_info /
// put_raw_records splice them back without decomposing. See the block above
// each pair.

enum class ManifestStep : std::uint8_t { Item, End, Error };

class ManifestReader {
public:
    using Step = ManifestStep;

    ManifestReader(const std::uint8_t* payload, std::size_t size) noexcept;

    MessageError header(ManifestHeader* out) noexcept;
    Step next_source_info(SourceInfoEntry* out) noexcept;
    Step next_topic(TopicRecord* out, ByteView* field_records) noexcept;
    Step next_action(ActionRecord* out, ByteView* parameter_records,
                     ByteView* result_records, ByteView* error_entries) noexcept;
    MessageError finish() noexcept;

    // Verbatim relay path -- an alternative to the next_* walk, for a cache
    // that stores and re-emits a catalog without interpreting it. Call right
    // after header(), on their own reader, not mixed with next_* / finish().
    //
    // raw_source_info: the source_info block verbatim, info_count prefix
    //   included. A format-1 manifest has no block -- `*out` is then size 0.
    // raw_records: the topic-record run and the action-record run, each a
    //   ByteView spanning exactly header.topic_count / header.action_count
    //   record_size-framed records, with the payload consumed exactly
    //   (TrailingBytes otherwise). Either run may be empty.
    MessageError raw_source_info(ByteView* out) noexcept;
    MessageError raw_records(ByteView* topic_records,
                             ByteView* action_records) noexcept;

    MessageError error() const noexcept { return error_; }

private:
    void skip_source_info() noexcept;  // advance the cursor past any un-iterated source_info
    void skip_topics() noexcept;       // and past any un-iterated topic records

    const std::uint8_t* payload_;
    std::size_t size_;
    std::size_t cursor_;
    MessageError error_;
    std::uint8_t state_;
    std::uint16_t manifest_format_version_;
    std::uint16_t topic_count_;
    std::uint16_t action_count_;
    std::uint16_t topics_seen_;
    std::uint16_t actions_seen_;
    std::uint16_t source_info_left_;
};

// Walks a run of length-delimited field records: a topic's fields
// (`count` = topic.field_count), or an action's parameter or result field list.
// `enum_entries` on each Item is the raw byte run of that field's enum
// descriptors (walk with EnumEntryReader). End requires exactly `count`
// records and exact consumption of the run.
class FieldRecordReader {
public:
    using Step = ManifestStep;

    FieldRecordReader(ByteView run, std::uint16_t count) noexcept;
    Step next(FieldRecord* out, ByteView* enum_entries) noexcept;
    MessageError error() const noexcept { return error_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t cursor_;
    std::uint16_t left_;
    MessageError error_;
};

// Walks a run of enum descriptor entries (docs/commands.md section 3.8). The
// run is not length-delimited: `count` is field.enum_count, and End requires
// exactly that many entries with the run fully consumed.
class EnumEntryReader {
public:
    using Step = ManifestStep;

    EnumEntryReader(ByteView run, std::uint16_t count) noexcept;
    Step next(EnumEntry* out) noexcept;
    MessageError error() const noexcept { return error_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t cursor_;
    std::uint16_t left_;
    MessageError error_;
};

// Walks a run of action-specific error entries (docs/commands.md section 3.10),
// structurally identical to an enum run. `count` is the action's error_count.
class ActionErrorReader {
public:
    using Step = ManifestStep;

    ActionErrorReader(ByteView run, std::uint16_t count) noexcept;
    Step next(ActionError* out) noexcept;
    MessageError error() const noexcept { return error_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t cursor_;
    std::uint16_t left_;
    MessageError error_;
};

// ===========================================================================
// MANIFEST_DATA: builder
// ===========================================================================
// Write order mirrors the reader:
//   begin(header)                                   (header.topic_count /
//   add_source_info() * N        (format 2 only)     .action_count are the
//   for each topic:                                  totals finish() checks)
//     begin_topic(); add_field() [ add_enum() * ] * ; end_topic()
//   for each action:
//     begin_action();
//     add_action_param()  [ add_enum() * ] * ;
//     add_action_result() [ add_enum() * ] * ;
//     add_action_error()  * ;
//     end_action()
//   finish(&written)
//
// add_enum applies to the field last passed to add_field / add_action_param /
// add_action_result. A call out of this order is MessageError::WrongOrder.

class ManifestWriter {
public:
    ManifestWriter(std::uint8_t* out, std::size_t capacity) noexcept;

    MessageError begin(const ManifestHeader& header) noexcept;
    MessageError add_source_info(const SourceInfoEntry& entry) noexcept;

    // Verbatim relay path -- splice spans captured by
    // ManifestReader::raw_source_info / raw_records straight in, without
    // decomposing them. An alternative to add_source_info and the
    // begin_topic/add_field/... calls:
    //
    //   begin(header);                    // topic_count / action_count are
    //   put_raw_source_info(block);        //   upper bounds here, not exact
    //   put_raw_records(topics, actions);
    //   finish(&written);
    //
    // put_raw_source_info replaces the empty source_info block begin() opened
    // (format 2 only; `block` carries its own info_count and must be >= 2
    // octets). put_raw_records copies as many leading whole records as the
    // output capacity holds -- never a partial record -- then backpatches the
    // header topic_count / action_count to what actually landed. record_size
    // framing is re-validated; record contents are trusted.
    MessageError put_raw_source_info(ByteView block) noexcept;
    MessageError put_raw_records(ByteView topic_records,
                                ByteView action_records) noexcept;

    MessageError begin_topic(const TopicRecord& topic) noexcept;
    MessageError add_field(const FieldRecord& field) noexcept;
    MessageError add_enum(const EnumEntry& entry) noexcept;
    MessageError end_topic() noexcept;

    MessageError begin_action(const ActionRecord& action) noexcept;
    MessageError add_action_param(const FieldRecord& field) noexcept;
    MessageError add_action_result(const FieldRecord& field) noexcept;
    MessageError add_action_error(const ActionError& entry) noexcept;
    MessageError end_action() noexcept;

    MessageError finish(std::size_t* written) noexcept;

    // Octets emitted so far. Lets a caller that is packing a size-limited
    // response (reserve room for the records before writing source_info, say)
    // decide what still fits without a second pass. 0 before begin(); after a
    // failure it stops advancing, like every other call.
    std::size_t size() const noexcept { return cursor_; }

private:
    void close_source_info() noexcept;   // backpatch info_count
    void close_field() noexcept;         // backpatch the open field record_size, check enum count
    void open_action_errors() noexcept;  // close fields, write the error_count slot
    MessageError add_field_common(const FieldRecord& field) noexcept;
    // Copy leading whole records from `run` until the output is full; returns
    // false and leaves error_ == Ok when it stopped short on capacity, false
    // with error_ set on a framing fault, true when the whole run was copied.
    bool splice_record_run(ByteView run, std::size_t record_cap,
                           std::uint16_t* written) noexcept;

    std::uint8_t* out_;
    std::size_t capacity_;
    std::size_t cursor_;
    MessageError error_;
    std::uint8_t state_;
    std::uint16_t manifest_format_version_;
    std::uint16_t topic_count_;           // from the header
    std::uint16_t action_count_;
    std::uint16_t topics_written_;
    std::uint16_t actions_written_;
    std::uint16_t fields_written_;        // in the current topic
    std::uint16_t action_params_written_;
    std::uint16_t action_results_written_;
    std::uint16_t action_errors_written_;
    std::size_t record_start_;            // offset of the current topic/action record_size
    std::size_t field_record_start_;      // offset of the current field record_size
    std::size_t error_count_slot_;        // offset of the action error_count u16
    std::size_t topic_count_slot_;        // offset of the header topic_count u16 (put_raw_records backpatch)
    std::size_t action_count_slot_;       // offset of the header action_count u16
    bool field_open_;                     // a field record is open (enums may follow)
    bool source_info_open_;               // the info_count slot is reserved, not yet patched
    std::size_t source_info_slot_;
    std::uint16_t source_info_written_;
    std::uint16_t field_enum_count_;      // declared enum_count of the open field
    std::uint16_t enums_written_;         // add_enum calls since the field opened
    std::uint16_t topic_field_count_;     // declared field_count of the open topic
    std::uint16_t action_param_count_;    // declared parameter_field_count of the open action
    std::uint16_t action_result_count_;   // declared result_field_count of the open action
};

}  // namespace btp

#endif  // BTP_MESSAGES_HPP
