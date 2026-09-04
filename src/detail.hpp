#ifndef BTP_DETAIL_HPP
#define BTP_DETAIL_HPP

// Internal helpers shared by the translation units of btp::codec. This header
// is deliberately NOT under include/ and is not installed: it is not part of
// the public API and carries no compatibility promise.
//
// It exists because codec.cpp and fragmentation.cpp both have to answer the
// same two questions -- "are these transport limits usable" and "is this
// CIPHER_ID consistent with these flags" -- and two copies of a validation
// rule are two places for it to drift.

#include "btp/codec.hpp"

#include <cstdint>

namespace btp {
namespace detail {

// No fixed list of named profiles to check membership against any more
// (TransportLimits is caller-constructed, not an enum) -- just that there is
// enough room for the 40-octet header+CRC floor. There is no independent
// payload ceiling to cross-check any more (max_payload_size() derives it),
// so this is the one thing left that can be wrong.
inline bool valid_transport(const TransportLimits& transport) noexcept {
    return transport.max_frame_size >= kV1MinimumFrameSize;
}

// docs/encryption.md section 3: with ENCRYPTED clear there is no cipher in
// use, so CIPHER_ID must be 0; with ENCRYPTED set, CIPHER_ID must be 0 or 1
// (the only assigned values) -- 2 and 3 are reserved for future ciphers and
// are rejected, the same principle already applied to reserved flag bits.
inline bool valid_cipher_id_for_flags(std::uint16_t flags) noexcept {
    const std::uint16_t raw_cipher_id =
        static_cast<std::uint16_t>((flags & kCipherIdMask) >> kCipherIdShift);
    if ((flags & kFlagEncrypted) == 0U) {
        return raw_cipher_id == 0U;
    }
    return raw_cipher_id <=
           static_cast<std::uint16_t>(CipherId::ChaCha20Poly1305);
}

}  // namespace detail
}  // namespace btp

#endif  // BTP_DETAIL_HPP
