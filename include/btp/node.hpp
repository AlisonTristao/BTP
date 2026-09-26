#ifndef BTP_NODE_HPP
#define BTP_NODE_HPP

// The friendly facade: one object that is a BTP endpoint's transmit side
// (btp::Endpoint), its receive side (btp::Receiver) and -- opt-in -- its
// responder session (btp::Session), sharing one identity, one clock notion and
// one set of caller-owned buffers.
//
// btp::Endpoint + btp::Receiver + btp::Session already are the un-hand-rolled
// TX / RX / session layers. What stayed the integration's, and what every
// consumer (bally_OS, bally_dongle, TraceView) then wrote once by hand, is the
// WIRING between them: give the endpoint an identity, size and bind the
// reassembly storage, thread one `now_ms` through both, feed each decoded frame
// to the session before routing it, answer HELLO / SESSION_CLOSE through the
// same send path. btp::Node is that wiring written once.
//
// It adds NO wire field and holds NO new state of its own -- it forwards to the
// three objects it owns, which stay reachable (endpoint(), receiver(),
// session()) for a caller that wants a layer directly. Same guarantees as the
// rest of the library: no internal allocation, noexcept, no I/O, no clock of
// its own, no global state.
//
// EVERYTHING the node calls out to is a virtual method on btp::NodeConfig,
// an abstract class YOU inherit from once (see below) -- never std::function,
// never a bare ctx you cast back by hand at every call site. Internally the
// node still bridges into btp::Endpoint / btp::Receiver's own C-style
// function-pointer callbacks (they stay exactly as they always were); the
// bridging thunks live here, written once, so no caller ever writes one.
//
//   send()   REQUIRED -- one encoded frame -> your radio / UART / HID. A
//            receive-only node still implements it (return false).
//   clock()  optional (has_clock() false by default) -> millis since boot;
//            not overridden means you pass now_ms explicitly.
//   seal()   optional (has_seal() false by default) -- encrypt one logical
//            payload; not overridden means cleartext. The KEY lives in your
//            override, never in BTP.
//   open()   optional (has_open() false by default) -- decrypt one received
//            payload; not overridden means receive() hands back the sealed
//            bytes for you to open.
//
// OUT of scope, on purpose (docs/library.md chapter 11 still applies):
//   * everything past HELLO_RESULT on the initiator side -- MANIFEST_REQUEST
//     still needs an explicit request_manifest() call once connected(), a
//     subscription table, a command's retry/correlation. connect() (below)
//     is only the handshake (btp::SessionInitiator, library 2.14.0).
//   * ROUTING POLICY -- receive() hands back a whole logical message and its
//     header; whether an unrecognised one is relayed (a hub) or dropped (an
//     endpoint) is the one switch on top, the caller's.
//   * manifest storage, the subscription aggregator, the priority scheduler,
//     telemetry schema storage -- state the library keeps above the wire.
//   * key derivation / selection -- the body of your seal / open callbacks.
//   * LINK framing -- COBS, HID-report de-padding, a serial byte stream. A
//     caller that owns its framing decodes one whole frame itself (e.g. via
//     btp::SerialDecoder) and hands it to receive(const DecodedFrame&, ...);
//     Serial frames larger than one ESP-NOW payload are still a later cut.
//
// This is library 2.11.0 territory.

