// example/node_config.hpp
//
// The btp::NodeConfig each example builds its btp::Node around: every
// callback btp::Node calls out to (send/seal/open/terminal/command),
// factored out of sender.cpp / receiver.cpp so those files stay the LOOP,
// not the wiring. ProducerLink is sender.cpp's; ConsumerLink is
// receiver.cpp's; both share the ONE demo AEAD key below (AEAD is
// symmetric -- the same key seals on one end and opens on the other) --
// keeping it in one file is also what makes "both ends must agree" true by
// construction instead of by two comments promising two copy-pasted arrays
// stay identical.
//
// This split is a documentation choice, not a BTP one: btp::NodeConfig
// itself lives in include/btp/node.hpp (part of the library, the CONTRACT
// every deployment implements); the CONCRETE implementation below -- the
// fake link, the real AES-128-GCM calls, the robot's own command handling
// -- is deployment-specific and belongs to the consumer, never to BTP
// (docs/library.md chapter 11: no I/O, no key, no routing policy). In a
// real project this is bally_dongle's / bally_OS's / TraceView's own code,
// not a BTP header at all -- it is only a shared example/ file here because
// sender.cpp and receiver.cpp are two halves of the SAME demo and would
// otherwise duplicate it (and the key) verbatim.

#ifndef BTP_EXAMPLE_NODE_CONFIG_HPP
#define BTP_EXAMPLE_NODE_CONFIG_HPP

#include <btp/aead.hpp>
#include <btp/node.hpp>

namespace example {

// Must be the SAME 16 bytes on both ends -- AEAD is symmetric, both peers
// seal/open with one shared key. A real deployment provisions each pair's
// key out of band (radio pairing, a manifest, a factory step) -- never like
// this, and never the same key for every pair.
const std::uint8_t kDemoAeadKey[btp::kAesGcmKeySize] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
};

// Everything btp::Node calls out to for sender.cpp's producer, in one
// class: the fake link (send()), AES-128-GCM (seal()/open(), the demo key
// baked in as a member instead of a void* ctx you cast back by hand), the
// COMMAND_REQUEST handler (command()) and the TERMINAL_IN handler
// (terminal()). A real deployment's send() hands the frame to ESP-NOW / a
// UART / USB-HID instead.
class ProducerLink : public btp::NodeConfig {
public:
    // The node hands every finished frame here. Fake: a real node transmits it.
    bool send(const std::uint8_t* /*frame*/, std::size_t /*n*/) override {
        return true;
    }

    // CIPHER_ID on `header` is already AesGcm (fragmenting_flags() never
    // sets those bits, and AesGcm is 0), so aead_seal()/aead_open()
    // dispatch there on their own; the KEY is the only thing living
    // here, never inside BTP itself.
    bool has_seal() const noexcept override { return true; }
    bool seal(const btp::Header& header, std::uint16_t payload_size,
              const std::uint8_t* plaintext, std::uint8_t* out) override {
        const btp::AeadKey key{kDemoAeadKey, btp::kAesGcmKeySize};
        return btp::aead_seal(key, header, payload_size, plaintext, out) ==
               btp::AeadError::Ok;
    }

    bool has_open() const noexcept override { return true; }
    bool open(const btp::Header& header, std::uint16_t payload_size,
              const std::uint8_t* plaintext, std::uint8_t* out_plaintext) override {
        const btp::AeadKey key{kDemoAeadKey, btp::kAesGcmKeySize};
        return btp::aead_open(key, header, payload_size, plaintext,
                              out_plaintext) == btp::AeadError::Ok;
    }

