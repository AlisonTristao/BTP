#include "btp/aead.hpp"

// btp::aead is optional. Under CMake it is a separate target behind
// BTP_ENABLE_AEAD, so this file simply is not compiled when the option is off.
// PlatformIO has no such switch -- library.json's srcDir pulls in every .cpp
// unconditionally -- so on a platform whose SDK ships no crypto backend this
// would be a hard build error for a feature the user may not even want.
// Compile the implementation only when a backend's headers are actually
// reachable, and let CMake, which knows it asked for one, fail loudly if none
// is.
//
// ---------------------------------------------------------------------------
// Backend selection
// ---------------------------------------------------------------------------
//
// Two backends implement the same four primitives:
//
//   BTP_AEAD_BACKEND_CLASSIC -- mbedtls's own <mbedtls/gcm.h> and
//       <mbedtls/chachapoly.h>. Present in mbedtls 2.x and 3.x, which is what
//       the Arduino ESP32 SDK ships and what this project's CMake build
//       fetches.
//   BTP_AEAD_BACKEND_PSA -- the PSA Crypto API, <psa/crypto.h>. In mbedtls 4.x
//       / TF-PSA-Crypto (ESP-IDF 6.x) gcm.h and chachapoly.h moved to
//       mbedtls/private/ and are no longer part of the public surface; PSA is
//       the supported way to reach the same primitives there.
//
// The order is classic first, PSA second, and that is deliberate rather than
// alphabetical: the classic branch is what already runs in production on every
// consumer whose headers are public (Arduino ESP32 firmware, desktop CMake
// builds). Switching those to a different backend would be risk without gain,
// so as long as <mbedtls/gcm.h> is reachable the generated code stays exactly
// what it was. PSA is picked up only where the classic headers are gone, which
// is precisely the case that has no backend today.
//
// Both are byte-for-byte interchangeable on the wire -- they compute the same
// AES-128-GCM and ChaCha20-Poly1305 over the same nonce and AAD -- so a peer
// built against one interoperates with a peer built against the other. The
// v2 conformance vectors are the check on that.
//
// Either macro may be predefined on the command line to force a backend, which
// is how the PSA branch is compile-tested on a host whose mbedtls is 3.x.
#if defined(BTP_AEAD_BACKEND_CLASSIC) && defined(BTP_AEAD_BACKEND_PSA)
#  error "define at most one of BTP_AEAD_BACKEND_CLASSIC / BTP_AEAD_BACKEND_PSA"
#endif

#if !defined(BTP_AEAD_BACKEND_CLASSIC) && !defined(BTP_AEAD_BACKEND_PSA)
#  if defined(__has_include)
#    if __has_include(<mbedtls/gcm.h>) && __has_include(<mbedtls/chachapoly.h>)
#      define BTP_AEAD_BACKEND_CLASSIC 1
#    elif __has_include(<psa/crypto.h>)
#      define BTP_AEAD_BACKEND_PSA 1
#    endif
#  endif
#endif

#if defined(BTP_AEAD_BACKEND_CLASSIC) || defined(BTP_AEAD_BACKEND_PSA)
#  define BTP_AEAD_HAVE_BACKEND 1
#endif

// BTP_AEAD_REQUIRE_BACKEND is set by a build system that has already resolved
// and linked a crypto library, so a file that quietly compiled to nothing --
// and would fail at link time with an unhelpful message about missing
// btp::aead_seal -- must be a configure-time error instead.
//
// It is satisfied by EITHER backend. The gate used to be named
// BTP_AEAD_REQUIRE_MBEDTLS and, by its name, asserted specifically that
// <mbedtls/gcm.h> had been found; keeping that meaning would now break the
// legitimate case of building against a system mbedtls 4.x, where the classic
// headers are absent but PSA works. The old spelling is still honoured so an
// out-of-tree build that sets it keeps its error.
#if (defined(BTP_AEAD_REQUIRE_BACKEND) || defined(BTP_AEAD_REQUIRE_MBEDTLS)) && \
    !defined(BTP_AEAD_HAVE_BACKEND)
#  error "btp::aead was requested but neither <mbedtls/gcm.h> nor <psa/crypto.h> was found"
#endif

#if defined(BTP_AEAD_HAVE_BACKEND)

#if defined(BTP_AEAD_BACKEND_CLASSIC)
#include <mbedtls/chachapoly.h>
#include <mbedtls/gcm.h>
#else
#include <psa/crypto.h>
#endif

#include <new>  // placement-new for AeadCipher::storage_ (no heap involved)