#include "btp/catalog.hpp"    // Catalog, CatalogTopic (brings btp/telemetry.hpp, btp/messages.hpp)
#include "btp/codec.hpp"      // Header, ByteView, MessageType, TransportLimits, kFlagEncrypted
#include "btp/endpoint.hpp"   // Endpoint, EndpointSealFn, LogicalMessage, kEndpointAeadTagSize
#include "btp/fragmentation.hpp"  // ReassemblySlot, ReassemblyStorage
#include "btp/receiver.hpp"   // Receiver, ReceivedMessage, ReceiveOutcome
#include "btp/session.hpp"    // Session, Hello, SessionEvent, kSessionMaxReplySize
#include "btp/subscription.hpp"  // SubscriptionTable, SubscriptionClient
#include "btp/telemetry.hpp"  // SampleReader

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace btp {

class Node;  // NodeTerminalFn below needs the name before the class itself.

// ---------------------------------------------------------------------------
// Links (library 2.47.0)
// ---------------------------------------------------------------------------
//
// A Node has one identity, one sequence counter and one served catalogue,
// and up to kMaxNodeLinks LINKS: independent paths to peers (an ESP-NOW
// radio, a TCP connection, a BLE central...). Link 0 is the NodeConfig the
// Node is built with; links 1.. are attached later (Node::attach_link()).
// Each link has its own send() / transport / key policy, reassembly,
// optional responder session and subscription table -- see NodeLinkSlot.
// Every request is answered on the link it arrived on.
static const std::size_t kMaxNodeLinks = 4U;

// "This link, as it was when I took this reference". The epoch is bumped by
// Node::reset_link() (a connection went away), so a reference taken before
// a reset no longer matches afterwards -- Node::link_current() / send_on()
// refuse it, and a reply that outlived its connection is dropped instead of
// reaching whoever connected next. Plain data, cheap to copy.
struct LinkRef {
    std::uint8_t link;
    std::uint32_t epoch;
};

// ---------------------------------------------------------------------------
// Callbacks the node calls out to (independent of NodeConfig -- these stay
// plain C-style function pointers: on_sample() / publish() / on_publish() /
// on_terminal() / Node::enable_commands() are all post-construction escape
// hatches on a live Node, not part of the NodeConfig an object is built
// with, so there is no "your class" to attach a virtual method to.)
// ---------------------------------------------------------------------------

// Called by receive() for one TELEMETRY sample of a topic the attached learn
// catalog knows. `reader` is positioned at the first field; pull values with
// reader.next() -- raw*scale+offset already applied -- and match them to
// topic.fields[i] / catalog.field_name(topic, i). Do not keep `reader` past the
// callback.
using NodeSampleFn = void (*)(void* ctx, const CatalogTopic& topic,
                              SampleReader& reader);

// Called by publish() to write the fields of one sample, in schema order, into
// an open SampleWriter (begin() already done): one put_f64 / put_i64 / put_u64
// / put_bool / put_null / put_array_* per field. Do not call begin() / finish().
using NodeFillFn = void (*)(void* ctx, SampleWriter& writer);

// Called by publish_named() the same way, with a NamedSampleWriter (catalog.hpp)
// instead: put("field_name", value) rather than one positional put_f64() per
// field, in schema order still, but checked against the name at each step.
using NodeNamedFillFn = void (*)(void* ctx, NamedSampleWriter& writer);

// Called by receive() for a TERMINAL frame -- see on_terminal() below.
// `node` is this same node, already mid-receive() -- send() / send_with()
// are fine to call from here, the same reentrant-from-inside-receive()
// pattern serve_subscribe() / serve_command() / emit_manifest() already
// use for their own replies, so a handler that talks back (the usual case:
// TERMINAL_IN answered with TERMINAL_OUT) needs nothing threaded in via
// `ctx` to reach it. `now_ms` is receive()'s own now, for a reply's
// timestamp. `header.object_id` is kTerminalIn or kTerminalOut (object_id
// namespace); `payload` is the opaque bytes as-is, no struct (docs/session-
// and-terminal.md section 7 -- that is TERMINAL's whole point). Do not keep
// `payload` past the callback -- same lifetime as ReceivedMessage::payload.
using NodeTerminalFn = void (*)(void* ctx, Node& node, const Header& header,
                                ByteView payload, std::uint64_t now_ms);

// One MANIFEST_DATA SUCCESS, as on_manifest() (library 2.48.0) hands it over --
// for ANY described source, not just one peer's catalogue the way
// learn_catalog() keeps it. A hub enumeration arrives as one call per source.
//
//   header          the response that just ARRIVED: described_source_id,
//                   described_boot_id, config_revision, source_uuid, role,
//                   flags. Always current, even when the topics come from the
//                   cache. Its source_name view points into `payload`.
//   payload         that response's whole payload. Walk its source_info with
//                   btp::ManifestReader -- source_info is not covered by
//                   config_revision (commands.md 3.12), so the fresh one
//                   rides even a NOT_MODIFIED.
//   topics_payload  a complete MANIFEST_DATA to read the TOPIC records from:
//                   `payload` itself for a full response; the cached copy
//                   when the peer answered NOT_MODIFIED (from_cache true).
//                   With no cache attached a NOT_MODIFIED is delivered with
//                   topics_payload == payload, which describes no topics --
//                   "keep what you have", as before.
//   from_cache      topics_payload is the cached manifest.
//   source_info_stale  set only by the skip-on-hello path
//                   (set_manifest_skip_on_hello()): nothing was asked on the
//                   wire, so header/payload are the CACHED ones -- only
//                   described_boot_id was refreshed from HELLO_RESULT -- and
//                   the source_info they carry is whatever was cached.
//   link            the link it arrived on.
//
// Every view is valid only during the callback.
struct NodeManifest {
    ManifestHeader header;
    ByteView payload;
    ByteView topics_payload;
    bool from_cache;
    bool source_info_stale;
    LinkRef link;
};

// Called by receive() for each MANIFEST_DATA SUCCESS -- see NodeManifest.
// `node` is this node, mid-receive(); sending from here is fine.
using NodeManifestFn = void (*)(void* ctx, Node& node, const NodeManifest& manifest);

// What the last non-SUCCESS MANIFEST_DATA said (NodeRx::ManifestRejected). A
// rejection describes no source on the wire (commands.md 3.2), so
// `target_source_id` is recovered from the request it answers -- 0 when the
// answer did not correlate with anything this node asked (target 0 was a
// full enumeration, or someone else's request).
struct NodeManifestOutcome {
    std::uint32_t target_source_id;
    std::uint8_t status;       // ResultStatus
    std::uint16_t error_code;  // ResultError
    bool correlated;
};

// Outstanding MANIFEST_REQUESTs a node remembers, to correlate an answer back
// to its target (above). A ring: the oldest is forgotten first, which only
// costs that answer's target_source_id -- a manifest itself is still
// delivered.
static const std::size_t kNodeManifestPendingSlots = 8U;

// Called by emit_status() once per actively-subscribed topic (see
// enable_status_topics() below) to fill the two counters this layer cannot
// know itself -- bytes actually put on the wire and samples dropped are the
// caller's own TX-path bookkeeping (the priority queue / staging slot this
// layer deliberately does not own, the same boundary publish() itself draws).
// `*bytes_total` / `*samples_dropped_total` start at 0; leave either alone to
// report 0. Do not keep `node` past the callback beyond what emit_status()
// itself already does (it is mid-send, the same reentrancy publish_named()
// allows from on_publish()).
using NodeStatusTopicFn = void (*)(void* ctx, Node& node, std::uint16_t topic_id,
                                   std::uint64_t* bytes_total,
                                   std::uint64_t* samples_dropped_total);

// One entry in the registry publish_subscribed_topics() walks: which topic,
// and the NodeNamedFillFn that fills a sample of it. Registered with
// on_publish(); plain data otherwise, no invariant of its own to protect --
// storage is bound by enable_publish_registry() (or, on StaticNode<>, already
// bound -- see its own comment).
struct PublishRegistration {
    std::uint16_t topic_id;
    NodeNamedFillFn fill;
    void* ctx;
};

// Filled by NodeActionFn (below) to describe how a COMMAND_REQUEST's action
// finished. Zero-initialise before your own fields matter -- status defaults
// to 0 (ResultStatus::Success), error_code to 0 (ResultError::None) and
// pending to false, so a handler that only has good news to report, right
// away, can leave every field alone.
struct NodeActionOutcome {
    std::uint8_t status;              // ResultStatus
    std::uint16_t error_code;         // ResultError
    const char* message;              // NUL-terminated UTF-8, optional (nullptr -> none)
    const std::uint8_t* result_data;  // optional action-defined result bytes
    std::size_t result_size;

    // true: the action is not finished yet -- every field above is ignored,
    // no COMMAND_RESULT goes out from this call. `ticket` (NodeActionFn's
    // last parameter) is already armed for this request; save a copy and
    // call Node::complete_command(ticket, outcome) once the action actually
    // finishes, from any task -- see NodeCommandTicket below. false (the
    // zero-initialised default) is the synchronous behavior this outcome
    // always had before pending existed: unchanged.
    bool pending;
};

// A COMMAND_REQUEST accepted for out-of-band completion -- see
// NodeActionOutcome::pending above. Handed to NodeActionFn already armed;
// opaque otherwise, copy it whole (it is a plain POD, cheap to copy) into
// wherever the async work tracks its own state, and pass it back to
// complete_command() unchanged once done. A default-constructed /
// zero-initialised ticket is not valid -- valid() says so, and
// complete_command() refuses it the same way it refuses a stale one used
// twice.
struct NodeCommandTicket {
    Header request;               // the original COMMAND_REQUEST's envelope
    std::uint16_t action_id;
    std::uint16_t action_version;
    std::size_t slot;             // this request's reserved DedupCache slot
    bool armed;
    // The link the request arrived on (2.47.0): complete_command() answers
    // there, and sends nothing if that link was reset in the meantime.
    LinkRef link;

    bool valid() const noexcept { return armed; }
};

// Called once for a Fresh COMMAND_REQUEST (btp::DedupCache has not seen this
// requester + sequence before). Two ways to answer:
//   SYNCHRONOUSLY (the original, "first cut" shape, still the common case):
//     fill `outcome` (pre-set to Success, no message, no result, not
//     pending) and return -- the node builds COMMAND_RESULT from it, sends
//     it, and records it so a retransmission of this exact request replays
//     the same bytes instead of running the action a second time.
//   ASYNCHRONOUSLY: set outcome->pending = true and return, having saved a
//     copy of `ticket`. Nothing is sent yet -- classify() already reports a
//     retransmission arriving in the meantime as DuplicateInFlight (dropped,
//     not re-executed), the exact same protection a slow synchronous call
//     gets for free, just held open longer. Once the real work finishes
//     (any task), call Node::complete_command(ticket, real_outcome) with
//     pending left false this time -- that call does what returning from
//     this one synchronously would have.
using NodeActionFn = void (*)(void* ctx, std::uint16_t action_id,
                              std::uint16_t action_version, ByteView parameters,
                              NodeActionOutcome* outcome,
                              const NodeCommandTicket& ticket);

// ---------------------------------------------------------------------------
// Configuration -- an abstract class YOU inherit from
// ---------------------------------------------------------------------------
//
// One class, one object, one lifetime: it holds this node's identity AND
// every callback the node calls out to, as named virtual methods instead of
// a bag of (function pointer, void* ctx) pairs. send() is the only one you
// MUST override -- everything else defaults to "this axis is off", exactly
// what leaving the matching field null meant before this class existed.
//
//   class RobotLink : public btp::NodeConfig {
//   public:
//       RobotLink(const AeadKey& key) : key_(key) {
//           source_id = 0x00CAFE01U;
//           boot_id   = 0x0000B001U;
//           transport = btp::kEspNowTransport;
//       }
//       bool send(const std::uint8_t* frame, std::size_t n) override {
//           return esp_now_send(peer_mac_, frame, n) == ESP_OK;
//       }
//       bool has_seal() const override { return true; }
//       bool seal(const Header& h, std::uint16_t n, const std::uint8_t* pt,
//                 std::uint8_t* out) override {
//           return aead_seal(key_, h, n, pt, out) == AeadError::Ok;
//       }
//       // has_open()/open() the same, if this node also receives.
//   private:
//       AeadKey key_;
//   };
//
//   RobotLink link(my_key);
//   btp::StaticNode<> node(link);   // node holds a REFERENCE to link -- see
//                                   // the constructor's own comment on
//                                   // ownership/lifetime below.
//
// A class that both inherits NodeConfig AND owns its Node as a member is the
// usual shape (example/receiver.cpp, example/sender.cpp): base-before-member
// construction order means `*this` is already a live NodeConfig by the time
// the Node member's constructor runs, and members outlive... survive their
// own destructor running BEFORE the base's, so the reference stays valid for
// the Node's entire lifetime with no manual bookkeeping. Don't call any
// method on the Node you're mid-constructing from your own mem-initializers
// or constructor body -- store, don't dispatch, same rule the Node's own
// constructor follows (see Node::Node's comment).
//
// NodeConfig is split in two since 2.47.0. NodeLink below is everything that
// belongs to ONE path to a peer -- how a frame leaves (send / transport) and
// the key policy on it (seal / open / accept_cleartext / reply_seal).
// NodeConfig adds what belongs to the node as a whole (identity, clock,
// terminal / command handlers). A single-link node sees no difference: it
// still inherits NodeConfig and overrides the same methods. A node with
// several links (Node::attach_link()) gives each extra link its own NodeLink.
class NodeLink {
public:
    virtual ~NodeLink() = default;

    // NOT "which transport" -- the node never touches your link. It is only
    // the frame/payload size ceiling the fragmenter targets, plus one policy
    // bit (allow_encrypted). Use one of the presets (kEspNowTransport 250/210,
    // kSerialTransport 4096/4056, kUsbHidTransport 62/22, the last with
    // allow_encrypted false) if your link is one of those three, or build
    // your own TransportLimits to fit any other link's MTU.
    TransportLimits transport{};

    // ---- REQUIRED ----
    // Hands one encoded frame to your radio / UART / HID. Needed to send()
    // or run a session. A receive-only node still implements this -- return
    // false unconditionally -- the same outcome an unset `send` produced
    // before this class existed.
    virtual bool send(const std::uint8_t* frame, std::size_t frame_size) = 0;

    // ---- OPTIONAL: every one of these defaults to "this axis is off",
    // exactly what leaving the matching NodeConfig field null used to mean.
    // Override has_X() alongside X() -- Node calls has_X() first and only
    // calls X() when it says true, so a subclass that has nothing to add can
    // leave both alone. ----

    // Encrypts ONE logical payload before it is fragmented; the mirror of
    // open() below. has_seal() false (the default) means send() is
    // cleartext. The KEY lives in your override, never in BTP.
    virtual bool has_seal() const noexcept { return false; }
    virtual bool seal(const Header& header, std::uint16_t payload_size,
                      const std::uint8_t* plaintext, std::uint8_t* out) {
        (void)header;
        (void)payload_size;
        (void)plaintext;
        (void)out;
        return false;
    }

    // Decrypts ONE reassembled logical payload -- the mirror of seal() above.
    // `header` is the canonical logical header the receiver hands back
    // (FRAGMENTED cleared, fragment_index 0, fragment_count 1, ENCRYPTED
    // still set). `sealed_size` includes the trailing 16-octet AEAD tag;
    // `out_plaintext` has room for `sealed_size - 16`. Return false (tag
    // mismatch, no key) and the node drops the message. has_open() false
    // (the default) means receive() hands back the sealed bytes for you to
    // open. The KEY lives here (real: RadioSeal / bally-seal, which selects
    // it from header.source_id), never in BTP.
    virtual bool has_open() const noexcept { return false; }
    virtual bool open(const Header& header, std::uint16_t sealed_size,
                      const std::uint8_t* sealed, std::uint8_t* out_plaintext) {
        (void)header;
        (void)sealed_size;
        (void)sealed;
        (void)out_plaintext;
        return false;
    }

    // Only consulted when has_open() is true: whether a completed logical
    // message that arrived WITHOUT the ENCRYPTED flag may still be routed.
    // The default (false) is fail-closed -- a node holding a key drops every
    // cleartext message (counted in stats().dropped_cleartext), so a peer
    // cannot sidestep the key by simply not setting ENCRYPTED. Override it to
    // allow the specific cleartext traffic a mixed link legitimately carries
    // (e.g. a hub's own unsealed control replies next to a robot's sealed
    // data). The session handshake (HELLO / HELLO_RESULT / SESSION_CLOSE /
    // SESSION_CLOSE_RESULT) never reaches this: it is answered before any key
    // exists, and stays cleartext on the wire. With has_open() false every
    // message is routed as it arrives, sealed or not, as always.
    virtual bool accept_cleartext(const Header& header) {
        (void)header;
        return false;
    }

    // Picks the seal for ONE automatic reply the node is about to send: a
    // SUBSCRIBE_RESULT / UNSUBSCRIBE_RESULT, a COMMAND_RESULT (a fresh one or
    // a DuplicateComplete replay), or a MANIFEST_DATA. `request_header` is
    // the ORIGINAL request's header -- source_id names who actually asked,
    // unlike the reply's own header, which always carries this node's own
    // identity, so this is the one place a hub-shaped node (several keys,
    // one per relayed peer or channel) can pick the matching key per reply
    // instead of one key for every automatic send. Leave both
    // `*out_seal`/`*out_seal_ctx` at their nullptr default (this method's own
    // default body already does) to fall through to has_seal()/seal(); set
    // `*out_seal` to send this ONE reply in the clear regardless, or to any
    // OTHER raw EndpointSealFn (not necessarily this object's own seal() --
    // that is what buys a hub several independently selectable keys without
    // several NodeConfig objects).
    //
    // Does NOT apply to send() / send_with() / publish() / publish_named()
    // -- those are the caller's own sends, already covered by send_with()'s
    // explicit seal argument -- nor to an INITIATOR's own outgoing requests
    // (connect() / subscribe() / command() and their renewals), which have
    // no "original request" to classify by and always use
    // has_seal()/seal().
    virtual void reply_seal(const Header& request_header, EndpointSealFn* out_seal,
                            void** out_seal_ctx) {
        (void)request_header;
        *out_seal = nullptr;
        *out_seal_ctx = nullptr;
    }

    // Whether this node's OWN MANIFEST_REQUEST to `target_source_id`
    // (request_manifest() / request_manifest_cached(), and the re-request the
    // manifest cache issues after an eviction) is sealed with has_seal() /
    // seal() -- true, the default -- or sent in the clear. The case for false
    // (library 2.48.0): a client behind a hub whose manifests are answered by
    // the HUB's own manifest cache, which does not hold the end-to-end key
    // the rest of that client's traffic is sealed with.
    virtual bool seal_manifest_request(std::uint32_t target_source_id) {
        (void)target_source_id;
        return true;
    }
};

// The link a Node is built with (link 0) plus everything that belongs to the
// node as a whole -- see NodeLink's own comment above for the split.
class NodeConfig : public NodeLink {
public:
    // Identity. Both non-zero (BTP reserves 0 for each); source_id is usually
    // derived from the MAC, boot_id changes every reboot. Plain data, not
    // behaviour -- set in your constructor (or any time before begin(), see
    // above), no override needed. One identity for every link of the node.
    std::uint32_t source_id = 0U;
    std::uint32_t boot_id = 0U;

    // millis since boot -- millis() / esp_timer_get_time() / 1000 on an MCU,
    // QElapsedTimer::elapsed() under Qt, a counter in a test. May be read
    // from a timer ISR that increments a word; the node's own methods must
    // NOT be called from an ISR (btp::Receiver / btp::Session are not
    // internally synchronised, by design). has_clock() false (the default)
    // means you pass now_ms explicitly to receive() / tick() / etc.
    virtual bool has_clock() const noexcept { return false; }
    virtual std::uint64_t clock() { return 0U; }

    // Called by receive() for a TERMINAL_IN / TERMINAL_OUT frame -- see
    // Node::on_terminal()'s own comment for the parameters. has_terminal()
    // false (the default) means the frame comes back as NodeRx::Complete for
    // the caller to route by hand, same as before this class existed.
    // Node::on_terminal() stays reachable to set or replace a handler after
    // construction, independent of this.
    virtual bool has_terminal() const noexcept { return false; }
    virtual void terminal(Node& node, const Header& header, ByteView payload,
                          std::uint64_t now_ms) {
        (void)node;
        (void)header;
        (void)payload;
        (void)now_ms;
    }

    // Same, plus the link the frame arrived on (2.47.0) -- a node with
    // several links answers TERMINAL_IN on the link it came from
    // (Node::send_on(from, ...)). The default forwards to terminal() above,
    // so a single-link node overrides only that one, as before.
    virtual void terminal_on(Node& node, const LinkRef& from, const Header& header,
                             ByteView payload, std::uint64_t now_ms) {
        (void)from;
        terminal(node, header, payload, now_ms);
    }

    // Manifest cache (library 2.48.0) -- WHERE a consumer keeps the
    // manifests it learned, so the next session asks with
    // known_config_revision and gets a ~60-octet NOT_MODIFIED instead of the
    // whole catalogue (Node::request_manifest_cached()). The node decides
    // WHEN to read and write; your override decides where (a file, NVS, an
    // SD card, RAM). has_manifest_cache() false (the default) turns the whole
    // thing off: nothing is read or stored, exactly as before this existed.
    // Only consulted on a node that consumes manifests (on_manifest() or
    // learn_catalog() attached).
    //
    // What is stored is the raw payload of a complete MANIFEST_DATA SUCCESS
    // -- the wire format IS the serialisation, there is nothing else to
    // define. Key it by source_id; manifest_store() also passes the decoded
    // header (config_revision, source_uuid, source_name...) for an index.
    //
    // manifest_load(): point *out at the stored payload and return true, or
    // return false when there is none. The bytes must stay valid until the
    // next manifest_* call on this config or until receive() returns,
    // whichever comes first. The node validates what comes back (a complete
    // SUCCESS describing `cached_source_id`) and evicts anything else as a miss.
    virtual bool has_manifest_cache() const noexcept { return false; }
    virtual bool manifest_load(std::uint32_t cached_source_id, ByteView* out) {
        (void)cached_source_id;
        (void)out;
        return false;
    }
    virtual void manifest_store(std::uint32_t cached_source_id, const ManifestHeader& header,
                                ByteView payload) {
        (void)cached_source_id;
        (void)header;
        (void)payload;
    }
    // The stored entry is unusable -- a NOT_MODIFIED answered a revision it
    // no longer matches, a different source_uuid answered for the same
    // source_id, or it failed to parse. The node re-requests the full
    // manifest right after.
    virtual void manifest_evict(std::uint32_t cached_source_id) { (void)cached_source_id; }

    // Mutual exclusion for the state every link shares (2.47.0): the seal /
    // manifest / command scratch buffers, the command dedup cache and the
    // frames_tx counter. The node calls lock() / unlock() around every use
    // of them. Only needed when several links are driven from different
    // tasks / threads (each link's own receive path stays one context, as
    // always); the default no-op is right for everyone else. MUST be
    // recursive: a command handler already running under it may send.
    virtual void lock() {}
    virtual void unlock() {}

    // Runs a Fresh COMMAND_REQUEST's action -- synchronously or not, see
    // NodeActionFn's own comment for the parameters (this is that same
    // signature). has_command() false (the default) means COMMAND_REQUEST
    // goes unanswered. Unlike terminal() above, this ONLY takes effect on a
    // StaticNode<> -- it owns the DedupCache this needs and wires it from
    // here in its own constructor; a bare Node has no such storage to bind
    // it to, and ignores this -- call Node::enable_commands(cache, handler,
    // ctx) yourself there instead, as always.
    virtual bool has_command() const noexcept { return false; }
    virtual void command(std::uint16_t action_id, std::uint16_t action_version,
                         ByteView parameters, NodeActionOutcome* outcome,
                         const NodeCommandTicket& ticket) {
        (void)action_id;
        (void)action_version;
        (void)parameters;
        (void)outcome;
        (void)ticket;
    }
};

// ---------------------------------------------------------------------------
// The outcome of one receive()
// ---------------------------------------------------------------------------
//
// Five things a node caller acts on, collapsed from btp::ReceiveOutcome (7) and
// btp::SessionEvent (7). The full detail stays in receiver().stats() and
// session_event().

enum class NodeRx : std::uint8_t {
    Complete,        // *out is a whole logical message the node did not consume -- route it.
    Pending,         // a fragment was stored or a duplicate absorbed -- nothing yet.
    SessionHandled,  // a HELLO / SESSION_CLOSE / session timeout -- the node already
                     // replied (if a reply was due) through `send`. See session_event().
    InitiatorHandled,  // connect()'s HELLO_RESULT arrived (accepted or rejected),
                       // or the connection watchdog timed out. See initiator_event().
    SubscriptionServed,  // a SUBSCRIBE / UNSUBSCRIBE against this node's own
                         // subscriptions() table -- the node already replied.
    SubscriptionHandled,  // a SUBSCRIBE_RESULT for a subscribe()/renew() this
                          // node holds. See subscription_event().
    CommandServed,   // a COMMAND_REQUEST against commands() -- the node ran the
                     // action (or replayed / rejected it); already replied,
                     // UNLESS the action marked its outcome pending (2.42.0) --
                     // then nothing was sent yet, see NodeActionOutcome::pending.
    CommandHandled,  // a COMMAND_RESULT for a command() this node holds. See
                     // command_outcome().
    CatalogUpdated,  // a MANIFEST_DATA -- the node ingested it into the learn catalog.
    SampleDelivered, // a TELEMETRY sample -- the node decoded it and called on_sample.
    TerminalDelivered,  // a TERMINAL_IN / TERMINAL_OUT frame -- the node called
                        // on_terminal(). Without one attached, this falls through
                        // to Complete instead, same as any other unmanaged type.
    RequestServed,   // a MANIFEST_REQUEST -- the node built and sent MANIFEST_DATA.
    Ignored,         // a frame the node would manage but cannot yet: a session not
                     // Active, or a sample for a topic the catalog has not learned.
    DroppedFrame,    // btp::decode / reassembly / a malformed managed payload rejected it.
                     // Counts are in receiver().stats().
    NoDatagram,      // routine() was called with nothing that arrived this pass --
                     // receive() did not run, but the housekeeping still did.
    ManifestHandled,   // a MANIFEST_DATA SUCCESS -- the node called on_manifest().
    ManifestRejected,  // a non-SUCCESS MANIFEST_DATA on a node that consumes
                       // manifests. See manifest_outcome().
};

const char* node_rx_string(NodeRx rx) noexcept;

// ---------------------------------------------------------------------------
// NodeLinkSlot -- the per-link state of a Node
// ---------------------------------------------------------------------------
//
// Everything a Node keeps for ONE link: the reassembly (btp::Receiver and the
// buffers a completed message is copied / opened into), the optional
// responder session, the subscription table peers on this link subscribe
// through, and the per-link counters. Opaque -- only Node reads or writes it.
// A Node owns the slot of its link 0 itself; StaticNode<> owns the slots of
// its extra links. The buffers stay the caller's, same rules as the Node
// constructor's own rx_buffer / open_buffer.
class NodeLinkSlot {
public:
    NodeLinkSlot(ReassemblySlot* slot_array, const ReassemblyStorage* storage,
                 std::size_t slot_count, std::uint64_t reassembly_timeout_ms,
                 const TransportLimits& transport, std::uint8_t* rx_buffer,
                 std::size_t rx_capacity, std::uint8_t* open_buffer,
                 std::size_t open_capacity) noexcept;

    NodeLinkSlot(const NodeLinkSlot&) = delete;
    NodeLinkSlot& operator=(const NodeLinkSlot&) = delete;

private:
    friend class Node;

    NodeLink* cfg_;  // nullptr until the link is attached (link 0: always set)
    Receiver receiver_;
    std::uint8_t* rx_buffer_;
    std::size_t rx_capacity_;
    std::uint8_t* open_buffer_;
    std::size_t open_capacity_;
    ReceiveOutcome last_receive_outcome_;  // see Node::receive_outcome()
    Session session_;       // placeholder HELLO until enable_session()
    bool session_on_;
    SessionEvent last_session_event_;
    std::uint32_t session_path_dropped_crc_;
    std::uint32_t session_path_dropped_decode_;
    std::uint32_t dropped_cleartext_;
    std::uint32_t dropped_open_failed_;
    SubscriptionTable* subscriptions_;  // nullptr until enable_subscriptions()
    // Bumped by Node::reset_link(). Atomic: read by whoever holds a LinkRef
    // (a task finishing a command) while the link's own task resets it.
    std::atomic<std::uint32_t> epoch_;
};

// ---------------------------------------------------------------------------
// Node
// ---------------------------------------------------------------------------
//
//   class RobotLink : public btp::NodeConfig { ... };  // send() required,
//                                                       // has_seal()/seal()
//                                                       // etc. opt-in --
//                                                       // see NodeConfig's
//                                                       // own comment
//   RobotLink link;
//   link.source_id = source_id;
//   link.boot_id = boot_id;
//   link.transport = btp::kEspNowTransport;
//   btp::StaticNode<> node(link);
//   if (!node.begin()) { /* bad identity or storage */ }
//
//   // producer:
//   node.send(btp::MessageType::Telemetry, 0x0101, body, body_size, now_us());
//
//   // consumer, opt-in responder session:
//   node.enable_session(local_hello, /*hello_deadline_ms=*/0);
//   node.arm_session();
//   for (;;) {
//       btp::ReceivedMessage msg{};
//       if (node.receive(datagram, n, &msg) == btp::NodeRx::Complete)
//           route(msg.header, msg.payload);          // the caller's one switch
//       node.tick();                                  // session watchdog + sweep
//   }
//
// ONE context calls send / receive / tick (as btp::Receiver / btp::Session
// require). A timer may pace tick() -- but by raising a flag the loop reads,
// never by calling tick() from the ISR.

class Node {
public:
    // The rest of the arguments are caller-owned storage, exactly as
    // btp::Receiver / btp::Endpoint expect it:
    //   slots / storage / slot_count / reassembly_timeout_ms  -> btp::Receiver
    //   rx_buffer / rx_capacity        the buffer receive() copies a completed
    //                                  logical message into (>= slot capacity)
    //   seal_scratch / seal_scratch_cap  the sealed copy a FRAGMENTED encrypted
    //                                  send is cut from; may be {nullptr, 0}
    //                                  when `cfg.has_seal()` is false or
    //                                  messages never fragment
    //   open_buffer / open_capacity    where `cfg.open()` writes the plaintext
    //                                  (>= slot capacity); may be {nullptr, 0}
    //                                  when `cfg.has_open()` is false
    //   scratch_buffer / scratch_capacity  where serve_catalog() builds a
    //                                  MANIFEST_DATA and publish() a sample;
    //                                  may be {nullptr, 0} for a node that does
    //                                  neither
    // `cfg` itself is stored by REFERENCE, not copied -- it must outlive the Node
    // (the usual shape: your class inherits NodeConfig and holds the Node as
    // its own member, see NodeConfig's own comment for why that is safe with
    // no manual lifetime bookkeeping). This is also what replaces the old
    // reconfigure() method: because identity/transport/callbacks are read
    // from `cfg` live, at the point of each call, a caller whose identity is
    // only known after something else exists just mutates its own
    // source_id/boot_id/transport fields (a plain assignment, no Node method
    // involved) any time before begin() -- no placeholder-then-reconfigure
    // dance needed.
    // slot_array, not slots: see Reassembler's own constructor comment
    // (btp/fragmentation.hpp) on the Qt <QObject> macro collision.
    Node(NodeConfig& cfg, ReassemblySlot* slot_array,
         const ReassemblyStorage* storage, std::size_t slot_count,
         std::uint64_t reassembly_timeout_ms, std::uint8_t* rx_buffer,
         std::size_t rx_capacity, std::uint8_t* seal_scratch,
         std::size_t seal_scratch_cap, std::uint8_t* open_buffer,
         std::size_t open_capacity, std::uint8_t* scratch_buffer,
         std::size_t scratch_capacity) noexcept;

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    // endpoint.configure() + receiver.valid() + (session enabled?
    // session.valid()). Check once at boot, and again if you mutate identity/
    // transport on your NodeConfig later (see the constructor's own comment).
    // A missing `send` is not possible any more (NodeConfig::send() is
    // required) -- a receive-only node's override simply returns false in
    // place, same outcome as before.
    //
    // `arm_and_announce` (default false -- every existing begin() call keeps
    // meaning exactly what it always did) additionally arm_session()s any
    // enabled session and announce_catalog()s any served catalogue, once
    // begin() itself succeeds -- the "just go live" case a producer with
    // nothing to wait for wants in one call. Leave it false for the OTHER
    // case: a session armed only on some later trigger of your own (a
    // console's ENTER line, say) -- arm_session() stays yours to call then.
    // A false return is still only about identity/storage; a failed
    // announce_catalog() here is silent, best-effort, same as calling it
    // separately.
    bool begin(bool arm_and_announce = false) noexcept;

    // The INITIATOR's analogue of the overload above: begin() (identity /
    // storage only, no session to arm, no catalogue to announce -- this node
    // is the one connecting out), then connect(local_hello, deadline_ms) in
    // the same call. False means either failed -- check connected() /
    // initiator_event() if you need to tell which.
    bool begin(const Hello& local_hello, std::uint64_t connect_deadline_ms) noexcept;
    bool configured() const noexcept;

    std::uint32_t source_id() const noexcept { return cfg_.source_id; }
    std::uint32_t boot_id() const noexcept { return cfg_.boot_id; }

    // ---- transmit ----------------------------------------------------------
    // Encodes `payload` as one or more frames and hands each to `cfg.send()`.
    // A fresh sequence is reserved. `cfg.has_seal()` seals once over the
    // whole payload before fragmenting; a false from `cfg.seal()` sends
    // nothing (fail-closed). Returns "did the whole logical message go out".
    //
    // Frames never exceed 250 octets (the ESP-NOW ceiling), whatever the
    // transport: on a TCP / Serial / BLE node a payload larger than that goes
    // out as several fragments the receiver reassembles (2.46.0 -- before, a
    // payload past ~200 octets on those transports made send() fail).
    bool send(MessageType type, std::uint16_t object_id,
              const std::uint8_t* payload, std::size_t size,
              std::uint64_t timestamp_us) noexcept;

    // Same, with a seal for THIS message (a hub that seals channel C to a robot
    // and channel B to a desktop with different keys). A null `seal` forces
    // cleartext regardless of cfg.has_seal()/cfg.seal().
    bool send_with(MessageType type, std::uint16_t object_id,
                   const std::uint8_t* payload, std::size_t size,
                   std::uint64_t timestamp_us, EndpointSealFn seal,
                   void* seal_ctx) noexcept;

    // ---- receive ---------------------------------------------------------
    // Feed one already-delimited datagram (ESP-NOW / USB HID deliver these
    // whole). Sweeps stale partials, decodes, checks CRC, reassembles; with a
    // session enabled it also runs the HELLO handshake, renews the watchdog and
    // answers SESSION_CLOSE (through `cfg.send()`) before a frame is routed. On
    // NodeRx::Complete, *out holds the whole logical message -- its payload
    // copied into rx_buffer (or open_buffer, if `cfg.open()` decrypted it),
    // valid until the next receive().
    //
    // The no-now overload reads `cfg.clock()` when `cfg.has_clock()`; without
    // one it behaves as now = 0 (fine for a session-less consumer, wrong for
    // the watchdog).
    NodeRx receive(const std::uint8_t* datagram, std::size_t size,
                   ReceivedMessage* out) noexcept;
    NodeRx receive(const std::uint8_t* datagram, std::size_t size,
                   std::uint64_t now_ms, ReceivedMessage* out) noexcept;

    // Same, for a frame something ELSE already decoded -- a caller that owns
    // the link framing itself (COBS / a serial byte stream fed through
    // btp::SerialDecoder, a HID report de-padder) and wants btp::Node for the
    // session / reassembly / discovery wiring above one whole BTP frame. Skips
    // btp::decode() (`frame` came from a decoder that already validated the
    // envelope and CRC); everything past that -- the initiator, the responder
    // session, reassembly, open(), and every discovery / subscription /
    // command / terminal outcome -- is identical to the datagram overloads.
    // stats().session_path_dropped_* are not touched here (that decoder counts
    // its own).
    NodeRx receive(const DecodedFrame& frame, ReceivedMessage* out) noexcept;
    NodeRx receive(const DecodedFrame& frame, std::uint64_t now_ms,
                   ReceivedMessage* out) noexcept;

    // The btp::Receiver-level detail behind the most recent receive()'s
    // NodeRx::Pending / NodeRx::DroppedFrame -- same idea as session_event()
    // for the session path, one level further down. NodeRx collapses
    // ReceiveOutcome's seven values into two (Pending covers
    // FragmentAccepted/DuplicateFragment; DroppedFrame covers
    // DroppedCrc/DroppedDecode/DroppedReassembly/InvalidArgument) because
    // most callers only need "stored, nothing yet" vs "rejected" -- but a
    // caller with its own per-reason bookkeeping (a hub clearing a pending
    // relay reservation only on DroppedReassembly, say, not on every drop)
    // needs the original reason, not Node's rounder one. Meaningless (stale,
    // from whatever receive() last actually reached btp::Receiver) when the
    // most recent NodeRx was anything else -- a session/initiator/
    // subscription/command/catalog/terminal outcome never touches this.
    ReceiveOutcome receive_outcome() const noexcept { return link0_.last_receive_outcome_; }

    // ---- session responder (opt-in) -------------------------------------
    // `local` is this peer's HELLO advertisement (role, versions, limits,
    // peer_uuid, config_revision). `hello_deadline_ms` bounds the wait for the
    // peer's HELLO after arm_session() (2000 for a serial console; 0 disables
    // that bound -- only the negotiated watchdog then applies). Copied in.
    void enable_session(const Hello& local,
                        std::uint64_t hello_deadline_ms) noexcept;
    bool session_enabled() const noexcept { return link0_.session_on_; }

    // Idle -> AwaitingHello. Call once the link is up (or after a console ENTER
    // line). No-op if no session is enabled.
    void arm_session() noexcept;
    void arm_session(std::uint64_t now_ms) noexcept;

    // Call from the loop / a timer. Sweeps reassembly timeouts and, with a
    // session, polls its watchdog. Returns the session event (TimedOut when a
    // dead session is noticed; None otherwise / no session).
    SessionEvent tick() noexcept;
    SessionEvent tick(std::uint64_t now_ms) noexcept;

    // The session event the most recent receive() / tick() produced.
    SessionEvent session_event() const noexcept { return link0_.last_session_event_; }

    // ---- several links (2.47.0) ---------------------------------------------
    // Everything above without a link argument is link 0 (the NodeConfig the
    // node was built with) -- a single-link node never needs this section.
    //
    // Links 1..kMaxNodeLinks-1 are attached with their own NodeLink and
    // storage; StaticNode<..., Links> owns that storage and has an
    // attach_link(link, cfg, timeout) overload that needs none. Attaching
    // (re)configures the slot for cfg.transport / reassembly_timeout_ms and
    // resets it. Returns false for link 0, an index past kMaxNodeLinks or a
    // slot whose receiver does not validate (bad transport / storage).
    // `subscriptions` (optional) is the table SUBSCRIBE / UNSUBSCRIBE on this
    // link are served from; nullptr leaves them unanswered on this link.
    bool attach_link(std::uint8_t link, NodeLink& cfg, NodeLinkSlot& slot,
                     std::uint64_t reassembly_timeout_ms,
                     SubscriptionTable* subscriptions = nullptr) noexcept;
    bool link_attached(std::uint8_t link) const noexcept;

    // receive() for one link: decode, reassemble, that link's session and
    // open(), then the same managed outcomes -- every reply leaves through
    // this link. NodeRx::DroppedFrame for a link that is not attached. The
    // initiator (connect()) lives on link 0 only.
    NodeRx receive_on(std::uint8_t link, const std::uint8_t* datagram,
                      std::size_t size, std::uint64_t now_ms,
                      ReceivedMessage* out) noexcept;
    NodeRx receive_on(std::uint8_t link, const DecodedFrame& frame,
                      std::uint64_t now_ms, ReceivedMessage* out) noexcept;

    // enable_session() / arm_session() for one link: only frames on THIS link
    // wait for a HELLO -- the others keep routing as before.
    void enable_session_on(std::uint8_t link, const Hello& local,
                           std::uint64_t hello_deadline_ms) noexcept;
    void arm_session_on(std::uint8_t link, std::uint64_t now_ms) noexcept;
    const Session* link_session(std::uint8_t link) const noexcept;
    SessionEvent link_session_event(std::uint8_t link) const noexcept;

    // The link's peer went away: drops its partial reassemblies, returns its
    // session (if enabled) to Idle -- arm it again for the next peer --,
    // frees its subscriptions, zeroes its counters and bumps its epoch so
    // every LinkRef taken before this call stops being current. The command
    // dedup cache is NOT touched: it is scoped to this node's boot, not to a
    // connection (docs/session-and-terminal.md section 5.3).
    void reset_link(std::uint8_t link) noexcept;

    // {link, its current epoch}; {link, 0} for a link that is not attached.
    LinkRef link_ref(std::uint8_t link) const noexcept;
    bool link_current(const LinkRef& ref) const noexcept;

    // send_with() through one link, refused (nothing sent) when `to` is not
    // current any more -- see LinkRef.
    bool send_on(const LinkRef& to, MessageType type, std::uint16_t object_id,
                 const std::uint8_t* payload, std::size_t size,
                 std::uint64_t timestamp_us, EndpointSealFn seal,
                 void* seal_ctx) noexcept;

    // The subscription table peers on this link subscribe through (nullptr
    // when the link has none). Link 0's is subscriptions().
    SubscriptionTable* link_subscriptions(std::uint8_t link) noexcept;
    const SubscriptionTable* link_subscriptions(std::uint8_t link) const noexcept;

    // ---- session initiator (opt-in via connect()) -------------------------
    // The OTHER end of a session from enable_session() above: this node
    // connects OUT to a peer instead of accepting a connection. No enable_*
    // call needed -- connect() itself is the opt-in (Idle until then), and
    // both may be live on the same node at once (a hub bridging two links).
    // btp::SessionInitiator (session.hpp) is the state machine; this is its
    // wiring into the same send / receive / tick pipeline as the responder.
    //
    //   node.connect(my_hello, robot_addr_deadline_ms);
    //   for (;;) {
    //       if (node.receive(datagram, n, &msg) == btp::NodeRx::InitiatorHandled &&
    //           node.initiator_event() == btp::InitiatorEvent::Connected) {
    //           // node.effective_limits() / node.connected_peer_source_id() set
    //       }
    //       node.tick();   // TimedOut if HELLO_RESULT (or the peer) goes quiet
    //   }

    // Idle -> AwaitingResult. Sends `local` as a HELLO (needs cfg.send) and
    // waits up to `deadline_ms` for HELLO_RESULT (docs/session-and-terminal.md
    // section 5.1: 2000 for a serial console; 0 disables the bound). A no-op
    // (returns false) outside Idle, on a malformed `local`, or on a send
    // failure -- call disconnect() or wait for InitiatorHandled first.
    bool connect(const Hello& local, std::uint64_t deadline_ms) noexcept;
    bool connect(const Hello& local, std::uint64_t now_ms,
                std::uint64_t deadline_ms) noexcept;

    bool connected() const noexcept;
    const EffectiveLimits& effective_limits() const noexcept;
    std::uint32_t connected_peer_source_id() const noexcept;
    std::uint32_t connected_peer_boot_id() const noexcept;
    // HELLO_RESULT's own config_revision -- see SessionInitiator::
    // peer_config_revision()'s own comment on why it is not part of
    // EffectiveLimits.
    std::uint32_t connected_peer_config_revision() const noexcept;
    // HELLO_RESULT's peer_uuid -- 16 octets, all zero until connected. The
    // peer's stable identity (session-and-terminal.md 1.1), unlike
    // connected_peer_source_id(), which the spec does not promise is stable.
    const std::uint8_t* connected_peer_uuid() const noexcept;

    // Sends SESSION_CLOSE and tears the connection down locally right away
    // (does not wait for SESSION_CLOSE_RESULT). A no-op outside AwaitingResult
    // / Active.
    bool disconnect(std::uint8_t reason, std::uint32_t drain_timeout_ms) noexcept;
    bool disconnect(std::uint64_t now_ms, std::uint8_t reason,
                    std::uint32_t drain_timeout_ms) noexcept;

    // The initiator event the most recent receive() / tick() produced.
    InitiatorEvent initiator_event() const noexcept { return last_initiator_event_; }

    // ---- subscriptions (opt-in, docs/commands.md section 4) ---------------
    // btp::SubscriptionTable / btp::SubscriptionClient (subscription.hpp) are
    // the two halves, state above the wire same as the session layer; the
    // storage stays the caller's (attach it, don't build it into the node --
    // a hub aggregating subscriptions across peers needs its own shape here).

    // RESPONDER: grant subscriptions on THIS node's served catalogue (needs
    // serve_catalog() too -- the table validates against it). nullptr detaches.
    void enable_subscriptions(SubscriptionTable* table) noexcept {
        link0_.subscriptions_ = table;
    }
    SubscriptionTable* subscriptions() noexcept { return link0_.subscriptions_; }
    const SubscriptionTable* subscriptions() const noexcept {
        return link0_.subscriptions_;
    }

    // INITIATOR: hold subscriptions on a peer. nullptr detaches.
    void enable_subscription_client(SubscriptionClient* client) noexcept {
        subscription_client_ = client;
    }
    SubscriptionClient* subscription_client() noexcept { return subscription_client_; }

    // Sends SUBSCRIBE to (peer_source_id, peer_boot_id) for one topic; tick()
    // renews it on its own before the granted lease runs out. Returns a
    // local id (!= 0) that names it across renewals -- pass to unsubscribe()
    // -- or 0 (no subscription client attached, no cfg.send, no free slot).
    std::uint32_t subscribe(std::uint32_t peer_source_id, std::uint32_t peer_boot_id,
                            std::uint16_t topic_id, std::uint32_t rate_millihz,
                            std::uint32_t lease_ms) noexcept;

    // Sends UNSUBSCRIBE for a subscribe()-returned id and drops it locally
    // right away (does not wait for UNSUBSCRIBE_RESULT).
    bool unsubscribe(std::uint32_t local_id) noexcept;

    // The subscription event the most recent receive() / tick() produced.
    SubscriptionEvent subscription_event() const noexcept {
        return last_subscription_event_;
    }

    // The full outcome behind subscription_event() -- mirrors command_outcome():
    // local_id/peer/topic/requested_rate always meaningful when event is
    // Granted or Rejected, plus effective_rate_millihz (Granted) or
    // status/error_code (Rejected), straight off SUBSCRIBE_RESULT with no
    // separate decode. See btp::SubscriptionOutcome's own comment.
    const SubscriptionOutcome& subscription_outcome() const noexcept {
        return last_subscription_outcome_;
    }

    // ---- commands (opt-in, docs/commands.md section 2) ---------------------
    // btp::DedupCache (session.hpp) is the RESPONDER's memory -- execute an
    // action once, remember the result, replay a retransmission instead of
    // running it again. btp::CommandClient (session.hpp) is the INITIATOR's:
    // send, correlate the eventual COMMAND_RESULT, time out with no retry
    // budget if none comes. Storage stays the caller's, same reasoning as
    // subscriptions -- the caller sizes the cache for the largest request /
    // result it will ever see.

    // RESPONDER: `cache` deduplicates; `handler` runs a Fresh request,
    // synchronously or not (NodeActionFn above). nullptr for either detaches.
    void enable_commands(DedupCache* cache, NodeActionFn handler, void* ctx) noexcept {
        commands_ = cache;
        on_command_ = handler;
        on_command_ctx_ = ctx;
    }
    DedupCache* commands() noexcept { return commands_; }
    const DedupCache* commands() const noexcept { return commands_; }

    // Finishes a command NodeActionFn marked outcome->pending (library
    // 2.42.0) -- builds and sends the same COMMAND_RESULT a synchronous
    // return would have, from `ticket` (still armed) and `outcome` (pending
    // now ignored either way). Safe to call from a different task than the
    // one that ran receive() -- ticket.slot names a DedupCache slot, and
    // classify()/record_result() are the only two calls not synchronised
    // with each other by the library itself (same note as always: a caller
    // running RX on another thread already needs its own critical section
    // around the pair). Returns false, and sends nothing: !ticket.valid()
    // (default-constructed, or already completed once -- a ticket is spent
    // the first time this succeeds, calling it again on the same one hits
    // this the same way an out-of-range slot would) or enable_commands()
    // was never called.
    bool complete_command(const NodeCommandTicket& ticket,
                          const NodeActionOutcome& outcome) noexcept;

    // INITIATOR: hold outstanding commands on a peer. nullptr detaches.
    void enable_command_client(CommandClient* client) noexcept {
        command_client_ = client;
    }
    CommandClient* command_client() noexcept { return command_client_; }

    // Sends COMMAND_REQUEST to (peer_source_id, peer_boot_id). Returns a
    // local id (!= 0) that names the outcome across command_outcome() /
    // NodeRx::CommandHandled, or 0 (no command client attached, no cfg.send,
    // no free slot, or a request encode_command_request() rejects).
    std::uint32_t command(std::uint32_t peer_source_id, std::uint32_t peer_boot_id,
                          std::uint16_t action_id, std::uint16_t action_version,
                          const std::uint8_t* parameters,
                          std::size_t parameters_size) noexcept;

    // The command event the most recent receive() / tick() produced, and its
    // full detail on Completed (status / error_code / message / result --
    // message / result view the same buffer ReceivedMessage::payload does,
    // valid only until the next receive()).
    const CommandOutcome& command_outcome() const noexcept {
        return last_command_outcome_;
    }

    // ---- terminal (opt-in) --------------------------------------------------
    // TERMINAL has no other Node support -- no enable_*() storage, no serve/
    // learn split. `callback` runs for EITHER direction (kTerminalIn AND
    // kTerminalOut both reach it -- check header.object_id if this node only
    // expects one) and receive() reports NodeRx::TerminalDelivered instead of
    // Complete. nullptr detaches -- back to Complete, same as before this
    // existed. Sending stays plain send(MessageType::Terminal, kTerminalIn /
    // kTerminalOut, ...); there is no terminal-specific send sugar either.
    void on_terminal(NodeTerminalFn callback, void* ctx) noexcept {
        on_terminal_ = callback;
        on_terminal_ctx_ = ctx;
    }

    // ---- STATUS (opt-in, docs/commands.md section 5) -----------------------
    // Periodically builds and sends a v1 STATUS from counters the node
    // already tracks: receiver().stats(), frames_tx() (below), and --
    // once attached -- commands()'s replay count. Spontaneous, like the wire
    // format itself -- no result, no correlation. Best-effort, not exact:
    // frames_rx / reassembly_completed both read receiver().stats().completed
    // (a REASSEMBLED LOGICAL message, the closest counter this layer keeps to
    // "a frame"), and frames_tx only counts sends through send() / send_with()
    // -- the bootstrap traffic (HELLO, SUBSCRIBE, COMMAND_REQUEST and their
    // early-path replies) is not in it. telemetry_dropped is not tracked
    // separately yet and reads 0. v2 (per-topic TopicStatusRecord) is not
    // built yet -- only v1.
    //
    // period_ms == 0 disables it (the default, and what enable_status(0)
    // returns to). tick() sends one whenever at least period_ms has passed
    // since the last -- call tick() more often than period_ms for a timely
    // report, the same "cadence bounds how late it's noticed" rule as the
    // session watchdog. The first report goes out one period after
    // enable_status(), not immediately.
    void enable_status(std::uint32_t period_ms) noexcept;
    bool status_enabled() const noexcept { return status_period_ms_ != 0U; }

    // ---- STATUS v2 per-topic block (opt-in, library 2.41.0) ----------------
    // Upgrades emit_status()'s output from plain v1 to v2 (docs/commands.md
    // section 5.2): one TopicStatusRecord per topic of the SERVED catalog
    // (serve_catalog()) that currently has at least one active subscriber
    // (enable_subscriptions()) -- source_id (this node's own),
    // subscriber_count and effective_rate_millihz all come from
    // subscriptions_ directly, the same introspection calls a hand-rolled
    // reporter would otherwise duplicate; `callback` is asked only for the
    // two fields genuinely outside this layer's own state (see
    // NodeStatusTopicFn above). At most kMaxStatusTopics topics are reported
    // per emission, in the served catalog's own order -- a catalog with more
    // than that concurrently subscribed has the rest silently left out of
    // THIS message (no error, no partial-record marker; commands.md places
    // no lower bound on how many records one STATUS carries).
    //
    // Falls back to v1 with no code on either side needing to know: no
    // callback attached, no served catalog, no subscription table, or simply
    // nothing subscribed at emit time. nullptr `callback` detaches.
    static constexpr std::size_t kMaxStatusTopics = 8U;
    void enable_status_topics(NodeStatusTopicFn callback, void* ctx) noexcept {
        on_status_topics_ = callback;
        on_status_topics_ctx_ = ctx;
    }

    // Logical messages sent through send() / send_with() since construction
    // (see the STATUS note above for what this does not count).
    std::uint64_t frames_tx() const noexcept { return frames_tx_; }

    // ---- discovery: consumer side (opt-in) --------------------------------
    // Attach a catalogue for the node to keep current from MANIFEST_DATA. Once
    // set, receive() ingests every MANIFEST_DATA frame into it (returning
    // NodeRx::CatalogUpdated) instead of handing it back, and -- with on_sample
    // set -- decodes TELEMETRY samples against the learned schema. `catalog`
    // stays the caller's; pass nullptr to detach.
    void learn_catalog(Catalog* catalog) noexcept;
    const Catalog* learned_catalog() const noexcept { return learn_catalog_; }

    // Send a MANIFEST_REQUEST. `known_config_revision` 0 asks for the complete
    // manifest; a non-zero value the responder already published gets a
    // NOT_MODIFIED reply (which ingest() treats as "keep what I have").
    // target_source_id 0 is a full-catalog request. Needs `cfg.send`.
    bool request_manifest(std::uint32_t target_source_id,
                          std::uint32_t target_boot_id,
                          std::uint32_t known_config_revision) noexcept;

    // Consumer of MANY sources (library 2.48.0): called for every MANIFEST_DATA
    // SUCCESS, whichever source it describes -- see NodeManifest. With this
    // set, receive() consumes MANIFEST_DATA (NodeRx::ManifestHandled /
    // ManifestRejected) instead of handing it back as Complete. Coexists with
    // learn_catalog() (both are fed). nullptr detaches.
    void on_manifest(NodeManifestFn callback, void* ctx) noexcept {
        on_manifest_ = callback;
        on_manifest_ctx_ = ctx;
    }

    // request_manifest() with known_config_revision filled in from the
    // manifest cache (NodeConfig::manifest_load()) -- 0 on a miss or with no
    // cache, which asks for the whole thing. The answer then arrives through
    // on_manifest() / learn_catalog() either way: a full one (stored to the
    // cache) or a NOT_MODIFIED (delivered with the cached topics).
    // target_source_id 0 (a full enumeration) never uses the cache --
    // known_config_revision names ONE source's revision (commands.md 3.1);
    // ask each known source by id instead when you already know who exists.
    //
    // With set_manifest_skip_on_hello(true), a request aimed at the peer this
    // node is connect()ed to, whose HELLO_RESULT config_revision equals the
    // cached one, sends NOTHING: on_manifest() runs right here, from the
    // cache, with source_info_stale set. Returns true when a request went out
    // or the cache answered.
    bool request_manifest_cached(std::uint32_t target_source_id,
                                 std::uint32_t target_boot_id) noexcept;

    // Opt-in, default false -- and false is the recommendation: the
    // NOT_MODIFIED a skipped request would have fetched costs ~60 octets and
    // is the only thing that refreshes source_info (firmware version, running
    // partition...), which config_revision does not cover. Turn it on only
    // when that round trip matters more than current source_info.
    void set_manifest_skip_on_hello(bool skip) noexcept { manifest_skip_on_hello_ = skip; }
    bool manifest_skip_on_hello() const noexcept { return manifest_skip_on_hello_; }

    // The last NodeRx::ManifestRejected. See NodeManifestOutcome.
    const NodeManifestOutcome& manifest_outcome() const noexcept {
        return last_manifest_outcome_;
    }

    // Called by receive() for a TELEMETRY sample of a topic the learn catalogue
    // knows. Without this, a TELEMETRY frame comes back as NodeRx::Complete for
    // the caller to decode itself.
    void on_sample(NodeSampleFn callback, void* ctx) noexcept;

    // ---- discovery: producer side (opt-in) -------------------------------
    // Attach a catalogue the node SERVES: on a MANIFEST_REQUEST for this source
    // (or a full-catalog request) receive() builds a MANIFEST_DATA from it and
    // sends it (NodeRx::RequestServed), honouring known_config_revision with a
    // NOT_MODIFIED reply (compared against catalog->config_revision(), read
    // live). `source_uuid` is 16 octets (nullptr -> zero), `role` a btp::Role
    // (0 -> Producer), `source_name` NUL-terminated (nullptr -> ""); all three
    // go in the manifest header. Needs `cfg.send` and a scratch buffer. The
    // same Catalog may also be the learn catalogue. nullptr detaches.
    void serve_catalog(Catalog* catalog, std::uint8_t role,
                       const std::uint8_t* source_uuid,
                       const char* source_name) noexcept;

    // Send the served catalogue as an UNSOLICITED MANIFEST_DATA (request
    // reference zero) -- a producer broadcasting its schema on boot so a
    // late-joining consumer needs no MANIFEST_REQUEST. Needs a served catalogue,
    // `cfg.send` and a scratch buffer.
    bool announce_catalog() noexcept;

    // Encode one sample of `topic_id` -- must be a topic in the SERVED catalogue
    // -- and send it as TELEMETRY. `fill` writes the field values in schema
    // order into the SampleWriter. Returns false for an unknown topic, a fill
    // that under/over-runs the schema, or a send failure.
    bool publish(std::uint16_t topic_id, NodeFillFn fill, void* ctx,
                 std::uint64_t timestamp_us) noexcept;

    // Same, checked by field NAME instead of position -- see
    // catalog.hpp's NamedSampleWriter for what this buys over publish().
    bool publish_named(std::uint16_t topic_id, NodeNamedFillFn fill, void* ctx,
                       std::uint64_t timestamp_us) noexcept;

    // publish() / publish_named(), sealed with `seal` for THIS sample instead
    // of cfg.seal() -- the publish-side mirror of send_with() (a producer
    // whose TELEMETRY key differs from the one cfg.seal() carries for
    // automatic replies, the dual-key hub case NodeConfig::reply_seal()
    // covers on the reply side). A null `seal` forces cleartext regardless of
    // cfg.has_seal()/cfg.seal(), same rule send_with() already follows.
    bool publish_with(std::uint16_t topic_id, NodeFillFn fill, void* ctx,
                      std::uint64_t timestamp_us, EndpointSealFn seal,
                      void* seal_ctx) noexcept;
    bool publish_named_with(std::uint16_t topic_id, NodeNamedFillFn fill,
                            void* ctx, std::uint64_t timestamp_us,
                            EndpointSealFn seal, void* seal_ctx) noexcept;

    // ---- publish-on-subscribe (opt-in) --------------------------------------
    // Ties publish_named() to the SubscriptionTable's own due()/note_published()
    // cadence, so a caller with several topics does not hand-write "for each
    // topic: if due(), fill and publish, then note_published()" once per
    // topic -- see example/sender.cpp for the walkthrough. StaticNode<> already
    // has storage for this (below), ready with no setup beyond on_publish()
    // calls; a raw Node needs enable_publish_registry() first.

    // Points the registry at caller-owned storage (StaticNode<> calls this on
    // your behalf). nullptr / 0 detaches -- on_publish() then fails closed and
    // publish_subscribed_topics() is a no-op, same "opt-in, safe when unset"
    // rule as subscriptions_ / commands_.
    void enable_publish_registry(PublishRegistration* slot_array,
                                 std::size_t slot_count) noexcept;

    // Registers `fill` as the callback that fills a sample of `topic_id` for
    // publish_subscribed_topics() below. `topic_id` should already be a topic
    // in the served catalogue (serve_catalog()) -- publish_subscribed_topics()
    // silently skips a registration whose topic is not (or no longer) there,
    // the same "unknown topic, nothing sent" rule publish_named() itself
    // already has. Returns false: no registry attached (enable_publish_registry()
    // first, or use a StaticNode<>, which already has one), a null `fill`, or
    // the registry is full.
    bool on_publish(std::uint16_t topic_id, NodeNamedFillFn fill,
                    void* ctx) noexcept;

    // Call once per loop pass, in place of your own due() / publish_named() /
    // note_published() dance: walks every on_publish()-registered topic, and
    // for each one due() against the attached SubscriptionTable
    // (enable_subscriptions()), calls publish_named() and note_published().
    // Returns how many were actually published -- 0 with no registry
    // attached, no SubscriptionTable attached, or nothing due right now.
    std::size_t publish_subscribed_topics(std::uint64_t now_ms) noexcept;

    // Call once per loop pass with whatever the link produced this pass --
    // see the loop in example/sender.cpp / receiver.cpp, now down to one
    // call. `size` == 0 (nothing arrived) skips receive() and returns
    // NodeRx::NoDatagram; otherwise this IS receive(datagram, size, now_ms,
    // out), same return value. Either way, publish_subscribed_topics(now_ms)
    // and tick(now_ms) run right after: due topics get sent (a no-op with
    // nothing registered, no SubscriptionTable, or nothing due), then the
    // session/connection watchdog and subscription lease renewal (each a
    // no-op when not enabled). The SAME call for a producer, a consumer, or
    // a node that is both -- the parts that don't apply to this node were
    // already no-ops before this existed.
    NodeRx routine(const std::uint8_t* datagram, std::size_t size,
                   std::uint64_t now_ms, ReceivedMessage* out) noexcept;

    // Same, for the common case a Complete message's *out details go
    // nowhere -- every OTHER NodeRx outcome already ran its own callback
    // (on_sample / on_publish / on_terminal / ...) before returning, so a
    // caller with one attached for everything it cares about has nothing
    // left to read from *out. Routines through the four-argument overload
    // above with a throwaway ReceivedMessage -- kept, not replaced, for a
    // caller that DOES want Complete's payload (an unmanaged message type,
    // e.g. one this library adds support for later but an older caller
    // does not yet handle through a callback).
    NodeRx routine(const std::uint8_t* datagram, std::size_t size,
                   std::uint64_t now_ms) noexcept;

    // The housekeeping alone, no datagram involved -- for a caller whose
    // receive() runs elsewhere (its own task/thread/ISR) and only wants
    // publish_subscribed_topics() + tick() from this call.
    void routine(std::uint64_t now_ms) noexcept;

    // ---- escape hatches -----------------------------------------------------
    Endpoint& endpoint() noexcept { return endpoint_; }
    const Endpoint& endpoint() const noexcept { return endpoint_; }
    Receiver& receiver() noexcept { return link0_.receiver_; }
    const Receiver& receiver() const noexcept { return link0_.receiver_; }
    Session* session() noexcept {
        return link0_.session_on_ ? &link0_.session_ : nullptr;
    }
    const Session* session() const noexcept {
        return link0_.session_on_ ? &link0_.session_ : nullptr;
    }
    // Always present (Idle until connect()), unlike session() above.
    SessionInitiator& initiator() noexcept { return initiator_; }
    const SessionInitiator& initiator() const noexcept { return initiator_; }

    struct Stats {
        Receiver::Stats rx;  // stats(): link 0's; link_stats(): that link's
        // btp::decode failures seen on the session path (the DecodedFrame the
        // session needs is decoded here, not inside btp::Receiver, so these
        // would otherwise go uncounted).
        std::uint32_t session_path_dropped_crc;
        std::uint32_t session_path_dropped_decode;
        // Completed messages dropped because has_open() is true and they
        // arrived without ENCRYPTED, with accept_cleartext() refusing them.
        std::uint32_t dropped_cleartext;
        // Completed ENCRYPTED messages dropped because open() failed (bad
        // tag, no key for this sender) or the sealed size did not fit.
        std::uint32_t dropped_open_failed;
    };
    Stats stats() const noexcept;
    Stats link_stats(std::uint8_t link) const noexcept;

private:
    std::uint64_t resolve_now(std::uint64_t fallback) const noexcept;
    // Every method below that takes a NodeLinkSlot& works on THAT link: its
    // receiver / session / subscriptions, and every reply it sends leaves
    // through that link's own send() / seal policy -- a request is always
    // answered on the link it arrived on.
    //
    // The shared tail of every receive() overload: feed one already-decoded
    // frame to the initiator, then the responder session, then reassembly, and
    // finish() the outcome. The datagram overloads btp::decode() into a
    // DecodedFrame first; the DecodedFrame overloads skip straight here.
    NodeRx receive_datagram(NodeLinkSlot& link, const std::uint8_t* datagram,
                            std::size_t size, std::uint64_t now_ms,
                            ReceivedMessage* out) noexcept;
    NodeRx receive_frame(NodeLinkSlot& link, const DecodedFrame& frame,
                         std::uint64_t now_ms, ReceivedMessage* out) noexcept;
    NodeRx route_decoded(NodeLinkSlot& link, const DecodedFrame& decoded,
                         std::uint64_t now_ms, ReceivedMessage* out) noexcept;
    NodeRx finish(NodeLinkSlot& link, ReceiveOutcome outcome, ReceivedMessage* out,
                  std::uint64_t now_ms) noexcept;
    // One logical message out through `link` -- the one place Endpoint's
    // send_logical() is called with a link's send() / transport.
    bool transmit(NodeLinkSlot& link, MessageType type, std::uint16_t object_id,
                  const std::uint8_t* payload, std::size_t size,
                  std::uint64_t timestamp_us, EndpointSealFn seal,
                  void* seal_ctx) noexcept;
    bool transmit_reserved(NodeLinkSlot& link, std::uint32_t sequence,
                           MessageType type, std::uint16_t object_id,
                           const std::uint8_t* payload, std::size_t size,
                           std::uint64_t timestamp_us) noexcept;
    // link.cfg_->reply_seal()'s pick, or its has_seal()/seal() when it
    // leaves *out_seal null -- see NodeLink::reply_seal()'s own comment.
    // Every automatic-reply send site below calls this once, right before
    // transmit(), instead of using the link's seal() directly.
    void reply_seal_for(NodeLinkSlot& link, const Header& request,
                        EndpointSealFn* out_seal, void** out_seal_ctx) const noexcept;
    void serve_manifest(NodeLinkSlot& link, const Header& request,
                        ByteView payload) noexcept;
    void emit_manifest(NodeLinkSlot& link, const Header& request,
                       const RequestRef& reply_to, std::uint8_t status,
                       std::uint8_t flags, std::uint16_t error_code,
                       bool with_topics) noexcept;
    void serve_subscribe(NodeLinkSlot& link, const Header& request, ByteView payload,
                         std::uint64_t now_ms) noexcept;
    void serve_unsubscribe(NodeLinkSlot& link, const Header& request,
                           ByteView payload) noexcept;
    void drain_subscription_renewals(std::uint64_t now_ms) noexcept;
    void serve_command(NodeLinkSlot& link, const Header& request,
                       ByteView payload) noexcept;
    // Returns false, sending nothing, when commands_->record_result() itself
    // refuses `slot` (WrongOrder -- already completed by an earlier call, the
    // way a spent/reused NodeCommandTicket would; or InvalidArgument -- an
    // out-of-range index) -- complete_command()'s guard against a stale or
    // repeated ticket. The Fresh path in serve_command() below never sees
    // false in practice (its slot is always freshly Reserved), but checks
    // the same way for the one code path either caller goes through.
    // `deliver` false records the result for replay but sends nothing -- the
    // link the request came from was reset meanwhile (complete_command()).
    bool emit_command_result(NodeLinkSlot& link, const Header& request,
                             std::uint16_t action_id, std::uint16_t action_version,
                             const NodeActionOutcome& outcome, std::size_t slot,
                             bool deliver = true) noexcept;
    bool publish_named_on(NodeLinkSlot& link, std::uint16_t topic_id,
                          NodeNamedFillFn fill, void* ctx,
                          std::uint64_t timestamp_us, EndpointSealFn seal,
                          void* seal_ctx) noexcept;
    void emit_command_reject(NodeLinkSlot& link, const Header& request,
                             std::uint16_t action_id, std::uint16_t action_version,
                             ResultStatus status, ResultError error) noexcept;
    void emit_status(std::uint64_t now_ms) noexcept;

    // Bridges from NodeLink / NodeConfig's virtual methods into the plain
    // C-style function pointers btp::Endpoint / btp::Receiver themselves take
    // (they never changed -- only what calls them did). `ctx` is a NodeLink*
    // for send/seal (the link the frame leaves through) and a NodeConfig*
    // for terminal/command (this node's own cfg_, or -- for command_thunk, the
    // only one a subclass needs -- a StaticNode<>'s own cfg_ reached through
    // its own protected access). Written once, here, so no caller ever
    // writes one of these.
    static bool send_thunk(void* ctx, const std::uint8_t* frame,
                           std::size_t frame_size) noexcept;
    static bool seal_thunk(void* ctx, const Header& header,
                           std::uint16_t payload_size,
                           const std::uint8_t* plaintext,
                           std::uint8_t* out) noexcept;
    // No open_thunk: a link's open() is never handed to Endpoint/Receiver as a
    // callback -- finish() below calls it directly (a plain virtual
    // dispatch), unlike send()/seal(), which Endpoint calls internally
    // mid-fragmentation and therefore needs bridged to its own C-style
    // EndpointSendFn/EndpointSealFn.
    static void terminal_thunk(void* ctx, Node& node, const Header& header,
                               ByteView payload, std::uint64_t now_ms) noexcept;

  protected:
    // StaticNode<>'s own constructor (a template, defined inline below) needs
    // this one to wire cfg.has_command()/cfg.command() into enable_commands()
    // -- see NodeConfig::command()'s own comment on why only StaticNode<> can
    // do this. Not part of the public API; a bare Node ignores cfg.command
    // entirely, same as always.
    static void command_thunk(void* ctx, std::uint16_t action_id,
                              std::uint16_t action_version, ByteView parameters,
                              NodeActionOutcome* outcome,
                              const NodeCommandTicket& ticket) noexcept;

  private:
    // link.cfg_->has_seal() ? &seal_thunk : nullptr, and the matching ctx --
    // centralise the has_seal() check every send-a-message call site needs.
    static EndpointSealFn current_seal(const NodeLinkSlot& link) noexcept;
    static void* current_seal_ctx(const NodeLinkSlot& link) noexcept;

    NodeLinkSlot* slot(std::uint8_t link) noexcept;
    const NodeLinkSlot* slot(std::uint8_t link) const noexcept;
    static LinkRef ref_of(std::uint8_t link, const NodeLinkSlot& s) noexcept;
    std::uint8_t index_of(const NodeLinkSlot& link) const noexcept;
    void deliver_terminal(NodeLinkSlot& link, const Header& header,
                          ByteView payload, std::uint64_t now_ms) noexcept;
    // One MANIFEST_REQUEST out on link 0, remembered in manifest_pending_.
    bool send_manifest_request(std::uint32_t target_source_id,
                               std::uint32_t target_boot_id,
                               std::uint32_t known_config_revision) noexcept;
    // The pending target a MANIFEST_DATA answers, forgotten once found.
    bool take_manifest_pending(const RequestRef& ref,
                               std::uint32_t* target_source_id) noexcept;
    // A validated cache entry for `source_id`, or false (evicting a bad one).
    bool load_cached_manifest(std::uint32_t source_id, ByteView* payload,
                              ManifestHeader* header) noexcept;
    void deliver_manifest(const NodeManifest& manifest) noexcept;
    NodeRx consume_manifest(NodeLinkSlot& link, const ReceivedMessage& msg) noexcept;

    NodeConfig& cfg_;
    std::uint8_t* seal_scratch_;
    std::size_t seal_scratch_cap_;
    std::uint8_t* scratch_buffer_;
    std::size_t scratch_capacity_;

    Endpoint endpoint_;
    NodeLinkSlot link0_;  // the link cfg_ describes
    NodeLinkSlot* links_[kMaxNodeLinks];  // [0] == &link0_, the rest attach_link()

    SessionInitiator initiator_;  // Idle until connect()
    InitiatorEvent last_initiator_event_;

    SubscriptionClient* subscription_client_;  // nullptr until enable_subscription_client()
    SubscriptionEvent last_subscription_event_;
    SubscriptionOutcome last_subscription_outcome_;

    DedupCache* commands_;         // nullptr until enable_commands()
    NodeActionFn on_command_;
    void* on_command_ctx_;
    CommandClient* command_client_;  // nullptr until enable_command_client()
    CommandOutcome last_command_outcome_;

    NodeTerminalFn on_terminal_;    // nullptr until on_terminal()
    void* on_terminal_ctx_;

    std::uint64_t frames_tx_;
    std::uint32_t status_period_ms_;  // 0 == disabled
    std::uint64_t status_last_ms_;
    std::uint64_t status_started_ms_;
    NodeStatusTopicFn on_status_topics_;  // nullptr until enable_status_topics()
    void* on_status_topics_ctx_;

    Catalog* learn_catalog_;
    NodeSampleFn on_sample_;
    void* on_sample_ctx_;

    NodeManifestFn on_manifest_;  // nullptr until on_manifest()
    void* on_manifest_ctx_;
    bool manifest_skip_on_hello_;
    NodeManifestOutcome last_manifest_outcome_;
    struct ManifestPending {
        std::uint32_t sequence;
        std::uint32_t target_source_id;
        bool used;
    };
    ManifestPending manifest_pending_[kNodeManifestPendingSlots];
    std::size_t manifest_pending_next_;

    Catalog* serve_catalog_;
    std::uint8_t serve_role_;
    std::uint8_t serve_uuid_[16];
    const char* serve_name_;

    PublishRegistration* publish_slots_;  // nullptr until enable_publish_registry()
    std::size_t publish_slot_capacity_;
    std::size_t publish_slot_count_;
};

// ---------------------------------------------------------------------------
// StaticNode -- Node that owns its buffers (the common embedded case)
// ---------------------------------------------------------------------------

// 4000 ms, matching bally_OS RxRouter / bally_dongle ProtocolRouter: how long a
// half-arrived message holds a slot before the sweep reclaims it.
static const std::uint64_t kNodeDefaultReassemblyTimeoutMs = 4000U;

namespace detail {

template <std::size_t Slots, std::size_t SlotBytes, std::size_t SealBytes,
          std::size_t ScratchBytes>
struct NodeStorage {
    // slot_array, not slots -- see Reassembler's constructor comment
    // (btp/fragmentation.hpp) on the Qt <QObject> macro collision; this one is
    // a MEMBER, not just a parameter, so getting it wrong would silently drop
    // the field from a Qt-macro-active build entirely, not just rename it.
    ReassemblySlot slot_array[Slots];
    std::uint8_t storage_bytes[Slots][SlotBytes];
    ReassemblyStorage storage[Slots];
    std::uint8_t rx_buffer[SlotBytes];
    std::uint8_t open_buffer[SlotBytes];
    std::uint8_t seal_scratch[SealBytes];
    std::uint8_t scratch_buffer[ScratchBytes];

    NodeStorage() noexcept {
        for (std::size_t i = 0; i < Slots; ++i) {
            storage[i].data = storage_bytes[i];
            storage[i].capacity = SlotBytes;
        }
    }
};

// Same "wire the byte pointers in my own constructor" reasoning as
// NodeStorage above, its own small type so it finishes constructing --
// storage[] pointing at bytes[], DedupCache-ready -- before the NEXT member
// in StaticNode's declaration order (commands_cache_) starts building.
// DedupCache::DedupCache reads storage[i].data / .capacity EAGERLY, once,
// and latches valid() from that snapshot -- a plain command_storage_[i] a
// StaticNode constructor BODY fills in later is too late; by then
// commands_cache_ already copied a null.
template <std::size_t MaxCommands, std::size_t CommandBytes>
struct CommandStorage {
    std::uint8_t bytes[MaxCommands][CommandBytes];
    DedupStorage storage[MaxCommands];

    CommandStorage() noexcept {
        for (std::size_t i = 0; i < MaxCommands; ++i) {
            storage[i].data = bytes[i];
            storage[i].capacity = CommandBytes;
        }
    }
};

// The storage of ONE extra link of a StaticNode<..., Links> (2.47.0): its
// reassembly slots and buffers, its subscription table and the NodeLinkSlot
// over them. Same sizes as link 0's -- a StaticNode's links are symmetric.
// Everything is wired in this constructor, member by member in declaration
// order, before the slot that points into it is built; the transport and
// timeout are only known at Node::attach_link(), which reconfigures them.
template <std::size_t Slots, std::size_t SlotBytes, std::size_t MaxSubscriptions>
struct ExtraLinkStorage {
    ReassemblySlot slot_array[Slots];
    std::uint8_t storage_bytes[Slots][SlotBytes];
    ReassemblyStorage storage[Slots];
    std::uint8_t rx_buffer[SlotBytes];
    std::uint8_t open_buffer[SlotBytes];
    SubscriptionRecord subscription_slots[MaxSubscriptions];
    SubscriptionTable subscriptions;
    NodeLinkSlot link;

    static ReassemblyStorage* wire(ReassemblyStorage* s,
                                   std::uint8_t (*bytes)[SlotBytes]) noexcept {
        for (std::size_t i = 0; i < Slots; ++i) {
            s[i].data = bytes[i];
            s[i].capacity = SlotBytes;
        }
        return s;
    }

    ExtraLinkStorage() noexcept
        : slot_array(),
          storage_bytes(),
          storage(),
          rx_buffer(),
          open_buffer(),
          subscription_slots(),
          subscriptions(subscription_slots, MaxSubscriptions),
          link(slot_array, wire(storage, storage_bytes), Slots,
               kNodeDefaultReassemblyTimeoutMs, TransportLimits{}, rx_buffer,
               SlotBytes, open_buffer, SlotBytes) {}
};

// N extra links; the N == 0 specialisation (Links == 1, every existing
// StaticNode) holds nothing.
template <std::size_t N, std::size_t Slots, std::size_t SlotBytes,
          std::size_t MaxSubscriptions>
struct ExtraLinks {
    ExtraLinkStorage<Slots, SlotBytes, MaxSubscriptions> items[N];
    NodeLinkSlot* slot(std::size_t i) noexcept { return &items[i].link; }
    SubscriptionTable* subscriptions(std::size_t i) noexcept {
        return &items[i].subscriptions;
    }
};

template <std::size_t Slots, std::size_t SlotBytes, std::size_t MaxSubscriptions>
struct ExtraLinks<0, Slots, SlotBytes, MaxSubscriptions> {
    NodeLinkSlot* slot(std::size_t) noexcept { return nullptr; }
    SubscriptionTable* subscriptions(std::size_t) noexcept { return nullptr; }
};

}  // namespace detail

// Defaults: 4 concurrent reassemblies, 600 octets each (a fragmented
// COMMAND_REQUEST plus headroom), 2048 octets of seal scratch (the largest
// UTF-8 status document this family sends), 512 octets of manifest / sample
// scratch (serve_catalog / publish), and -- for the node's OWN catalogue,
// below -- 8 topics / 64 field specs / 1.5 KiB of name/unit/description
// text, matching btp::StaticCatalog's own defaults. A Serial deployment
// bumps SlotBytes; a large manifest bumps ScratchBytes; a schema with many
// topics, fields, or long unit/description strings bumps the Catalog*
// template arguments.
// MaxSubscriptions, MaxCommands and CommandBytes last, after every other
// capacity, so an existing StaticNode<...> spelled out to CatalogStringBytes
// (or shorter) still compiles unchanged -- they only fix the new trailing
// defaults. MaxCommands sizes BOTH the responder's DedupCache (slots AND its
// requester table, one dimension standing in for two -- construct your own
// btp::DedupCache and call enable_commands(cache, handler, ctx) [via the
// Node:: overload, reachable below] to size them independently) and the
// initiator's CommandClient; CommandBytes is the per-slot byte capacity the
// responder's cache needs for the largest COMMAND_REQUEST + COMMAND_RESULT a
// handler here will ever see (the initiator's ClientCommand is fixed-size
// metadata, no byte storage of its own).
template <std::size_t Slots = 4, std::size_t SlotBytes = 600,
          std::size_t SealBytes = 2048, std::size_t ScratchBytes = 512,
          std::size_t CatalogTopics = 8, std::size_t CatalogFields = 64,
          std::size_t CatalogStringBytes = 1536,
          std::size_t MaxSubscriptions = 8, std::size_t MaxCommands = 4,
          std::size_t CommandBytes = 128, std::size_t CatalogSourceInfo = 0,
          std::size_t Links = 1>
class StaticNode
    : private detail::NodeStorage<Slots, SlotBytes, SealBytes, ScratchBytes>,
      public Node {
    using Storage =
        detail::NodeStorage<Slots, SlotBytes, SealBytes, ScratchBytes>;

    // Links last (2.47.0), after every other capacity, for the same reason
    // CatalogSourceInfo is: every existing StaticNode<...> keeps compiling and
    // keeps its size (Links == 1 adds no storage at all).
    static_assert(Links >= 1U && Links <= kMaxNodeLinks,
                  "StaticNode: Links must be 1..kMaxNodeLinks");

    // CatalogSourceInfo last (after every other capacity) so an existing
    // StaticNode<...> spelled out to CommandBytes still compiles -- it is only
    // non-zero for a producer that emits the format-2 source_info block
    // (fw version / chip / partition ahead of the topics in MANIFEST_DATA).
    StaticCatalog<CatalogTopics, CatalogFields, CatalogStringBytes,
                  CatalogSourceInfo>
        catalog_;

    // Backs on_publish()/publish_subscribed_topics() -- sized by CatalogTopics
    // (the node's own catalogue is the only source of topic_ids worth
    // registering a fill for, so its capacity is already the right bound; no
    // new template argument needed). Bound in the constructor body below.
    PublishRegistration publish_registrations_[CatalogTopics];

    // Backs enable_subscriptions() -- unlike publish_registrations_ above,
    // this genuinely needs its OWN capacity: several requesters can each
    // hold a subscription on the SAME topic, so the number of live grants
    // is not bounded by CatalogTopics. subscription_slots_ before
    // subscriptions_ on purpose -- members initialise in declaration order,
    // and subscriptions_ points into subscription_slots_.
    SubscriptionRecord subscription_slots_[MaxSubscriptions];
    SubscriptionTable subscriptions_;

    // Backs enable_subscription_client() -- the INITIATOR's memory of what
    // THIS node holds on OTHER peers' catalogues, the other side of the link
    // from subscription_slots_ / subscriptions_ above. Same per-capacity
    // reasoning (several subscriptions can be outstanding at once, across
    // topics and peers) and the same declaration-order requirement (slots
    // before the object that points into them). Named apart from Node's own
    // private subscription_client_ pointer on purpose -- this is the storage
    // it points to, not the pointer itself.
    ClientSubscription subscription_client_slots_[MaxSubscriptions];
    SubscriptionClient client_subscriptions_;

    // Backs enable_commands() -- the RESPONDER's dedup memory (docs/
    // commands.md section 2). command_storage_ is detail::CommandStorage
    // (above), not a plain array -- DedupCache's constructor reads its
    // storage[i].data / .capacity EAGERLY and latches valid() from that one
    // snapshot, so the byte pointers must already be wired before
    // commands_cache_ is built, not after (see detail::CommandStorage's own
    // comment). command_requesters_ tracks one row per distinct peer that
    // has sent a command, sized the same as MaxCommands as a reasonable
    // embedded default -- construct your own btp::DedupCache to size them
    // apart.
    DedupSlot command_slots_[MaxCommands];
    detail::CommandStorage<MaxCommands, CommandBytes> command_storage_;
    DedupRequester command_requesters_[MaxCommands];
    DedupCache commands_cache_;

    // Backs enable_command_client() -- the INITIATOR's memory of outstanding
    // COMMAND_REQUESTs, the other side of the link from commands_cache_
    // above, same MaxCommands capacity.
    ClientCommand client_command_slots_[MaxCommands];
    CommandClient client_commands_;

    // Links 1..Links-1 -- each a full per-link slot (reassembly, rx/open
    // buffers, subscription table) the same size as link 0's. Everything
    // else (catalogue, scratch buffers, dedup cache, identity, sequence) is
    // the node's and shared. See Node::attach_link().
    detail::ExtraLinks<Links - 1U, Slots, SlotBytes, MaxSubscriptions> extra_links_;

public:
    explicit StaticNode(
        NodeConfig& cfg,
        std::uint64_t reassembly_timeout_ms =
            kNodeDefaultReassemblyTimeoutMs) noexcept
        : Storage(),
          Node(cfg, Storage::slot_array, Storage::storage, Slots,
               reassembly_timeout_ms, Storage::rx_buffer, SlotBytes,
               Storage::seal_scratch, SealBytes, Storage::open_buffer, SlotBytes,
               Storage::scratch_buffer, ScratchBytes),
          catalog_(),
          publish_registrations_(),
          subscription_slots_(),
          subscriptions_(subscription_slots_, MaxSubscriptions),
          subscription_client_slots_(),
          client_subscriptions_(subscription_client_slots_, MaxSubscriptions),
          command_slots_(),
          command_storage_(),  // wires its own storage[] -- see its comment
          command_requesters_(),
          commands_cache_(command_slots_, command_storage_.storage, MaxCommands,
                         command_requesters_, MaxCommands),
          client_command_slots_(),
          client_commands_(client_command_slots_, MaxCommands),
          extra_links_() {
        // Ready for on_publish() / a peer's SUBSCRIBE / this node's own
        // subscribe() or command() with no separate setup call --
        // StaticNode<> owns all of its storage, same as catalog_.
        enable_publish_registry(publish_registrations_, CatalogTopics);
        enable_subscriptions(&subscriptions_);
        enable_subscription_client(&client_subscriptions_);
        enable_command_client(&client_commands_);
        // The RESPONDER side wired straight from cfg.has_command()/
        // cfg.command() through the command_thunk bridge -- has_command()
        // false (the default) leaves commands_ pointed at commands_cache_
        // with on_command_ still null, same "attached but unanswered" no-op
        // as never calling this at all (serve_command()'s own guard checks
        // on_command_ != nullptr). enable_commands(handler, ctx) sugar stays
        // reachable to set or replace it after construction.
        enable_commands(cfg.has_command() ? &Node::command_thunk : nullptr, &cfg);
    }

    // This node's own catalogue -- no separate btp::StaticCatalog to declare
    // in the caller or thread through by pointer. The serve_catalog() /
    // learn_catalog() overloads below (no Catalog* argument) point at it;
    // Node::serve_catalog(&external, ...) / Node::learn_catalog(&external)
    // stay reachable (via the `using` below) for a caller managing several
    // catalogues (a hub) or sharing one across nodes.
    Catalog& catalog() noexcept { return catalog_; }
    const Catalog& catalog() const noexcept { return catalog_; }

    using Node::serve_catalog;
    using Node::learn_catalog;
    using Node::begin;
    using Node::enable_commands;
    using Node::attach_link;

    // Node::attach_link() with this node's own storage for `link`
    // (1..Links-1) and its own subscription table. False past Links.
    bool attach_link(std::uint8_t link, NodeLink& cfg,
                     std::uint64_t reassembly_timeout_ms =
                         kNodeDefaultReassemblyTimeoutMs) noexcept {
        if (link == 0U || link >= Links) return false;
        return Node::attach_link(link, cfg, *extra_links_.slot(link - 1U),
                                 reassembly_timeout_ms,
                                 extra_links_.subscriptions(link - 1U));
    }

    // Sugar for enable_commands(&cache, handler, ctx) -- StaticNode<>
    // already owns the DedupCache and its storage (commands_cache_ above),
    // so this is just the handler. `ctx` defaults to nullptr for a handler
    // that closes over nothing (a free function acting on globals / the
    // node itself, same shape sender.cpp's handle_command() uses).
    void enable_commands(NodeActionFn handler, void* ctx = nullptr) noexcept {
        Node::enable_commands(&commands_cache_, handler, ctx);
    }

    // Convenience for a StaticNode<> producer that serves its own catalogue
    // and answers a session with `local_hello` -- what a caller otherwise
    // hand-writes as four separate calls (catalog().set_config_revision(),
    // serve_catalog(), enable_session(), begin(true)): set the config
    // revision, serve the catalogue (role read straight off
    // local_hello.role -- no need to repeat it), enable the session, then go
    // fully live (arm it, send the catalogue's MANIFEST_DATA unsolicited so
    // a late joiner needs no request). `source_uuid` stays nullptr (zeroed)
    // unless this node has one to advertise.
    //
    // Doesn't fit a node that needs the deferred-arm case (a session armed
    // only on some later trigger of your own, e.g. a console's ENTER line):
    // call serve_catalog() / enable_session() / Node::begin(false) yourself
    // there and arm_session() later, same as always.
    bool begin(const char* source_name, const Hello& local_hello,
              std::uint64_t hello_deadline_ms = 0U,
              std::uint32_t config_revision = 1U,
              const std::uint8_t* source_uuid = nullptr) noexcept {
        catalog_.set_config_revision(config_revision);
        serve_catalog(local_hello.role, source_uuid, source_name);
        enable_session(local_hello, hello_deadline_ms);
        return Node::begin(/*arm_and_announce=*/true);
    }

    void serve_catalog(std::uint8_t role = 0U,
                       const std::uint8_t* source_uuid = nullptr,
                       const char* source_name = nullptr) noexcept {
        Node::serve_catalog(&catalog_, role, source_uuid, source_name);
    }
    void learn_catalog() noexcept { Node::learn_catalog(&catalog_); }

    // Same, plus on_sample() in the one call -- the consumer's analogue of
    // topic(..., fill) above: "learn schemas into my own catalogue AND
    // decode every sample against it with this callback" is one statement
    // instead of two. `sample` (default nullptr) leaves on_sample() as it
    // was -- call it yourself later if you'd rather.
    void learn_catalog(NodeSampleFn sample, void* ctx = nullptr) noexcept {
        Node::learn_catalog(&catalog_);
        if (sample != nullptr) on_sample(sample, ctx);
    }

    // Sugar for catalog().topic(...) -- declare a topic in this node's own
    // catalogue without reaching for catalog() first.
    // `fill` (default nullptr -- every existing call site keeps declaring a
    // topic with no fill registered, exactly as before this parameter
    // existed) is on_publish()'d right here, before the schema chain below
    // even runs: declaring a topic and saying how to fill it become the one
    // statement publish_subscribed_topics() (node.hpp) needs, instead of a
    // separate on_publish() call after .end().
    TopicBuilder topic(std::uint16_t topic_id, std::uint16_t schema_version,
                       const char* name, NodeNamedFillFn fill = nullptr,
                       void* fill_ctx = nullptr,
                       TelemetryEncoding encoding = TelemetryEncoding::PackedLe,
                       bool subscribable = true,
                       std::uint32_t max_rate_millihz = 0U) noexcept {
        if (fill != nullptr) on_publish(topic_id, fill, fill_ctx);
        return catalog_.topic(topic_id, schema_version, name, encoding,
                              subscribable, max_rate_millihz);
    }
};

// ---------------------------------------------------------------------------
// SizedNode -- StaticNode<> pre-sized to one of three memory tiers
// ---------------------------------------------------------------------------
//
// StaticNode<>'s ten template parameters size ten genuinely different things
// (concurrent reassemblies, the seal/manifest scratch, the catalogue, the
// subscription and command tables) -- reasoning about all ten by hand for
// "does this fit my budget" is exactly the kind of thing worth naming once.
// Pick a tier and every parameter scales together:
//
//   btp::SizedNode<btp::NodeSize::Low>    node(cfg);   // sizeof() == 7,248
//   btp::SizedNode<btp::NodeSize::Medium> node(cfg);   // sizeof() == 17,448 -- StaticNode<>'s own defaults
//   btp::SizedNode<btp::NodeSize::High>   node(cfg);   // sizeof() == 67,720
//
// (measured, not estimated -- test_sized_node_medium_matches_static_node_
// defaults() in tests/test_node.cpp pins Medium to StaticNode<>'s own
// sizeof(), so the two can never silently drift apart). Slots costs more
// than SlotBytes alone suggests: each ReassemblySlot (fragmentation.hpp)
// carries a FIXED 255-entry fragment_sizes_ array -- fragment_count is an
// 8-bit wire field, so a slot has to be ready to track the worst case
// regardless of Slots -- plus a few hundred more octets of fixed metadata,
// on top of the SlotBytes storage region and the shared rx_buffer /
// open_buffer (each one SlotBytes, not per-slot). Raising Slots is
// therefore the single most expensive knob to turn per unit; raising
// SlotBytes on its own is comparatively cheap. StaticNode<...> with your
// own arguments stays the escape hatch for a node that needs one dimension
// off this curve -- a desktop hub with a huge catalogue but few concurrent
// reassemblies, say.
enum class NodeSize : std::uint8_t { Low, Medium, High };

namespace detail {

template <NodeSize Size>
struct NodeSizeTraits;

// A robot with ONE small topic and little else -- a battery-powered sensor,
// or a memory-starved corner of a bigger deployment. Room for a handful of
// small fragmented commands, a small catalogue, few subscribers.
template <>
struct NodeSizeTraits<NodeSize::Low> {
    static const std::size_t Slots = 2U;
    static const std::size_t SlotBytes = 400U;
    static const std::size_t SealBytes = 512U;
    static const std::size_t ScratchBytes = 256U;
    static const std::size_t CatalogTopics = 4U;
    static const std::size_t CatalogFields = 16U;
    static const std::size_t CatalogStringBytes = 512U;
    static const std::size_t MaxSubscriptions = 4U;
    static const std::size_t MaxCommands = 2U;
    static const std::size_t CommandBytes = 64U;
};

// The ESP32-class robot this library was sized for in the first place --
// identical to StaticNode<>'s own bare defaults, so btp::SizedNode<
// NodeSize::Medium> and btp::StaticNode<> cost exactly the same.
template <>
struct NodeSizeTraits<NodeSize::Medium> {
    static const std::size_t Slots = 4U;
    static const std::size_t SlotBytes = 600U;
    static const std::size_t SealBytes = 2048U;
    static const std::size_t ScratchBytes = 512U;
    static const std::size_t CatalogTopics = 8U;
    static const std::size_t CatalogFields = 64U;
    static const std::size_t CatalogStringBytes = 1536U;
    static const std::size_t MaxSubscriptions = 8U;
    static const std::size_t MaxCommands = 4U;
    static const std::size_t CommandBytes = 128U;
};

// A hub or desktop aggregator -- many topics, many subscribers, bigger
// commands, several concurrent large reassemblies. Not meant for a
// memory-constrained MCU.
template <>
struct NodeSizeTraits<NodeSize::High> {
    static const std::size_t Slots = 8U;
    static const std::size_t SlotBytes = 2048U;
    static const std::size_t SealBytes = 4096U;
    static const std::size_t ScratchBytes = 2048U;
    static const std::size_t CatalogTopics = 32U;
    static const std::size_t CatalogFields = 256U;
    static const std::size_t CatalogStringBytes = 4096U;
    static const std::size_t MaxSubscriptions = 32U;
    static const std::size_t MaxCommands = 16U;
    static const std::size_t CommandBytes = 512U;
};

}  // namespace detail

template <NodeSize Size>
using SizedNode =
    StaticNode<detail::NodeSizeTraits<Size>::Slots,
              detail::NodeSizeTraits<Size>::SlotBytes,
              detail::NodeSizeTraits<Size>::SealBytes,
              detail::NodeSizeTraits<Size>::ScratchBytes,
              detail::NodeSizeTraits<Size>::CatalogTopics,
              detail::NodeSizeTraits<Size>::CatalogFields,
              detail::NodeSizeTraits<Size>::CatalogStringBytes,
              detail::NodeSizeTraits<Size>::MaxSubscriptions,
              detail::NodeSizeTraits<Size>::MaxCommands,
              detail::NodeSizeTraits<Size>::CommandBytes>;

}  // namespace btp

#endif  // BTP_NODE_HPP