    // node.enable_commands() calls this for a Fresh COMMAND_REQUEST (docs/
    // commands.md section 2). `outcome` arrives pre-set to Success / no
    // message / no result / not pending -- good news needs no field touched
    // at all.
    //
    // 0x0002 ("self_test") shows the OTHER shape: a slow action sets
    // outcome->pending = true and saves `ticket` instead of answering right
    // here -- sender.cpp's loop stands in for "some other task", and calls
    // node.complete_command(ticket, real_outcome) a few passes later, once
    // the (simulated) work is done. Nothing goes out until then; a
    // retransmission arriving in the meantime is dropped as
    // DuplicateInFlight, same as a slow SYNCHRONOUS handler already gets.
    // Only one can be outstanding at a time in this demo -- a real
    // deployment sizes as many ticket slots as it can have actions running
    // at once.
    bool has_command() const noexcept override { return true; }
    void command(std::uint16_t action_id, std::uint16_t /*action_version*/,
                btp::ByteView /*parameters*/,
                btp::NodeActionOutcome* outcome,
                const btp::NodeCommandTicket& ticket) override {
        switch (action_id) {
            case 0x0001U:  // e.g. "stop" -- no parameters, answered right away
                // ... actually stop the robot ...
                break;     // outcome is already Success
            case 0x0002U:  // "self_test" -- pretend it takes a while
                pending_ticket_ = ticket;
                has_pending_ = true;
                outcome->pending = true;
                break;
            default:
                outcome->status = static_cast<std::uint8_t>(btp::ResultStatus::Rejected);
                outcome->error_code = static_cast<std::uint16_t>(btp::ResultError::NotFound);
                break;
        }
    }

    // sender.cpp's loop polls this instead of a real task-completion queue --
    // see the comment on command() above.
    bool has_pending_command() const noexcept { return has_pending_; }
    const btp::NodeCommandTicket& pending_command() const noexcept {
        return pending_ticket_;
    }
    void clear_pending_command() noexcept { has_pending_ = false; }

    // TERMINAL has no other btp::Node support -- its payload is opaque
    // bytes, no struct, on purpose (docs/session-and-terminal.md section
    // 7). Called directly for a TERMINAL_IN frame; `node`/`now_ms` arrive
    // straight from receive() itself, so answering back needs nothing
    // threaded in via a ctx. Here it just echoes the bytes back as
    // TERMINAL_OUT -- a real shell would feed `payload` to a line editor
    // instead.
    bool has_terminal() const noexcept override { return true; }
    void terminal(btp::Node& node, const btp::Header& /*header*/,
                  btp::ByteView payload, std::uint64_t now_ms) override {
        node.send(btp::MessageType::Terminal, btp::object_id::kTerminalOut,
                  payload.data, payload.size, now_ms * 1000ULL);
    }

private:
    // Backs the 0x0002 ("self_test") demo above -- one ticket at a time, no
    // queue, this is illustration, not a pattern to copy verbatim.
    btp::NodeCommandTicket pending_ticket_{};
    bool has_pending_ = false;
};

// Everything btp::Node calls out to for receiver.cpp's consumer, in one
// class: the fake link (send()), AES-128-GCM (seal()/open()) and the
// TERMINAL_OUT handler (terminal()). A real deployment's send() hands the
// frame to ESP-NOW / a UART / USB-HID instead.
class ConsumerLink : public btp::NodeConfig {
public:
    // The node hands every finished frame here. Fake: a real node
    // transmits it. Needed now that this node also connect()s /
    // subscribe()s out.
    bool send(const std::uint8_t* /*frame*/, std::size_t /*n*/) override {
        return true;
    }

    // CIPHER_ID on `header` is already AesGcm (fragmenting_flags() never
    // sets those bits, and AesGcm is 0), so aead_seal()/aead_open()
    // dispatch there on their own; the KEY is the only thing living
    // here, never inside BTP itself.
    bool has_seal() const noexcept override { return true; }
    bool seal(const btp::Header& header, std::uint16_t payload_size,
              const std::uint8_t* plaintext, std::uint8_t* out) override {
        const btp::AeadKey key{kDemoAeadKey, btp::kAesGcmKeySize};
        return btp::aead_seal(key, header, payload_size, plaintext, out) ==
               btp::AeadError::Ok;
    }

    bool has_open() const noexcept override { return true; }
    bool open(const btp::Header& header, std::uint16_t sealed_size,
              const std::uint8_t* sealed, std::uint8_t* out_plaintext) override {
        const btp::AeadKey key{kDemoAeadKey, btp::kAesGcmKeySize};
        return btp::aead_open(key, header, sealed_size, sealed,
                              out_plaintext) == btp::AeadError::Ok;
    }

    // Called for a TERMINAL_OUT frame -- see ProducerLink::terminal() above
    // for the other direction. A real shell would render `payload` to the
    // console instead.
    bool has_terminal() const noexcept override { return true; }
    void terminal(btp::Node& /*node*/, const btp::Header& /*header*/,
                  btp::ByteView /*payload*/, std::uint64_t /*now_ms*/) override {}
};

}  // namespace example

#endif  // BTP_EXAMPLE_NODE_CONFIG_HPP
