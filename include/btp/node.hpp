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
// EVERYTHING the node calls out to is a C-style function pointer in NodeConfig
// (void* context + a plain function), never std::function / virtual:
//
//   send   one encoded frame -> your radio / UART / HID. Needed to send() or
//          run a session; a receive-only node omits it.
//   clock  (optional)  -> millis since boot; nullptr means you pass now_ms
//   seal   (optional)  encrypt one logical payload; nullptr means cleartext.
//                      The KEY lives in your seal function, never in BTP.
//   open   (optional)  decrypt one received payload; nullptr means receive()
//                      hands back the sealed bytes for you to open.
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
//   * SERIAL byte-stream framing (COBS) and Serial frames larger than one
//     ESP-NOW payload -- not in this first cut (see send() / receive() below).
//
// This is library 2.11.0 territory.

#include "btp/catalog.hpp"    // Catalog, CatalogTopic (brings btp/telemetry.hpp, btp/messages.hpp)
#include "btp/codec.hpp"      // Header, ByteView, MessageType, TransportProfile, kFlagEncrypted
#include "btp/endpoint.hpp"   // Endpoint, EndpointSealFn, LogicalMessage, kEndpointAeadTagSize
#include "btp/fragmentation.hpp"  // ReassemblySlot, ReassemblyStorage
#include "btp/receiver.hpp"   // Receiver, ReceivedMessage, ReceiveOutcome
#include "btp/session.hpp"    // Session, Hello, SessionEvent, kSessionMaxReplySize
#include "btp/subscription.hpp"  // SubscriptionTable, SubscriptionClient
#include "btp/telemetry.hpp"  // SampleReader

#include <cstddef>
#include <cstdint>