namespace btp {

namespace {

const std::size_t kTagSize = 16U;
const std::size_t kNonceSize = 12U;
const std::size_t kAadSize = 36U;

// The AAD is the header of the LOGICAL message, never of one fragment
// (docs/encryption.md section 5). The tag is computed once, before
// fragmenting, so the fragmentation fields cannot be part of what it
// authenticates: they differ per fragment, and the sealing side has no single
// value to put there.
// Canonicalizing them here -- FRAGMENTED cleared, index 0, count 1 -- makes
// both sides agree by construction instead of by convention, so aead_open()
// accepts the header decoded from any fragment of the message.
//
// It also keeps the tag independent of how the message got fragmented, which
// matters whenever a gateway relays between transports with different payload
// ceilings (kEspNowMaxPayloadSize vs kSerialMaxPayloadSize): it may
// re-fragment without holding the key.
//
// For an unfragmented message this is a no-op -- FRAGMENTED is already clear
// and the fields are already 0/1 -- so every existing conformance vector
// keeps producing byte-identical AAD.
Header aad_header(const Header& header) noexcept {
    Header canonical = header;
    canonical.flags = static_cast<std::uint16_t>(
        canonical.flags & static_cast<std::uint16_t>(~kFlagFragmented));
    canonical.fragment_index = 0U;
    canonical.fragment_count = 1U;
    return canonical;
}

// wire_payload_size is what the AAD records: the size of the whole logical
// payload as it appears on the wire, ciphertext plus tag. A sealer therefore
// passes plaintext_size + kTagSize, while an opener passes ciphertext_size
// unchanged because it already includes the tag.
//
// Shared by every stateless entry point below AND by AeadCipher::seal() /
// open() (this file, further down): the two differ only in whether the key
// is imported fresh on this call (prepare(), which does that too) or was
// already imported by a prior AeadCipher::reset() (AeadCipher's own methods,
// which have no key to validate here and call this directly) -- the AAD /
// nonce construction is identical either way and both must agree with it
// byte for byte, so it is written once.
AeadError build_aad_and_nonce(const Header& header,
                              std::uint16_t wire_payload_size,
                              std::uint8_t out_aad[kAadSize],
                              std::uint8_t out_nonce[kNonceSize]) noexcept {
    if (encode_header(aad_header(header), wire_payload_size, out_aad) !=
        Error::Ok) {
        return AeadError::InvalidArgument;
    }
    aead_nonce(header, out_nonce);
    return AeadError::Ok;
}

// All four stateless entry points validate the same arguments and build the
// same two buffers. Doing it once keeps that -- the subtle part, and the one
// both peers must agree on byte for byte -- from being spelled out four times
// and drifting in one of them.
//
// Pointer rules follow the rest of the library: an input may be null only when
// its size is zero, and an output buffer is always required.
AeadError prepare(const AeadKey& key,
                  std::size_t expected_key_size,
                  const Header& header,
                  std::uint16_t wire_payload_size,
                  const std::uint8_t* input,
                  std::size_t input_size,
                  const std::uint8_t* output,
                  std::uint8_t out_aad[kAadSize],
                  std::uint8_t out_nonce[kNonceSize]) noexcept {
    if (key.data == nullptr || key.size != expected_key_size) {
        return AeadError::InvalidArgument;
    }
    if (input == nullptr && input_size != 0U) {
        return AeadError::InvalidArgument;
    }
    if (output == nullptr) {
        return AeadError::InvalidArgument;
    }
    return build_aad_and_nonce(header, wire_payload_size, out_aad, out_nonce);
}

// Guards the plaintext_size + kTagSize sum a sealer has to compute: the size
// is already a uint16, so a caller near the top of the range would wrap it
// silently and authenticate a size no receiver can reproduce.
bool seal_size_fits(std::uint16_t payload_size) noexcept {
    return payload_size <= static_cast<std::uint16_t>(0xFFFFU - kTagSize);
}

// prepare() deliberately permits a null input when its size is zero, matching
// the codec contract. Some classic mbedTLS ports (notably ESP32's hardware
// AES-GCM wrapper) still reject a null pointer before considering the length.
// A valid dummy address keeps the public contract portable and is read zero
// times, so it cannot affect ciphertext or authentication tags.
const std::uint8_t kEmptyInput[1] = {0U};

const std::uint8_t* non_null(const std::uint8_t* input) noexcept {
    return input != nullptr ? input : kEmptyInput;
}

}  // namespace

#if defined(BTP_AEAD_BACKEND_CLASSIC)

AeadError aead_seal_aes_gcm(const AeadKey& key, const Header& header,
                            std::uint16_t payload_size,
                            const std::uint8_t* plaintext,
                            std::uint8_t* out_ciphertext_and_tag) noexcept {
    if (!seal_size_fits(payload_size)) {
        return AeadError::InvalidArgument;
    }

    std::uint8_t aad[kAadSize];
    std::uint8_t nonce[kNonceSize];
    const AeadError prepared = prepare(
        key, kAesGcmKeySize, header,
        static_cast<std::uint16_t>(payload_size + kTagSize), plaintext,
        payload_size, out_ciphertext_and_tag, aad, nonce);
    if (prepared != AeadError::Ok) {
        return prepared;
    }

    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);

    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key.data,
                                static_cast<unsigned int>(key.size * 8U));
    if (rc == 0) {
        rc = mbedtls_gcm_crypt_and_tag(
            &ctx, MBEDTLS_GCM_ENCRYPT, payload_size, nonce, sizeof(nonce),
            aad, sizeof(aad), non_null(plaintext), out_ciphertext_and_tag, kTagSize,
            out_ciphertext_and_tag + payload_size);
    }

    mbedtls_gcm_free(&ctx);
    return rc == 0 ? AeadError::Ok : AeadError::InvalidArgument;
}

