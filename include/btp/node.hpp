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
    RequestServed,   // a MANIFEST_REQUEST -- the node built and sent MANIFEST_DATA.
    Ignored,         // a frame the node would manage but cannot yet: a session not
                     // Active, or a sample for a topic the catalog has not learned.
    DroppedFrame,    // btp::decode / reassembly / a malformed managed payload rejected it.
                     // Counts are in receiver().stats().
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
    // seal / open / reply_seal) without touching the receiver's storage,
    // any attached session/catalogue/subscription/command state, or
    // frames_tx(). The one way to change it after construction -- the
    // constructor is the only other place `cfg` is read. Meant for a caller
    // whose identity or send callback is only known after something else
    // exists (e.g. TxScheduler configured later than the Node itself is
    // constructed): build with a placeholder NodeConfig, reconfigure() with
    // the real one once it is known, THEN begin().
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
    bool begin() noexcept;
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
    void serve_manifest(const Header& request, ByteView payload) noexcept;
    void emit_manifest(const RequestRef& reply_to, std::uint8_t status,
                       std::uint8_t flags, std::uint16_t error_code,
                       bool with_topics) noexcept;
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

}  // namespace detail

// Defaults: 4 concurrent reassemblies, 600 octets each (a fragmented
// COMMAND_REQUEST plus headroom), 2048 octets of seal scratch (the largest
// UTF-8 status document this family sends), 512 octets of manifest / sample
// scratch (serve_catalog / publish), and -- for the node's OWN catalogue,
// below -- 8 topics / 64 field specs / 1 KiB of name text, matching
// btp::StaticCatalog's own defaults. A Serial deployment bumps SlotBytes; a
// large manifest bumps ScratchBytes; a schema with many topics or fields
// bumps the Catalog* template arguments.
template <std::size_t Slots = 4, std::size_t SlotBytes = 600,
          std::size_t SealBytes = 2048, std::size_t ScratchBytes = 512,
          std::size_t CatalogTopics = 8, std::size_t CatalogFields = 64,
          std::size_t CatalogStringBytes = 1024>
class StaticNode
    : private detail::NodeStorage<Slots, SlotBytes, SealBytes, ScratchBytes>,
      public Node {
    using Storage =
        detail::NodeStorage<Slots, SlotBytes, SealBytes, ScratchBytes>;

    StaticCatalog<CatalogTopics, CatalogFields, CatalogStringBytes> catalog_;

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
          catalog_() {}

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

    void serve_catalog(std::uint8_t role = 0U,
                       const std::uint8_t* source_uuid = nullptr,
                       const char* source_name = nullptr) noexcept {
        Node::serve_catalog(&catalog_, role, source_uuid, source_name);
    }
    void learn_catalog() noexcept { Node::learn_catalog(&catalog_); }

    // Sugar for catalog().topic(...) -- declare a topic in this node's own
    // catalogue without reaching for catalog() first.
    TopicBuilder topic(std::uint16_t topic_id, std::uint16_t schema_version,
                       const char* name,
                       TelemetryEncoding encoding = TelemetryEncoding::PackedLe,
                       bool subscribable = true,
                       std::uint32_t max_rate_millihz = 0U) noexcept {
        return catalog_.topic(topic_id, schema_version, name, encoding,
                              subscribable, max_rate_millihz);
    }
};

}  // namespace btp

#endif  // BTP_NODE_HPP
