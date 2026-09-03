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
//   * the SESSION INITIATOR -- sending ENTER / HELLO, the retry budget,
//     awaiting HELLO_RESULT. btp::Node is responder + producer.
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
    CatalogUpdated,  // a MANIFEST_DATA -- the node ingested it into the learn catalog.
    SampleDelivered, // a TELEMETRY sample -- the node decoded it and called on_sample.
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
    Node(const NodeConfig& cfg, ReassemblySlot* slots,
         const ReassemblyStorage* storage, std::size_t slot_count,
         std::uint64_t reassembly_timeout_ms, std::uint8_t* rx_buffer,
         std::size_t rx_capacity, std::uint8_t* seal_scratch,
         std::size_t seal_scratch_cap, std::uint8_t* open_buffer,
         std::size_t open_capacity) noexcept;

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    // endpoint.configure() + receiver.valid() + (session enabled?
    // session.valid()). Check once at boot. A missing `send` is not a failure
    // here -- send() / send_with() and a session reply check for it in place --
    // so a receive-only node can leave it null.
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

    // ---- escape hatches -----------------------------------------------------
    Endpoint& endpoint() noexcept { return endpoint_; }
    const Endpoint& endpoint() const noexcept { return endpoint_; }
    Receiver& receiver() noexcept { return receiver_; }
    const Receiver& receiver() const noexcept { return receiver_; }
    Session* session() noexcept { return session_on_ ? &session_ : nullptr; }
    const Session* session() const noexcept {
        return session_on_ ? &session_ : nullptr;
    }

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
    NodeRx finish(ReceiveOutcome outcome, ReceivedMessage* out) noexcept;

    NodeConfig cfg_;
    std::uint8_t* rx_buffer_;
    std::size_t rx_capacity_;
    std::uint8_t* seal_scratch_;
    std::size_t seal_scratch_cap_;
    std::uint8_t* open_buffer_;
    std::size_t open_capacity_;

    Endpoint endpoint_;
    Receiver receiver_;
    Session session_;       // placeholder HELLO until enable_session()
    bool session_on_;
    SessionEvent last_session_event_;
    std::uint32_t session_path_dropped_crc_;
    std::uint32_t session_path_dropped_decode_;

    Catalog* learn_catalog_;
    NodeSampleFn on_sample_;
    void* on_sample_ctx_;
};

// ---------------------------------------------------------------------------
// StaticNode -- Node that owns its buffers (the common embedded case)
// ---------------------------------------------------------------------------

// 4000 ms, matching bally_OS RxRouter / bally_dongle ProtocolRouter: how long a
// half-arrived message holds a slot before the sweep reclaims it.
static const std::uint64_t kNodeDefaultReassemblyTimeoutMs = 4000U;

namespace detail {

template <std::size_t Slots, std::size_t SlotBytes, std::size_t SealBytes>
struct NodeStorage {
    ReassemblySlot slots[Slots];
    std::uint8_t storage_bytes[Slots][SlotBytes];
    ReassemblyStorage storage[Slots];
    std::uint8_t rx_buffer[SlotBytes];
    std::uint8_t open_buffer[SlotBytes];
    std::uint8_t seal_scratch[SealBytes];

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
// UTF-8 status document this family sends). A Serial deployment bumps SlotBytes.
template <std::size_t Slots = 4, std::size_t SlotBytes = 600,
          std::size_t SealBytes = 2048>
class StaticNode : private detail::NodeStorage<Slots, SlotBytes, SealBytes>,
                   public Node {
    using Storage = detail::NodeStorage<Slots, SlotBytes, SealBytes>;

public:
    explicit StaticNode(
        const NodeConfig& cfg,
        std::uint64_t reassembly_timeout_ms =
            kNodeDefaultReassemblyTimeoutMs) noexcept
        : Storage(),
          Node(cfg, Storage::slots, Storage::storage, Slots,
               reassembly_timeout_ms, Storage::rx_buffer, SlotBytes,
               Storage::seal_scratch, SealBytes, Storage::open_buffer,
               SlotBytes) {}
};

}  // namespace btp

#endif  // BTP_NODE_HPP