AeadError aead_open_aes_gcm(const AeadKey& key, const Header& header,
                            std::uint16_t ciphertext_size,
                            const std::uint8_t* ciphertext_and_tag,
                            std::uint8_t* out_plaintext) noexcept {
    if (ciphertext_size < kTagSize) {
        return AeadError::InvalidArgument;
    }

    std::uint8_t aad[kAadSize];
    std::uint8_t nonce[kNonceSize];
    const AeadError prepared =
        prepare(key, kAesGcmKeySize, header, ciphertext_size,
                ciphertext_and_tag, ciphertext_size, out_plaintext, aad, nonce);
    if (prepared != AeadError::Ok) {
        return prepared;
    }

    const std::size_t plaintext_size =
        static_cast<std::size_t>(ciphertext_size) - kTagSize;

    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);

    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key.data,
                                static_cast<unsigned int>(key.size * 8U));
    if (rc == 0) {
        rc = mbedtls_gcm_auth_decrypt(
            &ctx, plaintext_size, nonce, sizeof(nonce), aad, sizeof(aad),
            ciphertext_and_tag + plaintext_size, kTagSize, ciphertext_and_tag,
            out_plaintext);
    }

    mbedtls_gcm_free(&ctx);

    if (rc == 0) {
        return AeadError::Ok;
    }
    if (rc == MBEDTLS_ERR_GCM_AUTH_FAILED) {
        return AeadError::TagMismatch;
    }
    return AeadError::InvalidArgument;
}

AeadError aead_seal_chacha20poly1305(
    const AeadKey& key, const Header& header, std::uint16_t payload_size,
    const std::uint8_t* plaintext,
    std::uint8_t* out_ciphertext_and_tag) noexcept {
    if (!seal_size_fits(payload_size)) {
        return AeadError::InvalidArgument;
    }

    std::uint8_t aad[kAadSize];
    std::uint8_t nonce[kNonceSize];
    const AeadError prepared = prepare(
        key, kChaCha20Poly1305KeySize, header,
        static_cast<std::uint16_t>(payload_size + kTagSize), plaintext,
        payload_size, out_ciphertext_and_tag, aad, nonce);
    if (prepared != AeadError::Ok) {
        return prepared;
    }

    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);

    int rc = mbedtls_chachapoly_setkey(&ctx, key.data);
    if (rc == 0) {
        rc = mbedtls_chachapoly_encrypt_and_tag(
            &ctx, payload_size, nonce, aad, sizeof(aad), non_null(plaintext),
            out_ciphertext_and_tag, out_ciphertext_and_tag + payload_size);
    }

    mbedtls_chachapoly_free(&ctx);
    return rc == 0 ? AeadError::Ok : AeadError::InvalidArgument;
}

AeadError aead_open_chacha20poly1305(const AeadKey& key, const Header& header,
                                     std::uint16_t ciphertext_size,
                                     const std::uint8_t* ciphertext_and_tag,
                                     std::uint8_t* out_plaintext) noexcept {
    if (ciphertext_size < kTagSize) {
        return AeadError::InvalidArgument;
    }

    std::uint8_t aad[kAadSize];
    std::uint8_t nonce[kNonceSize];
    const AeadError prepared =
        prepare(key, kChaCha20Poly1305KeySize, header, ciphertext_size,
                ciphertext_and_tag, ciphertext_size, out_plaintext, aad, nonce);
    if (prepared != AeadError::Ok) {
        return prepared;
    }

    const std::size_t plaintext_size =
        static_cast<std::size_t>(ciphertext_size) - kTagSize;

    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);

    int rc = mbedtls_chachapoly_setkey(&ctx, key.data);
    if (rc == 0) {
        rc = mbedtls_chachapoly_auth_decrypt(
            &ctx, plaintext_size, nonce, aad, sizeof(aad),
            ciphertext_and_tag + plaintext_size, ciphertext_and_tag,
            out_plaintext);
    }

    mbedtls_chachapoly_free(&ctx);

    if (rc == 0) {
        return AeadError::Ok;
    }
    if (rc == MBEDTLS_ERR_CHACHAPOLY_AUTH_FAILED) {
        return AeadError::TagMismatch;
    }
    return AeadError::InvalidArgument;
}

// ---------------------------------------------------------------------------
// AeadCipher (classic backend) -- the key schedule / GHASH table live in
// storage_, imported once by reset(), reused by every seal() / open().
// ---------------------------------------------------------------------------

