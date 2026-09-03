#ifndef BTP_CATALOG_HPP
#define BTP_CATALOG_HPP

// A caller-owned schema catalogue: the TELEMETRY topics a producer EXPOSES, or
// the ones a consumer has LEARNED from a MANIFEST_DATA. btp::Node attaches one
// and keeps it current -- serving MANIFEST_DATA on the producer side, ingesting
// it on the consumer side -- so neither end hand-rolls the ManifestReader walk
// that example/*_receiver.cpp used to (docs/commands.md section 3).
//
// btp::messages gives the byte layout of MANIFEST_DATA (ManifestReader /
// ManifestWriter) and btp::telemetry the sample codec against a FieldSpec[].
// This is the piece between them the library kept above the wire
// (docs/library.md section 11.2): "which schemas a consumer caches" -- with the
// storage still the caller's, so it stays fixed-capacity on a microcontroller.
//
// Same guarantees as the rest of the library: no internal allocation, noexcept,
// no clock, no I/O, no global state.
//
// v1 covers TELEMETRY topics and their fields -- field_id, order, type, scale,
// offset, element counts, nullability, and the topic + field NAMES. NOT yet:
// units, descriptions, enum labels, action (command) records, the format-2
// source_info block. A round-tripped manifest keeps what the sample codec needs
// plus the names.

#include "btp/messages.hpp"   // ManifestReader/Writer, FieldRecord, TelemetryEncoding, MessageError
#include "btp/telemetry.hpp"  // FieldSpec

#include <cstddef>
#include <cstdint>

namespace btp {

// ---------------------------------------------------------------------------
// One topic in a Catalog
// ---------------------------------------------------------------------------
//
// `fields`, `name` and `field_names` point into the Catalog's caller-owned
// pools and are valid until the next add_topic() / ingest() / clear().

struct CatalogTopic {
    std::uint16_t topic_id;
    std::uint16_t schema_version;
    std::uint8_t encoding;   // TelemetryEncoding (PackedLe / TlvLe / ...)
    std::uint8_t flags;      // kTopicSubscribable
    std::uint32_t max_rate_millihz;

    const FieldSpec* fields;
    std::size_t field_count;

    const char* name;                // NUL-terminated; "" without a string pool
    const char* const* field_names;  // field_count NUL-terminated entries
};

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------

class Catalog {
public:
    // The caller owns every pool. `string_pool` / `name_ptr_pool` may be
    // {nullptr, 0}: names then come back as "" and a producer serialises empty
    // field names. btp::StaticCatalog bundles the four regions.
    Catalog(CatalogTopic* topics, std::size_t topic_capacity,
            FieldSpec* field_pool, std::size_t field_pool_capacity,
            const char** name_ptr_pool, std::size_t name_ptr_capacity,
            char* string_pool, std::size_t string_pool_capacity) noexcept;

    // True when the topic slots and the field pool are non-null and non-empty.
    // Check once after construction.
    bool valid() const noexcept { return valid_; }

    // Drop every topic and reset the pools. config_revision() is kept.
    void clear() noexcept;

    std::uint32_t config_revision() const noexcept { return config_revision_; }
    void set_config_revision(std::uint32_t revision) noexcept {
        config_revision_ = revision;
    }

    std::size_t topic_count() const noexcept { return topic_count_; }
    // nullptr when no topic has that id (or index is out of range).
    const CatalogTopic* topic(std::uint16_t topic_id) const noexcept;
    const CatalogTopic* topic_at(std::size_t index) const noexcept;

    // The name of field `index` of `t` -- "" when names were not kept or the
    // index is out of range, so it is always safe to print.
    const char* field_name(const CatalogTopic& t,
                           std::size_t index) const noexcept;

    // ----- producer: fill the catalogue by hand -----------------------------
    // Copies the topic metadata, field_spec() of each FieldRecord, and (when a
    // string pool is set) the topic name and each field's name.
    //   Ok                 -- stored.
    //   BufferTooSmall     -- a pool is full.
    //   InvalidArgument    -- a null pointer, field_count 0, a duplicate
    //                         topic_id, or field orders not 0,1,2,... .
    MessageError add_topic(std::uint16_t topic_id, std::uint16_t schema_version,
                           TelemetryEncoding encoding, bool subscribable,
                           std::uint32_t max_rate_millihz, const char* name,
                           const FieldRecord* fields,
                           std::size_t field_count) noexcept;

    // ----- consumer: learn from a MANIFEST_DATA payload ---------------------
    // Walks `payload` (a whole logical MANIFEST_DATA, post-reassembly) and
    // REPLACES the catalogue with the topics it describes. Also copies the
    // header's config_revision into config_revision().
    //   Ok                 -- replaced (or, on a NOT_MODIFIED response, kept).
    //   <ManifestReader error>  -- malformed manifest; catalogue left cleared.
    //   BufferTooSmall     -- a pool overflowed; catalogue left cleared.
    MessageError ingest(const std::uint8_t* payload, std::size_t size) noexcept;

    // ----- producer: serialise the topics into an open ManifestWriter -------
    // begin_topic / add_field / end_topic for every topic. The caller opened
    // `writer` with a header whose topic_count == topic_count(); afterwards it
    // adds actions (none in v1) and calls finish(). Field units and
    // descriptions are written empty.
    MessageError write_topics(ManifestWriter* writer) const noexcept;

private:
    // Copy into the string pool and return a NUL-terminated pointer, or "" when
    // there is no pool / no room.
    const char* intern(const char* s) noexcept;
    const char* intern(const std::uint8_t* data, std::size_t len) noexcept;
    bool has_topic(std::uint16_t topic_id) const noexcept;

    CatalogTopic* topics_;
    std::size_t topic_capacity_;
    std::size_t topic_count_;

    FieldSpec* field_pool_;
    std::size_t field_pool_capacity_;
    std::size_t field_pool_used_;

    const char** name_ptr_pool_;
    std::size_t name_ptr_capacity_;
    std::size_t name_ptr_used_;

    char* string_pool_;
    std::size_t string_pool_capacity_;
    std::size_t string_pool_used_;

    std::uint32_t config_revision_;
    bool valid_;
};

// ---------------------------------------------------------------------------
// StaticCatalog -- a Catalog that owns its pools
// ---------------------------------------------------------------------------

namespace detail {

template <std::size_t Topics, std::size_t Fields, std::size_t StringBytes>
struct CatalogStorage {
    CatalogTopic topics[Topics];
    FieldSpec field_pool[Fields];
    const char* name_ptr_pool[Fields];
    char string_pool[StringBytes];
};

}  // namespace detail

// Defaults: 8 topics, 64 field specs across them, 1 KiB of name text.
template <std::size_t Topics = 8, std::size_t Fields = 64,
          std::size_t StringBytes = 1024>
class StaticCatalog : private detail::CatalogStorage<Topics, Fields, StringBytes>,
                      public Catalog {
    using Storage = detail::CatalogStorage<Topics, Fields, StringBytes>;

public:
    StaticCatalog() noexcept
        : Storage(),
          Catalog(Storage::topics, Topics, Storage::field_pool, Fields,
                  Storage::name_ptr_pool, Fields, Storage::string_pool,
                  StringBytes) {}
};

}  // namespace btp

#endif  // BTP_CATALOG_HPP