namespace btp {

class Node;  // NodeTerminalFn below needs the name before the class itself.

// ---------------------------------------------------------------------------
// Callbacks the node calls out to
// ---------------------------------------------------------------------------

// Returns a monotonic millisecond reading -- millis() / esp_timer_get_time() /
// 1000 on an MCU, QElapsedTimer::elapsed() under Qt, a counter in a test. May
// be read from a timer ISR that increments a word; the node's own methods must
// NOT be called from an ISR (btp::Receiver / btp::Session are not internally
// synchronised, by design).
using NodeClockFn = std::uint64_t (*)(void* ctx);

// Decrypts ONE reassembled logical payload -- the mirror of EndpointSealFn.
// `header` is the canonical logical header the receiver hands back (FRAGMENTED
// cleared, fragment_index 0, fragment_count 1, ENCRYPTED still set).
// `sealed_size` includes the trailing 16-octet AEAD tag; `out_plaintext` has
// room for `sealed_size - 16`. Return false (tag mismatch, no key) and the
// node drops the message. The KEY lives here (real: RadioSeal / bally-seal,
// which selects it from header.source_id), never in BTP.
using NodeOpenFn = bool (*)(void* ctx, const Header& header,
                            std::uint16_t sealed_size,
                            const std::uint8_t* sealed,
                            std::uint8_t* out_plaintext);

// Picks the seal for ONE automatic reply the node is about to send: a
// SUBSCRIBE_RESULT / UNSUBSCRIBE_RESULT, a COMMAND_RESULT (a fresh one or a
// DuplicateComplete replay), or a MANIFEST_DATA. `request_header` is the
// ORIGINAL request's header -- source_id names who actually asked, unlike
// the reply's own header, which always carries this node's own identity, so
// this is the one place a hub-shaped node (several keys, one per relayed
// peer or channel) can pick the matching key per reply instead of one key
// for every automatic send. Leave both `*out_seal`/`*out_seal_ctx` at their
// nullptr default to send this ONE reply in the clear, whatever cfg.seal is.
//
// Optional: nullptr (the default) means every automatic reply seals with
// cfg.seal/cfg.seal_ctx, exactly as if this callback did not exist. Does NOT
// apply to send() / send_with() / publish() / publish_named() -- those are
// the caller's own sends, already covered by send_with()'s explicit seal
// argument -- nor to an INITIATOR's own outgoing requests (connect() /
// subscribe() / command() and their renewals), which have no "original
// request" to classify by and always use cfg.seal.
using NodeReplySealFn = void (*)(void* ctx, const Header& request_header,
                                 EndpointSealFn* out_seal, void** out_seal_ctx);

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
// to 0 (ResultStatus::Success) and error_code to 0 (ResultError::None), so a
// handler that only has good news to report can leave them alone.
struct NodeActionOutcome {
    std::uint8_t status;              // ResultStatus
    std::uint16_t error_code;         // ResultError
    const char* message;              // NUL-terminated UTF-8, optional (nullptr -> none)
    const std::uint8_t* result_data;  // optional action-defined result bytes
    std::size_t result_size;
};

// Called once for a Fresh COMMAND_REQUEST (btp::DedupCache has not seen this
// requester + sequence before). Runs the action SYNCHRONOUSLY -- there is no
// "pending, complete me later" path in this first cut, so a slow action
// belongs on a task of its own, with the handler answering once it is done.
// Fill `outcome` (pre-set to Success, no message, no result) and return; the
// node builds COMMAND_RESULT from it, sends it, and records it so a
// retransmission of this exact request replays the same bytes instead of
// running the action a second time.
using NodeActionFn = void (*)(void* ctx, std::uint16_t action_id,
                              std::uint16_t action_version, ByteView parameters,
                              NodeActionOutcome* outcome);

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct NodeConfig {
    // Identity. Both non-zero (BTP reserves 0 for each); source_id is usually
    // derived from the MAC, boot_id changes every reboot.
    std::uint32_t source_id;
    std::uint32_t boot_id;

    // NOT "which transport" -- the node never touches your link. It is only the
    // frame-size class the fragmenter targets (EspNow 250/210, Serial
    // 4096/4056, UsbHid 62/22) plus one rule (no ENCRYPTED on UsbHid). Pick the
    // one that fits your link's MTU.
    TransportProfile transport;

    EndpointSendFn send;      // one encoded frame -> the wire. Needed to send()
    void* send_ctx;           // or run a session; a receive-only node may omit it.

    NodeClockFn clock;        // optional. nullptr -> pass now_ms to receive/tick.
    void* clock_ctx;

    EndpointSealFn seal;      // optional. nullptr -> send() is cleartext.
    void* seal_ctx;

    NodeOpenFn open;          // optional. nullptr -> receive() returns sealed bytes.
    void* open_ctx;

    // optional. nullptr -> TERMINAL_IN / TERMINAL_OUT come back as
    // NodeRx::Complete for the caller to route by hand, same as before
    // on_terminal() existed. Wired straight from here by every Node (no
    // extra storage needed, same as seal / open above); on_terminal(...)
    // stays reachable to set or replace it after construction.
    NodeTerminalFn terminal;
    void* terminal_ctx;

    // optional. nullptr -> COMMAND_REQUEST goes unanswered, same as before
    // enable_commands() existed. Unlike terminal above, this ONLY takes
    // effect on a StaticNode<> -- it owns the DedupCache this needs
    // (commands_cache_) and wires it from here in its own constructor; a
    // bare Node has no such storage to bind it to; call
    // Node::enable_commands(cache, handler, ctx) yourself there instead, as
    // always. enable_commands(handler, ctx) (StaticNode<> sugar) stays
    // reachable to set or replace the handler after construction.
    NodeActionFn command;
    void* command_ctx;

    // Optional: a caller building NodeConfig with a positional aggregate
    // initializer (an existing one, or docs/library.md's own mockup) leaves
    // this nullptr too -- see NodeReplySealFn's own comment for what that
    // means.
    NodeReplySealFn reply_seal;
    void* reply_seal_ctx;
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
    CommandServed,   // a COMMAND_REQUEST against commands() -- the node already
                     // ran the action (or replayed / rejected it) and replied.
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
};

const char* node_rx_string(NodeRx rx) noexcept;

// ---------------------------------------------------------------------------
// Node
// ---------------------------------------------------------------------------
//
//   btp::StaticNode<> node({source_id, boot_id, btp::TransportProfile::EspNow,
//                           &radio_send, nullptr,   // send
//                           &clock_ms,   nullptr,   // clock
//                           &seal_c,     nullptr,   // seal  (the key lives here)
//                           &open_c,     nullptr}); // open
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
    // `cfg` is copied in. The rest is caller-owned storage, exactly as
    // btp::Receiver / btp::Endpoint expect it:
    //   slots / storage / slot_count / reassembly_timeout_ms  -> btp::Receiver
    //   rx_buffer / rx_capacity        the buffer receive() copies a completed
    //                                  logical message into (>= slot capacity)
    //   seal_scratch / seal_scratch_cap  the sealed copy a FRAGMENTED encrypted
    //                                  send is cut from; may be {nullptr, 0}
    //                                  when `cfg.seal` is null or messages
    //                                  never fragment
    //   open_buffer / open_capacity    where `cfg.open` writes the plaintext
    //                                  (>= slot capacity); may be {nullptr, 0}
    //                                  when `cfg.open` is null
    //   scratch_buffer / scratch_capacity  where serve_catalog() builds a
    //                                  MANIFEST_DATA and publish() a sample;
    //                                  may be {nullptr, 0} for a node that does
    //                                  neither
    Node(const NodeConfig& cfg, ReassemblySlot* slots,
         const ReassemblyStorage* storage, std::size_t slot_count,
         std::uint64_t reassembly_timeout_ms, std::uint8_t* rx_buffer,
         std::size_t rx_capacity, std::uint8_t* seal_scratch,
         std::size_t seal_scratch_cap, std::uint8_t* open_buffer,
         std::size_t open_capacity, std::uint8_t* scratch_buffer,
         std::size_t scratch_capacity) noexcept;

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    // Replaces the whole external configuration (identity, transport, send /
    // seal / open / reply_seal / terminal / command) without touching the
    // receiver's storage, any attached session/catalogue/subscription/
    // command state, or frames_tx(). The one way to change it after
    // construction -- the constructor is the only other place `cfg` is read.
    // Meant for a caller whose identity or send callback is only known
    // after something else exists (e.g. TxScheduler configured later than
    // the Node itself is constructed): build with a placeholder NodeConfig,
    // reconfigure() with the real one once it is known, THEN begin().
    //
    // Calling it again later (already begin()'d, maybe already receiving)
    // is not guarded -- the same trust the rest of this library places in
    // the caller. begin() afterward re-runs endpoint.configure(), which
    // resets the sequence counter to 1 (btp::Endpoint::configure()'s own
    // documented behaviour) -- correct for a fresh identity, a corruption of
    // any in-flight correlation (a session, an outstanding subscribe()/
    // command()) if the identity did not actually change. A hub re-keying a
    // still-idle node (no session armed, nothing outstanding) is the
    // intended later-call case; mid-flight is the caller's call to make.
    void reconfigure(const NodeConfig& cfg) noexcept { cfg_ = cfg; }