namespace {

// storage_ is raw bytes; these just name the two ways this file interprets
// it, matching whichever cipher_ says is actually live. Never both at once --
// reset() destroys the previous object (of whichever type it was) before
// placement-constructing the new one.
mbedtls_gcm_context* as_gcm(unsigned char* storage) noexcept {
    return reinterpret_cast<mbedtls_gcm_context*>(storage);
}
mbedtls_chachapoly_context* as_chachapoly(unsigned char* storage) noexcept {
    return reinterpret_cast<mbedtls_chachapoly_context*>(storage);
}

}  // namespace

// A silent overflow here would be storage_ corruption the first time a
// context bigger than the buffer got placement-constructed into it -- this
// turns that into a build failure instead, the moment either struct grows
// past what AeadCipher was sized for.
static_assert(sizeof(mbedtls_gcm_context) <= kAeadCipherStorageSize,
             "AeadCipher::storage_ is too small for mbedtls_gcm_context -- "
             "raise kAeadCipherStorageSize in include/btp/aead.hpp");
static_assert(sizeof(mbedtls_chachapoly_context) <= kAeadCipherStorageSize,
             "AeadCipher::storage_ is too small for mbedtls_chachapoly_context "
             "-- raise kAeadCipherStorageSize in include/btp/aead.hpp");

AeadCipher::AeadCipher() noexcept
    : storage_(), cipher_(CipherId::AesGcm), valid_(false) {}

AeadCipher::~AeadCipher() noexcept {
    if (!valid_) {
        return;
    }
    if (cipher_ == CipherId::AesGcm) {
        mbedtls_gcm_free(as_gcm(storage_));
    } else {
        mbedtls_chachapoly_free(as_chachapoly(storage_));
    }
}

bool AeadCipher::reset(CipherId cipher, const AeadKey& key) noexcept {
    if (valid_) {
        if (cipher_ == CipherId::AesGcm) {
            mbedtls_gcm_free(as_gcm(storage_));
        } else {
            mbedtls_chachapoly_free(as_chachapoly(storage_));
        }
        valid_ = false;
    }

    if (cipher == CipherId::AesGcm) {
        if (key.data == nullptr || key.size != kAesGcmKeySize) {
            return false;
        }
        mbedtls_gcm_context* ctx = new (storage_) mbedtls_gcm_context;
        mbedtls_gcm_init(ctx);
        const int rc = mbedtls_gcm_setkey(ctx, MBEDTLS_CIPHER_ID_AES, key.data,
                                          static_cast<unsigned int>(key.size * 8U));
        if (rc != 0) {
            mbedtls_gcm_free(ctx);
            return false;
        }
        cipher_ = cipher;
        valid_ = true;
        return true;
    }
    if (cipher == CipherId::ChaCha20Poly1305) {
        if (key.data == nullptr || key.size != kChaCha20Poly1305KeySize) {
            return false;
        }
        mbedtls_chachapoly_context* ctx = new (storage_) mbedtls_chachapoly_context;
        mbedtls_chachapoly_init(ctx);
        const int rc = mbedtls_chachapoly_setkey(ctx, key.data);
        if (rc != 0) {
            mbedtls_chachapoly_free(ctx);
            return false;
        }
        cipher_ = cipher;
        valid_ = true;
        return true;
    }
    return false;
}

AeadError AeadCipher::seal(const Header& header, std::uint16_t payload_size,
                           const std::uint8_t* plaintext,
                           std::uint8_t* out_ciphertext_and_tag) noexcept {
    if (!valid_) {
        return AeadError::InvalidArgument;  // reset() never succeeded (or failed)
    }
    if (cipher_id(header.flags) != cipher_) {
        return AeadError::InvalidCipherId;
    }
    if (!seal_size_fits(payload_size)) {
        return AeadError::InvalidArgument;
    }
    if (plaintext == nullptr && payload_size != 0U) {
        return AeadError::InvalidArgument;
    }
    if (out_ciphertext_and_tag == nullptr) {
        return AeadError::InvalidArgument;
    }

    std::uint8_t aad[kAadSize];
    std::uint8_t nonce[kNonceSize];
    if (build_aad_and_nonce(header, static_cast<std::uint16_t>(payload_size + kTagSize),
                            aad, nonce) != AeadError::Ok) {
        return AeadError::InvalidArgument;
    }

    int rc;
    if (cipher_ == CipherId::AesGcm) {
        rc = mbedtls_gcm_crypt_and_tag(
            as_gcm(storage_), MBEDTLS_GCM_ENCRYPT, payload_size, nonce,
            sizeof(nonce), aad, sizeof(aad), non_null(plaintext),
            out_ciphertext_and_tag, kTagSize,
            out_ciphertext_and_tag + payload_size);
    } else {
        rc = mbedtls_chachapoly_encrypt_and_tag(
            as_chachapoly(storage_), payload_size, nonce, aad, sizeof(aad),
            non_null(plaintext), out_ciphertext_and_tag,
            out_ciphertext_and_tag + payload_size);
    }
    return rc == 0 ? AeadError::Ok : AeadError::InvalidArgument;
}

AeadError AeadCipher::open(const Header& header, std::uint16_t ciphertext_size,
                           const std::uint8_t* ciphertext_and_tag,
                           std::uint8_t* out_plaintext) noexcept {
    if (!valid_) {
        return AeadError::InvalidArgument;  // reset() never succeeded (or failed)
    }
    if (cipher_id(header.flags) != cipher_) {
        return AeadError::InvalidCipherId;
    }
    if (ciphertext_size < kTagSize) {
        return AeadError::InvalidArgument;
    }
    if (ciphertext_and_tag == nullptr || out_plaintext == nullptr) {
        return AeadError::InvalidArgument;
    }

    std::uint8_t aad[kAadSize];
    std::uint8_t nonce[kNonceSize];
    if (build_aad_and_nonce(header, ciphertext_size, aad, nonce) != AeadError::Ok) {
        return AeadError::InvalidArgument;
    }
    const std::size_t plaintext_size =
        static_cast<std::size_t>(ciphertext_size) - kTagSize;

    int rc;
    if (cipher_ == CipherId::AesGcm) {
        rc = mbedtls_gcm_auth_decrypt(
            as_gcm(storage_), plaintext_size, nonce, sizeof(nonce), aad,
            sizeof(aad), ciphertext_and_tag + plaintext_size, kTagSize,
            ciphertext_and_tag, out_plaintext);
        if (rc == 0) return AeadError::Ok;
        return rc == MBEDTLS_ERR_GCM_AUTH_FAILED ? AeadError::TagMismatch
                                                  : AeadError::InvalidArgument;
    }
    rc = mbedtls_chachapoly_auth_decrypt(
        as_chachapoly(storage_), plaintext_size, nonce, aad, sizeof(aad),
        ciphertext_and_tag + plaintext_size, ciphertext_and_tag, out_plaintext);
    if (rc == 0) return AeadError::Ok;
    return rc == MBEDTLS_ERR_CHACHAPOLY_AUTH_FAILED ? AeadError::TagMismatch
                                                     : AeadError::InvalidArgument;
}

#else  // BTP_AEAD_BACKEND_PSA

// PSA backend (mbedtls 4.x / TF-PSA-Crypto, as shipped by ESP-IDF 6.x).
//
// PSA requires a process-wide psa_crypto_init() before any operation, which
// the classic backend has no equivalent of. This file makes that call itself,
// lazily, rather than declaring it a caller precondition -- see
// ensure_initialized() below for why, and note that it means the two backends
// have exactly the same contract: nothing to call, nothing to remember.

namespace {

// The tag length is pinned to kTagSize rather than left at the algorithm
// default, so the wire format stays exactly what docs/encryption.md section 2
// specifies regardless of any implementation default.
const psa_algorithm_t kAlgAesGcm =
    PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_GCM, kTagSize);
const psa_algorithm_t kAlgChaCha20Poly1305 =
    PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CHACHA20_POLY1305, kTagSize);

// Calls psa_crypto_init() once, on the first operation that needs it.
//
// The alternative -- documenting it as something the caller must do at boot --
// was tried and rejected, because of how it fails: PSA answers every later
// call with PSA_ERROR_BAD_STATE, so forgetting the call does not break one
// frame, it breaks all of them, from the first one, with an error that reads
// like a bad argument rather than a missing setup step. On a robot that would
// present as "the link never comes up", pointing an investigation at the radio
// or at the key material instead of at a one-line omission in boot.
//
// The cost this avoids is real but small: after the first success the steady
// state is a load and a branch, with no call into PSA and so no lock on the
// path of a firmware that seals every frame.
//
// A failed init is deliberately NOT cached. Early in boot psa_crypto_init()
// can fail because its entropy source is not ready yet, and caching that would
// turn a transient condition into a permanent one; retrying costs nothing
// once, since the retry only happens while the state is still failing.
//
// The flag is a plain bool with no synchronization. Two tasks racing here at
// most both call psa_crypto_init(), which PSA defines as idempotent and safe
// to call more than once, and both then write the same value -- so the race
// has no outcome other than one redundant call.
psa_status_t ensure_initialized() noexcept {
    static bool initialized = false;
    if (initialized) {
        return PSA_SUCCESS;
    }
    const psa_status_t status = psa_crypto_init();
    if (status == PSA_SUCCESS) {
        initialized = true;
    }
    return status;
}

// Owns exactly one imported key for the duration of one operation.
//
// A leaked key identifier is the failure this class exists to prevent: the PSA
// keystore is a fixed-size table, and a firmware that seals every frame would
// exhaust it within minutes of uptime, turning a resource leak into a total
// loss of the link. Destruction happens in the destructor, so it covers every
// exit -- a failed import, each return in the error mapping, and the success
// path alike -- without any of them having to remember to.
//
// Written for C++11 (see target_compile_features in CMakeLists.txt): no
// defaulted or deleted special members, copying suppressed the pre-11 way.
class PsaKey {
  public:
    PsaKey() noexcept : id_(MBEDTLS_SVC_KEY_ID_INIT) {}