    // endpoint.configure() + receiver.valid() + (session enabled?
    // session.valid()). Check once at boot, and again after reconfigure() if
    // identity/transport changed. A missing `send` is not a failure here --
    // send() / send_with() and a session reply check for it in place -- so a
    // receive-only node can leave it null.
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
    // Encodes `payload` as one or more frames and hands each to `cfg.send`. A
    // fresh sequence is reserved. `cfg.seal != nullptr` seals once over the
    // whole payload before fragmenting; a false from the seal sends nothing
    // (fail-closed). Returns "did the whole logical message go out".
    //
    // FIRST-CUT LIMIT: a single encoded frame must fit the ESP-NOW ceiling
    // (250 octets) -- true for every EspNow and UsbHid frame, and for Serial
    // payloads up to ~200 octets. A larger Serial frame returns false; native
    // large-Serial TX is a later change (it needs a caller frame buffer the
    // endpoint layer does not take yet).
    bool send(MessageType type, std::uint16_t object_id,
              const std::uint8_t* payload, std::size_t size,
              std::uint64_t timestamp_us) noexcept;

    // Same, with a seal for THIS message (a hub that seals channel C to a robot
    // and channel B to a desktop with different keys). A null `seal` forces
    // cleartext regardless of cfg.seal.
    bool send_with(MessageType type, std::uint16_t object_id,
                   const std::uint8_t* payload, std::size_t size,
                   std::uint64_t timestamp_us, EndpointSealFn seal,
                   void* seal_ctx) noexcept;