    ~PsaKey() {
        // psa_destroy_key() on a null identifier is defined as a no-op
        // returning PSA_SUCCESS, so this is correct even when the import never
        // ran or failed. There is nothing useful to do with the status here.
        (void)psa_destroy_key(id_);
    }

    // Imports raw key material under a single-purpose policy: one algorithm,
    // one usage. A key imported for sealing therefore cannot be used to open,
    // and PSA enforces that rather than this file.
    psa_status_t import(const AeadKey& key, psa_key_type_t type,
                        psa_algorithm_t alg, psa_key_usage_t usage) noexcept {
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, type);
        psa_set_key_bits(&attributes, key.size * 8U);
        psa_set_key_algorithm(&attributes, alg);
        psa_set_key_usage_flags(&attributes, usage);
        return psa_import_key(&attributes, key.data, key.size, &id_);
    }

    mbedtls_svc_key_id_t id() const noexcept { return id_; }

  private:
    PsaKey(const PsaKey&);
    PsaKey& operator=(const PsaKey&);

    mbedtls_svc_key_id_t id_;
};

// The most load-bearing few lines in this file.
//
// PSA_ERROR_INVALID_SIGNATURE means the tag did not verify: the message was
// forged, corrupted, replayed under a different header, or opened with the
// wrong key. It must stay distinguishable from every other failure, because
// the rest of the system counts tag rejections separately from argument
// errors -- one is a security event, the other is a caller bug.
//
// Everything else maps to InvalidArgument. AeadError offers no other outcome
// (InvalidCipherId belongs to the dispatchers), and this is exactly what the
// classic backend already does with any mbedtls return code that is neither 0
// nor its cipher's AUTH_FAILED, so the two backends report identically. The
// statuses that land here in practice are PSA_ERROR_BAD_STATE
// (psa_crypto_init() was never called), PSA_ERROR_NOT_SUPPORTED (the build
// left GCM or ChaCha20-Poly1305 out), PSA_ERROR_INVALID_ARGUMENT,
// PSA_ERROR_BUFFER_TOO_SMALL and PSA_ERROR_INSUFFICIENT_MEMORY.
AeadError from_psa(psa_status_t status) noexcept {
    if (status == PSA_SUCCESS) {
        return AeadError::Ok;
    }
    if (status == PSA_ERROR_INVALID_SIGNATURE) {
        return AeadError::TagMismatch;
    }
    return AeadError::InvalidArgument;
}

// The four entry points differ only in cipher, key type and key size; the call
// shape is identical, so it is written once. PSA writes ciphertext followed by
// the tag into one contiguous buffer, which is already the layout this
// protocol puts on the wire (docs/encryption.md section 2), so neither
// direction needs to move octets around afterwards. No dynamic allocation:
// aad and nonce are the same two stack buffers the classic backend uses, and
// PSA is handed the caller's own output buffer.
AeadError psa_seal(const AeadKey& key, std::size_t expected_key_size,
                   psa_key_type_t key_type, psa_algorithm_t alg,
                   const Header& header, std::uint16_t payload_size,
                   const std::uint8_t* plaintext,
                   std::uint8_t* out_ciphertext_and_tag) noexcept {
    if (!seal_size_fits(payload_size)) {
        return AeadError::InvalidArgument;
    }

    std::uint8_t aad[kAadSize];
    std::uint8_t nonce[kNonceSize];
    const AeadError prepared = prepare(
        key, expected_key_size, header,
        static_cast<std::uint16_t>(payload_size + kTagSize), plaintext,
        payload_size, out_ciphertext_and_tag, aad, nonce);
    if (prepared != AeadError::Ok) {
        return prepared;
    }

    psa_status_t status = ensure_initialized();
    if (status != PSA_SUCCESS) {
        return from_psa(status);
    }

    PsaKey psa_key;
    status = psa_key.import(key, key_type, alg, PSA_KEY_USAGE_ENCRYPT);
    if (status == PSA_SUCCESS) {
        std::size_t written = 0U;
        status = psa_aead_encrypt(
            psa_key.id(), alg, nonce, sizeof(nonce), aad, sizeof(aad),
            non_null(plaintext), payload_size, out_ciphertext_and_tag,
            static_cast<std::size_t>(payload_size) + kTagSize, &written);
    }
    return from_psa(status);
}

AeadError psa_open(const AeadKey& key, std::size_t expected_key_size,
                   psa_key_type_t key_type, psa_algorithm_t alg,
                   const Header& header, std::uint16_t ciphertext_size,
                   const std::uint8_t* ciphertext_and_tag,
                   std::uint8_t* out_plaintext) noexcept {
    if (ciphertext_size < kTagSize) {
        return AeadError::InvalidArgument;
    }

    std::uint8_t aad[kAadSize];
    std::uint8_t nonce[kNonceSize];
    const AeadError prepared =
        prepare(key, expected_key_size, header, ciphertext_size,
                ciphertext_and_tag, ciphertext_size, out_plaintext, aad, nonce);
    if (prepared != AeadError::Ok) {
        return prepared;
    }

    const std::size_t plaintext_size =
        static_cast<std::size_t>(ciphertext_size) - kTagSize;

    psa_status_t status = ensure_initialized();
    if (status != PSA_SUCCESS) {
        return from_psa(status);
    }

    PsaKey psa_key;
    status = psa_key.import(key, key_type, alg, PSA_KEY_USAGE_DECRYPT);
    if (status == PSA_SUCCESS) {
        std::size_t written = 0U;
        status = psa_aead_decrypt(psa_key.id(), alg, nonce, sizeof(nonce), aad,
                                  sizeof(aad), non_null(ciphertext_and_tag),
                                  ciphertext_size, out_plaintext,
                                  plaintext_size, &written);
    }
    return from_psa(status);
}

}  // namespace

// ---------------------------------------------------------------------------
// AeadCipher (PSA backend) -- one imported key id, live in storage_ from
// reset() until the next reset() or the destructor.
// ---------------------------------------------------------------------------

namespace {

mbedtls_svc_key_id_t* as_psa_key_id(unsigned char* storage) noexcept {
    return reinterpret_cast<mbedtls_svc_key_id_t*>(storage);
}

}  // namespace

// See the classic backend's own pair of these (above #else): same reasoning,
// a much smaller bound here since a PSA key id is not a cipher context.
static_assert(sizeof(mbedtls_svc_key_id_t) <= kAeadCipherStorageSize,
             "AeadCipher::storage_ is too small for a PSA key id -- raise "
             "kAeadCipherStorageSize in include/btp/aead.hpp");

AeadCipher::AeadCipher() noexcept
    : storage_(), cipher_(CipherId::AesGcm), valid_(false) {}

AeadCipher::~AeadCipher() noexcept {
    if (!valid_) {
        return;
    }
    (void)psa_destroy_key(*as_psa_key_id(storage_));
}

bool AeadCipher::reset(CipherId cipher, const AeadKey& key) noexcept {
    if (valid_) {
        (void)psa_destroy_key(*as_psa_key_id(storage_));
        valid_ = false;
    }

    psa_key_type_t type = PSA_KEY_TYPE_AES;
    psa_algorithm_t alg = kAlgAesGcm;
    std::size_t expected_size = kAesGcmKeySize;
    if (cipher == CipherId::ChaCha20Poly1305) {
        type = PSA_KEY_TYPE_CHACHA20;
        alg = kAlgChaCha20Poly1305;
        expected_size = kChaCha20Poly1305KeySize;
    } else if (cipher != CipherId::AesGcm) {
        return false;
    }
    if (key.data == nullptr || key.size != expected_size) {
        return false;
    }
    if (ensure_initialized() != PSA_SUCCESS) {
        return false;
    }

    // Both usages granted, unlike PsaKey's single-purpose import (aead_seal /
    // aead_open above): this object exists so a node can seal its own
    // traffic AND open its peer's under the one symmetric key, not two
    // separately-scoped one-way imports.
    mbedtls_svc_key_id_t* id =
        new (storage_) mbedtls_svc_key_id_t(MBEDTLS_SVC_KEY_ID_INIT);
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, type);
    psa_set_key_bits(&attributes, key.size * 8U);
    psa_set_key_algorithm(&attributes, alg);
    psa_set_key_usage_flags(&attributes,
                            PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    const psa_status_t status = psa_import_key(&attributes, key.data, key.size, id);
    if (status != PSA_SUCCESS) {
        return false;
    }
    cipher_ = cipher;
    valid_ = true;
    return true;
}

AeadError AeadCipher::seal(const Header& header, std::uint16_t payload_size,
                           const std::uint8_t* plaintext,
                           std::uint8_t* out_ciphertext_and_tag) noexcept {
    if (!valid_) {
        return AeadError::InvalidArgument;  // reset() never succeeded (or failed)
    }
    if (cipher_id(header.flags) != cipher_) {
        return AeadError::InvalidCipherId;
    }
    if (!seal_size_fits(payload_size)) {
        return AeadError::InvalidArgument;
    }
    if (plaintext == nullptr && payload_size != 0U) {
        return AeadError::InvalidArgument;
    }
    if (out_ciphertext_and_tag == nullptr) {
        return AeadError::InvalidArgument;
    }

    std::uint8_t aad[kAadSize];
    std::uint8_t nonce[kNonceSize];
    if (build_aad_and_nonce(header, static_cast<std::uint16_t>(payload_size + kTagSize),
                            aad, nonce) != AeadError::Ok) {
        return AeadError::InvalidArgument;
    }

    const psa_algorithm_t alg =
        cipher_ == CipherId::AesGcm ? kAlgAesGcm : kAlgChaCha20Poly1305;
    std::size_t written = 0U;
    const psa_status_t status = psa_aead_encrypt(
        *as_psa_key_id(storage_), alg, nonce, sizeof(nonce), aad, sizeof(aad),
        non_null(plaintext), payload_size, out_ciphertext_and_tag,
        static_cast<std::size_t>(payload_size) + kTagSize, &written);
    return from_psa(status);
}