    // ---- receive ---------------------------------------------------------
    // Feed one already-delimited datagram (ESP-NOW / USB HID deliver these
    // whole). Sweeps stale partials, decodes, checks CRC, reassembles; with a
    // session enabled it also runs the HELLO handshake, renews the watchdog and
    // answers SESSION_CLOSE (through `cfg.send`) before a frame is routed. On
    // NodeRx::Complete, *out holds the whole logical message -- its payload
    // copied into rx_buffer (or open_buffer, if `cfg.open` decrypted it), valid
    // until the next receive().
    //
    // The no-now overload reads `cfg.clock`; without one it behaves as now = 0
    // (fine for a session-less consumer, wrong for the watchdog).
    NodeRx receive(const std::uint8_t* datagram, std::size_t size,
                   ReceivedMessage* out) noexcept;
    NodeRx receive(const std::uint8_t* datagram, std::size_t size,
                   std::uint64_t now_ms, ReceivedMessage* out) noexcept;

    // ---- session responder (opt-in) -------------------------------------
    // `local` is this peer's HELLO advertisement (role, versions, limits,
    // peer_uuid, config_revision). `hello_deadline_ms` bounds the wait for the
    // peer's HELLO after arm_session() (2000 for a serial console; 0 disables
    // that bound -- only the negotiated watchdog then applies). Copied in.
    void enable_session(const Hello& local,
                        std::uint64_t hello_deadline_ms) noexcept;
    bool session_enabled() const noexcept { return session_on_; }

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
    SessionEvent session_event() const noexcept { return last_session_event_; }

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
        subscriptions_ = table;
    }
    SubscriptionTable* subscriptions() noexcept { return subscriptions_; }
    const SubscriptionTable* subscriptions() const noexcept { return subscriptions_; }

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

    // ---- commands (opt-in, docs/commands.md section 2) ---------------------
    // btp::DedupCache (session.hpp) is the RESPONDER's memory -- execute an
    // action once, remember the result, replay a retransmission instead of
    // running it again. btp::CommandClient (session.hpp) is the INITIATOR's:
    // send, correlate the eventual COMMAND_RESULT, time out with no retry
    // budget if none comes. Storage stays the caller's, same reasoning as
    // subscriptions -- the caller sizes the cache for the largest request /
    // result it will ever see.

    // RESPONDER: `cache` deduplicates; `handler` runs a Fresh request
    // synchronously (NodeActionFn above). nullptr for either detaches.
    void enable_commands(DedupCache* cache, NodeActionFn handler, void* ctx) noexcept {
        commands_ = cache;
        on_command_ = handler;
        on_command_ctx_ = ctx;
    }
    DedupCache* commands() noexcept { return commands_; }
    const DedupCache* commands() const noexcept { return commands_; }

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
    void enable_publish_registry(PublishRegistration* slots,
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
    Receiver& receiver() noexcept { return receiver_; }
    const Receiver& receiver() const noexcept { return receiver_; }
    Session* session() noexcept { return session_on_ ? &session_ : nullptr; }
    const Session* session() const noexcept {
        return session_on_ ? &session_ : nullptr;
    }
    // Always present (Idle until connect()), unlike session() above.
    SessionInitiator& initiator() noexcept { return initiator_; }
    const SessionInitiator& initiator() const noexcept { return initiator_; }

    struct Stats {
        Receiver::Stats rx;
        // btp::decode failures seen on the session path (the DecodedFrame the
        // session needs is decoded here, not inside btp::Receiver, so these
        // would otherwise go uncounted).
        std::uint32_t session_path_dropped_crc;
        std::uint32_t session_path_dropped_decode;
    };
    Stats stats() const noexcept;

private:
    std::uint64_t resolve_now(std::uint64_t fallback) const noexcept;
    NodeRx finish(ReceiveOutcome outcome, ReceivedMessage* out,
                 std::uint64_t now_ms) noexcept;
    // cfg_.reply_seal, or cfg_.seal/cfg_.seal_ctx when it is null -- see
    // NodeReplySealFn's comment. Every automatic-reply send site below calls
    // this once, right before send_with(), instead of using send()/cfg_.seal
    // directly.
    void reply_seal_for(const Header& request, EndpointSealFn* out_seal,
                        void** out_seal_ctx) const noexcept;
    void serve_manifest(const Header& request, ByteView payload) noexcept;
    void emit_manifest(const Header& request, const RequestRef& reply_to,
                       std::uint8_t status, std::uint8_t flags,
                       std::uint16_t error_code, bool with_topics) noexcept;
    void serve_subscribe(const Header& request, ByteView payload,
                         std::uint64_t now_ms) noexcept;
    void serve_unsubscribe(const Header& request, ByteView payload) noexcept;
    void drain_subscription_renewals(std::uint64_t now_ms) noexcept;
    void serve_command(const Header& request, ByteView payload) noexcept;
    void emit_command_result(const Header& request, std::uint16_t action_id,
                             std::uint16_t action_version,
                             const NodeActionOutcome& outcome,
                             std::size_t slot) noexcept;
    void emit_command_reject(const Header& request, std::uint16_t action_id,
                             std::uint16_t action_version, ResultStatus status,
                             ResultError error) noexcept;
    void emit_status(std::uint64_t now_ms) noexcept;

    NodeConfig cfg_;
    std::uint8_t* rx_buffer_;
    std::size_t rx_capacity_;
    std::uint8_t* seal_scratch_;
    std::size_t seal_scratch_cap_;
    std::uint8_t* open_buffer_;
    std::size_t open_capacity_;
    std::uint8_t* scratch_buffer_;
    std::size_t scratch_capacity_;

    Endpoint endpoint_;
    Receiver receiver_;
    Session session_;       // placeholder HELLO until enable_session()
    bool session_on_;
    SessionEvent last_session_event_;
    std::uint32_t session_path_dropped_crc_;
    std::uint32_t session_path_dropped_decode_;

    SessionInitiator initiator_;  // Idle until connect()
    InitiatorEvent last_initiator_event_;

    SubscriptionTable* subscriptions_;        // nullptr until enable_subscriptions()
    SubscriptionClient* subscription_client_;  // nullptr until enable_subscription_client()
    SubscriptionEvent last_subscription_event_;

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

    Catalog* learn_catalog_;
    NodeSampleFn on_sample_;
    void* on_sample_ctx_;

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
    ReassemblySlot slots[Slots];
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
          std::size_t CommandBytes = 128>
class StaticNode
    : private detail::NodeStorage<Slots, SlotBytes, SealBytes, ScratchBytes>,
      public Node {
    using Storage =
        detail::NodeStorage<Slots, SlotBytes, SealBytes, ScratchBytes>;

    StaticCatalog<CatalogTopics, CatalogFields, CatalogStringBytes> catalog_;

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

public:
    explicit StaticNode(
        const NodeConfig& cfg,
        std::uint64_t reassembly_timeout_ms =
            kNodeDefaultReassemblyTimeoutMs) noexcept
        : Storage(),
          Node(cfg, Storage::slots, Storage::storage, Slots,
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
          client_commands_(client_command_slots_, MaxCommands) {
        // Ready for on_publish() / a peer's SUBSCRIBE / this node's own
        // subscribe() or command() with no separate setup call --
        // StaticNode<> owns all of its storage, same as catalog_.
        enable_publish_registry(publish_registrations_, CatalogTopics);
        enable_subscriptions(&subscriptions_);
        enable_subscription_client(&client_subscriptions_);
        enable_command_client(&client_commands_);
        // The RESPONDER side wired straight from cfg.command / cfg.command_ctx
        // -- nullptr either way (the default) leaves commands_ pointed at
        // commands_cache_ with on_command_ still null, same "attached but
        // unanswered" no-op as never calling this at all (serve_command()'s
        // own guard checks on_command_ != nullptr). enable_commands(handler,
        // ctx) sugar stays reachable to set or replace it after construction.
        enable_commands(cfg.command, cfg.command_ctx);
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

}  // namespace btp

#endif  // BTP_NODE_HPP