AeadError AeadCipher::open(const Header& header, std::uint16_t ciphertext_size,
                           const std::uint8_t* ciphertext_and_tag,
                           std::uint8_t* out_plaintext) noexcept {
    if (!valid_) {
        return AeadError::InvalidArgument;  // reset() never succeeded (or failed)
    }
    if (cipher_id(header.flags) != cipher_) {
        return AeadError::InvalidCipherId;
    }
    if (ciphertext_size < kTagSize) {
        return AeadError::InvalidArgument;
    }
    if (ciphertext_and_tag == nullptr || out_plaintext == nullptr) {
        return AeadError::InvalidArgument;
    }

    std::uint8_t aad[kAadSize];
    std::uint8_t nonce[kNonceSize];
    if (build_aad_and_nonce(header, ciphertext_size, aad, nonce) != AeadError::Ok) {
        return AeadError::InvalidArgument;
    }
    const std::size_t plaintext_size =
        static_cast<std::size_t>(ciphertext_size) - kTagSize;

    const psa_algorithm_t alg =
        cipher_ == CipherId::AesGcm ? kAlgAesGcm : kAlgChaCha20Poly1305;
    std::size_t written = 0U;
    const psa_status_t status = psa_aead_decrypt(
        *as_psa_key_id(storage_), alg, nonce, sizeof(nonce), aad, sizeof(aad),
        non_null(ciphertext_and_tag), ciphertext_size, out_plaintext,
        plaintext_size, &written);
    return from_psa(status);
}

AeadError aead_seal_aes_gcm(const AeadKey& key, const Header& header,
                            std::uint16_t payload_size,
                            const std::uint8_t* plaintext,
                            std::uint8_t* out_ciphertext_and_tag) noexcept {
    return psa_seal(key, kAesGcmKeySize, PSA_KEY_TYPE_AES, kAlgAesGcm, header,
                    payload_size, plaintext, out_ciphertext_and_tag);
}

AeadError aead_open_aes_gcm(const AeadKey& key, const Header& header,
                            std::uint16_t ciphertext_size,
                            const std::uint8_t* ciphertext_and_tag,
                            std::uint8_t* out_plaintext) noexcept {
    return psa_open(key, kAesGcmKeySize, PSA_KEY_TYPE_AES, kAlgAesGcm, header,
                    ciphertext_size, ciphertext_and_tag, out_plaintext);
}

AeadError aead_seal_chacha20poly1305(
    const AeadKey& key, const Header& header, std::uint16_t payload_size,
    const std::uint8_t* plaintext,
    std::uint8_t* out_ciphertext_and_tag) noexcept {
    return psa_seal(key, kChaCha20Poly1305KeySize, PSA_KEY_TYPE_CHACHA20,
                    kAlgChaCha20Poly1305, header, payload_size, plaintext,
                    out_ciphertext_and_tag);
}

AeadError aead_open_chacha20poly1305(const AeadKey& key, const Header& header,
                                     std::uint16_t ciphertext_size,
                                     const std::uint8_t* ciphertext_and_tag,
                                     std::uint8_t* out_plaintext) noexcept {
    return psa_open(key, kChaCha20Poly1305KeySize, PSA_KEY_TYPE_CHACHA20,
                    kAlgChaCha20Poly1305, header, ciphertext_size,
                    ciphertext_and_tag, out_plaintext);
}

#endif  // BTP_AEAD_BACKEND_CLASSIC

AeadError aead_seal(const AeadKey& key, const Header& header,
                    std::uint16_t payload_size, const std::uint8_t* plaintext,
                    std::uint8_t* out_ciphertext_and_tag) noexcept {
    switch (cipher_id(header.flags)) {
        case CipherId::AesGcm:
            return aead_seal_aes_gcm(key, header, payload_size, plaintext,
                                     out_ciphertext_and_tag);
        case CipherId::ChaCha20Poly1305:
            return aead_seal_chacha20poly1305(key, header, payload_size,
                                              plaintext,
                                              out_ciphertext_and_tag);
    }
    return AeadError::InvalidCipherId;
}

AeadError aead_open(const AeadKey& key, const Header& header,
                    std::uint16_t ciphertext_size,
                    const std::uint8_t* ciphertext_and_tag,
                    std::uint8_t* out_plaintext) noexcept {
    switch (cipher_id(header.flags)) {
        case CipherId::AesGcm:
            return aead_open_aes_gcm(key, header, ciphertext_size,
                                     ciphertext_and_tag, out_plaintext);
        case CipherId::ChaCha20Poly1305:
            return aead_open_chacha20poly1305(key, header, ciphertext_size,
                                              ciphertext_and_tag,
                                              out_plaintext);
    }
    return AeadError::InvalidCipherId;
}

}  // namespace btp

#endif  // BTP_AEAD_HAVE_BACKEND
