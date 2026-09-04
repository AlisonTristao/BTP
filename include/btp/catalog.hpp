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
// offset, element counts, nullability, the topic + field NAMES, and (v1.1)
// each field's UNIT and DESCRIPTION. (v1.2) also the format-2 source_info
// block -- the informational key/label/value rows a MANIFEST_DATA carries
// ahead of the topics (fw version, chip, partition; docs/commands.md section
// 3.12), producer-set with add_source_info() and consumer-read back with
// source_info_at() after ingest(); it needs its own small pool
// (StaticCatalog's SourceInfoEntries template argument). NOT yet: a topic's
// own description, enum labels, action (command) records. A round-tripped
// manifest keeps what the sample codec needs, the names, the field-level UI
// labels (unit/description) and the source_info rows; a topic-level
// description still does not round-trip.

#include "btp/messages.hpp"   // ManifestReader/Writer, FieldRecord, TelemetryEncoding, MessageError
#include "btp/telemetry.hpp"  // FieldSpec, SampleWriter, SampleLayout, the f32/u16/... helpers

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace btp {

class TopicBuilder;  // below Catalog -- topic() returns one

// ---------------------------------------------------------------------------
// One topic in a Catalog
// ---------------------------------------------------------------------------
//
// `fields`, `name`, `field_names`, `field_units` and `field_descriptions`
// point into the Catalog's caller-owned pools and are valid until the next
// add_topic() / ingest() / clear().

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
    // Both nullptr when their pool was not set (Catalog::field_unit() /
    // field_description() already fold that into "" -- reach through these
    // directly only to skip the per-call bounds check).
    const char* const* field_units;
    const char* const* field_descriptions;
};

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------

class Catalog {
public:
    // The caller owns every pool. `string_pool` / `name_ptr_pool` may be
    // {nullptr, 0}: names then come back as "" and a producer serialises empty
    // field names. `unit_ptr_pool` / `description_ptr_pool` are the same idea,
    // one field-sized pointer array each, both optional and independent of
    // `name_ptr_pool` and of each other -- keep names but not units, say, by
    // passing {nullptr, 0} for just the one not wanted. All interned strings
    // -- names, units, descriptions alike -- share the one `string_pool`.
    // btp::StaticCatalog bundles all six regions.
    Catalog(CatalogTopic* topics, std::size_t topic_capacity,
            FieldSpec* field_pool, std::size_t field_pool_capacity,
            const char** name_ptr_pool, std::size_t name_ptr_capacity,
            char* string_pool, std::size_t string_pool_capacity,
            const char** unit_ptr_pool = nullptr,
            std::size_t unit_ptr_capacity = 0U,
            const char** description_ptr_pool = nullptr,
            std::size_t description_ptr_capacity = 0U,
            SourceInfoEntry* source_info_pool = nullptr,
            std::size_t source_info_capacity = 0U) noexcept;

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

    // The name / unit / description of field `index` of `t` -- "" when that
    // pool was not kept or the index is out of range, so each is always safe
    // to print.
    const char* field_name(const CatalogTopic& t,
                           std::size_t index) const noexcept;
    const char* field_unit(const CatalogTopic& t,
                           std::size_t index) const noexcept;
    const char* field_description(const CatalogTopic& t,
                                  std::size_t index) const noexcept;

    // ----- producer: fill the catalogue by hand -----------------------------
    // Copies the topic metadata, field_spec() of each FieldRecord, and (when
    // the matching pool is set) the topic name and each field's name, unit and
    // description -- `fields[i].unit` / `.description` come along automatically
    // once the caller passes a non-empty one, whether built by hand, by
    // btp::f32(name, unit) and friends, or by TopicBuilder (which only ever
    // sets unit, never description -- see its own comment).
    //   Ok                 -- stored.
    //   BufferTooSmall     -- a pool is full.
    //   InvalidArgument    -- a null pointer, field_count 0, a duplicate
    //                         topic_id, or field orders not 0,1,2,... .
    MessageError add_topic(std::uint16_t topic_id, std::uint16_t schema_version,
                           TelemetryEncoding encoding, bool subscribable,
                           std::uint32_t max_rate_millihz, const char* name,
                           const FieldRecord* fields,
                           std::size_t field_count) noexcept;

    // The common case: a static FieldRecord[] whose length is deduced, PackedLe,
    // subscribable, no rate cap.
    //   catalog.add_topic(0x0101, /*schema_version=*/3, "drive_status", kSchema);
    template <std::size_t N>
    MessageError add_topic(std::uint16_t topic_id, std::uint16_t schema_version,
                           const char* name, const FieldRecord (&fields)[N],
                           TelemetryEncoding encoding = TelemetryEncoding::PackedLe,
                           bool subscribable = true,
                           std::uint32_t max_rate_millihz = 0U) noexcept {
        return add_topic(topic_id, schema_version, encoding, subscribable,
                         max_rate_millihz, name, fields, N);
    }

    // The chained alternative to naming a FieldRecord[] and calling
    // add_topic() on it: catalog.topic(0x0101, 3, "drive_status").f32(...)...
    // .end(). See TopicBuilder below. Nothing is stored until end() runs.
    TopicBuilder topic(std::uint16_t topic_id, std::uint16_t schema_version,
                       const char* name,
                       TelemetryEncoding encoding = TelemetryEncoding::PackedLe,
                       bool subscribable = true,
                       std::uint32_t max_rate_millihz = 0U) noexcept;

    // ----- producer: the format-2 source_info block ------------------------
    // Informational key/label/value rows (docs/commands.md section 3.12) --
    // fw version, chip id, running partition, ... -- copied into the string
    // pool and emitted ahead of the topic records by write_source_info()
    // (btp::Node's MANIFEST_DATA path calls that for you). An entry whose
    // `value` is empty is skipped (Ok, no row added); `label` may be empty.
    //   Ok               -- stored, or skipped because `value` was empty.
    //   InvalidArgument  -- no source_info pool (see the ctor / StaticCatalog).
    //   BufferTooSmall   -- the source_info pool or the string pool is full.
    // Call the whole set once; a second run appends and does not reclaim the
    // first run's interned bytes. clear() / a fresh ingest() drop them.
    MessageError add_source_info(const char* key, const char* label,
                                const char* value) noexcept;
    void clear_source_info() noexcept;
    std::size_t source_info_count() const noexcept { return source_info_count_; }
    bool has_source_info() const noexcept { return source_info_count_ != 0U; }
    // nullptr when index is out of range; otherwise a SourceInfoEntry whose
    // key/label/value ByteViews point into this Catalog's string pool (valid
    // until the next clear() / ingest()).
    const SourceInfoEntry* source_info_at(std::size_t index) const noexcept;

    // Serialise the stored source_info rows into an open ManifestWriter --
    // called by btp::Node between begin(header) and write_topics(). Adds every
    // row that still fits and STOPS (returning Ok) before one that would not:
    // source_info is informational and the topic records that follow it win
    // the remaining space. A real writer error (WrongOrder for a format-1
    // header, a framing fault) is returned as-is.
    MessageError write_source_info(ManifestWriter* writer) const noexcept;

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
    // descriptions round-trip (whatever field_unit()/field_description() would
    // return); the topic's own description is still written empty -- see this
    // header's top comment.
    MessageError write_topics(ManifestWriter* writer) const noexcept;

private:
    // Copy into the string pool and return a NUL-terminated pointer, or "" when
    // there is no pool / no room.
    const char* intern(const char* s) noexcept;
    const char* intern(const std::uint8_t* data, std::size_t len) noexcept;
    // intern(), as a ByteView -- {interned bytes, len}, or {"", 0} when the
    // input was empty or the pool had no room.
    ByteView intern_view(const std::uint8_t* data, std::size_t len) noexcept;
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

    const char** unit_ptr_pool_;
    std::size_t unit_ptr_capacity_;
    std::size_t unit_ptr_used_;

    const char** description_ptr_pool_;
    std::size_t description_ptr_capacity_;
    std::size_t description_ptr_used_;

    char* string_pool_;
    std::size_t string_pool_capacity_;
    std::size_t string_pool_used_;

    SourceInfoEntry* source_info_pool_;
    std::size_t source_info_capacity_;
    std::size_t source_info_count_;

    std::uint32_t config_revision_;
    bool valid_;
};

// ---------------------------------------------------------------------------
// TopicBuilder -- declare a topic's schema one field at a time
// ---------------------------------------------------------------------------
//
// The chained alternative to naming a FieldRecord[] and calling add_topic()
// on it (btp/telemetry.hpp "Schema-declaration helpers") -- the same one-line
// -per-field feel, for a caller that would rather not name a separate array:
//
//   catalog.topic(0x0101, /*schema_version=*/3, "drive_status")
//       .f32("left_rpm", "rpm")
//       .f32("right_rpm", "rpm")
//       .u16("battery_v", 0.001, "V")
//       .i16("temp_c", 0.1, "Cel", /*is_nullable=*/true)
//       .end();
//
// Fields are buffered on the STACK (kMaxFields -- a per-declaration cap,
// unrelated to the Catalog's own pool sizing) and committed with one
// add_topic() call at end(). A one-shot object built by Catalog::topic() and
// meant to be used and dropped right there -- not for a hot path, not kept
// around, not default-constructible.
//
// A chain call after an error (kMaxFields exceeded, or one end() already
// reported) is a no-op; end() returns the first error seen, the same "sticky
// error" rule as btp::SampleWriter / btp::ManifestWriter.

class TopicBuilder {
public:
    static const std::size_t kMaxFields = 32U;

    TopicBuilder& u8(const char* name, double scale = 1.0, const char* unit = "",
                     bool is_nullable = false) noexcept {
        return add(btp::u8(name, scale, unit), is_nullable);
    }
    TopicBuilder& u16(const char* name, double scale = 1.0,
                      const char* unit = "", bool is_nullable = false) noexcept {
        return add(btp::u16(name, scale, unit), is_nullable);
    }
    TopicBuilder& u32(const char* name, double scale = 1.0,
                      const char* unit = "", bool is_nullable = false) noexcept {
        return add(btp::u32(name, scale, unit), is_nullable);
    }
    TopicBuilder& u64(const char* name, double scale = 1.0,
                      const char* unit = "", bool is_nullable = false) noexcept {
        return add(btp::u64(name, scale, unit), is_nullable);
    }
    TopicBuilder& i8(const char* name, double scale = 1.0, const char* unit = "",
                     bool is_nullable = false) noexcept {
        return add(btp::i8(name, scale, unit), is_nullable);
    }
    TopicBuilder& i16(const char* name, double scale = 1.0,
                      const char* unit = "", bool is_nullable = false) noexcept {
        return add(btp::i16(name, scale, unit), is_nullable);
    }
    TopicBuilder& i32(const char* name, double scale = 1.0,
                      const char* unit = "", bool is_nullable = false) noexcept {
        return add(btp::i32(name, scale, unit), is_nullable);
    }
    TopicBuilder& i64(const char* name, double scale = 1.0,
                      const char* unit = "", bool is_nullable = false) noexcept {
        return add(btp::i64(name, scale, unit), is_nullable);
    }
    TopicBuilder& f32(const char* name, const char* unit = "",
                      bool is_nullable = false) noexcept {
        return add(btp::f32(name, unit), is_nullable);
    }
    TopicBuilder& f64(const char* name, const char* unit = "",
                      bool is_nullable = false) noexcept {
        return add(btp::f64(name, unit), is_nullable);
    }
    TopicBuilder& boolean(const char* name, bool is_nullable = false) noexcept {
        return add(btp::boolean(name), is_nullable);
    }
    TopicBuilder& enum8(const char* name, bool is_nullable = false) noexcept {
        return add(btp::enum8(name), is_nullable);
    }
    TopicBuilder& enum16(const char* name, bool is_nullable = false) noexcept {
        return add(btp::enum16(name), is_nullable);
    }

    // Explicit field_id -- schema evolution, mirrors btp::field(): a rename
    // or reorder that must keep an id in place.
    TopicBuilder& field(std::uint16_t field_id, WireType type, const char* name,
                        double scale = 1.0, const char* unit = "",
                        double offset = 0.0, bool is_nullable = false) noexcept {
        return add(btp::field(field_id, type, name, scale, unit, offset),
                   is_nullable);
    }

    // Commits the buffered fields into the Catalog with one add_topic()
    // call. Ok on success; otherwise the same errors add_topic() documents,
    // plus whatever a chain call already stuck (CountTooLarge if a chain
    // exceeded kMaxFields). Safe to call more than once -- it re-runs
    // add_topic() each time.
    MessageError end() noexcept {
        if (error_ != MessageError::Ok) return error_;
        error_ = catalog_->add_topic(topic_id_, schema_version_, encoding_,
                                     subscribable_, max_rate_millihz_, name_,
                                     fields_, count_);
        return error_;
    }

private:
    friend class Catalog;

    TopicBuilder(Catalog* catalog, std::uint16_t topic_id,
                std::uint16_t schema_version, const char* name,
                TelemetryEncoding encoding, bool subscribable,
                std::uint32_t max_rate_millihz) noexcept
        : catalog_(catalog),
          topic_id_(topic_id),
          schema_version_(schema_version),
          name_(name),
          encoding_(encoding),
          subscribable_(subscribable),
          max_rate_millihz_(max_rate_millihz),
          count_(0U),
          error_(MessageError::Ok) {}

    TopicBuilder& add(FieldRecord field, bool is_nullable) noexcept {
        if (error_ != MessageError::Ok) return *this;
        if (count_ >= kMaxFields) {
            error_ = MessageError::CountTooLarge;
            return *this;
        }
        if (is_nullable) field.flags |= kFieldNullable;
        fields_[count_++] = field;
        return *this;
    }

    Catalog* catalog_;
    std::uint16_t topic_id_;
    std::uint16_t schema_version_;
    const char* name_;
    TelemetryEncoding encoding_;
    bool subscribable_;
    std::uint32_t max_rate_millihz_;
    FieldRecord fields_[kMaxFields];
    std::size_t count_;
    MessageError error_;
};

inline TopicBuilder Catalog::topic(std::uint16_t topic_id,
                                   std::uint16_t schema_version,
                                   const char* name, TelemetryEncoding encoding,
                                   bool subscribable,
                                   std::uint32_t max_rate_millihz) noexcept {
    return TopicBuilder(this, topic_id, schema_version, name, encoding,
                        subscribable, max_rate_millihz);
}

// ---------------------------------------------------------------------------
// NamedSampleWriter -- SampleWriter, checked by field name
// ---------------------------------------------------------------------------
//
// SampleWriter::put_f64() is positional (docs/telemetry.md section 6: a
// PACKED_LE body is fields in schema order) -- swap two put_f64() calls and it
// compiles and sends the wrong value on the wire, silently. This wraps a
// SampleWriter with a CatalogTopic's field_names[] (btp::Catalog already keeps
// them) and turns that mistake into an InvalidArgument MessageError instead:
// put() takes the field's NAME, and it must be the schema's next field -- the
// same cost as put_f64() (one string compare against the one name it could
// legally be), no allocation, no lookup table.
//
//   const btp::CatalogTopic* topic = catalog.topic(kDriveStatus);
//   btp::NamedSampleWriter w(out, capacity, *topic);
//   w.begin(topic->schema_version);
//   w.put("left_rpm", 1450.0);
//   w.put("right_rpm", -1448.5);
//   w.put("battery_v", 3.72);
//   w.put_null("temp_c");
//   std::size_t written = 0;
//   if (w.finish(&written) == btp::MessageError::Ok) send(out, written);
//
// A name that does not match the expected next field -- the caller's own
// schema and field order drifted apart -- is InvalidArgument and leaves the
// writer unusable, loud rather than a silently wrong value on the wire.
// put_i64 / put_u64 / put_bool mirror SampleWriter's raw-value calls (enum,
// bool, a field whose raw count is already at hand); array fields are not
// covered -- reach the underlying SampleWriter (writer()) for those.

class NamedSampleWriter {
public:
    NamedSampleWriter(std::uint8_t* out, std::size_t capacity,
                      const CatalogTopic& topic) noexcept
        : writer_(out, capacity, topic.fields, topic.field_count),
          topic_(&topic),
          next_(0U) {}

    MessageError begin(std::uint16_t schema_version,
                       SampleLayout layout = SampleLayout::LogicalPayload) noexcept {
        return writer_.begin(schema_version, layout);
    }

    MessageError put(const char* name, double engineering_value) noexcept {
        const MessageError order = check(name);
        return order != MessageError::Ok ? order
                                         : advance(writer_.put_f64(engineering_value));
    }
    MessageError put_i64(const char* name, std::int64_t raw) noexcept {
        const MessageError order = check(name);
        return order != MessageError::Ok ? order : advance(writer_.put_i64(raw));
    }
    MessageError put_u64(const char* name, std::uint64_t raw) noexcept {
        const MessageError order = check(name);
        return order != MessageError::Ok ? order : advance(writer_.put_u64(raw));
    }
    MessageError put_bool(const char* name, bool value) noexcept {
        const MessageError order = check(name);
        return order != MessageError::Ok ? order
                                         : advance(writer_.put_bool(value));
    }
    MessageError put_null(const char* name) noexcept {
        const MessageError order = check(name);
        return order != MessageError::Ok ? order : advance(writer_.put_null());
    }

    MessageError finish(std::size_t* written) noexcept {
        return writer_.finish(written);
    }
    std::size_t size() const noexcept { return writer_.size(); }

    // Escape hatch: the array put_array_* calls, or anything else
    // SampleWriter offers that the by-name calls above do not wrap.
    SampleWriter& writer() noexcept { return writer_; }

private:
    MessageError check(const char* name) noexcept {
        if (name == nullptr || next_ >= topic_->field_count) {
            return MessageError::InvalidArgument;
        }
        const char* expected = topic_->field_names != nullptr
                                   ? topic_->field_names[next_]
                                   : "";
        if (std::strcmp(name, expected) != 0) {
            return MessageError::InvalidArgument;
        }
        return MessageError::Ok;
    }
    MessageError advance(MessageError result) noexcept {
        if (result == MessageError::Ok) ++next_;
        return result;
    }

    SampleWriter writer_;
    const CatalogTopic* topic_;
    std::size_t next_;
};

// ---------------------------------------------------------------------------
// StaticCatalog -- a Catalog that owns its pools
// ---------------------------------------------------------------------------

namespace detail {

template <std::size_t Topics, std::size_t Fields, std::size_t StringBytes,
          std::size_t SourceInfoEntries = 0>
struct CatalogStorage {
    CatalogTopic topics[Topics];
    FieldSpec field_pool[Fields];
    const char* name_ptr_pool[Fields];
    const char* unit_ptr_pool[Fields];
    const char* description_ptr_pool[Fields];
    char string_pool[StringBytes];
    // A zero-length array is not standard C++; hold one slot when the feature
    // is off and hand back nullptr for the pointer so Catalog treats it as
    // "no source_info pool" exactly as {nullptr, 0} would.
    SourceInfoEntry source_info_storage[SourceInfoEntries ? SourceInfoEntries : 1];
    SourceInfoEntry* source_info_pool() noexcept {
        return SourceInfoEntries != 0U ? source_info_storage : nullptr;
    }
};

}  // namespace detail

// Defaults: 8 topics, 64 field specs across them, 1.5 KiB of name/unit/
// description text (up from 1 KiB pre-v1.1 -- units and descriptions now
// share this same pool, so the same field count needs more of it), and NO
// source_info pool -- a producer that emits the format-2 block passes a
// non-zero SourceInfoEntries (and usually a bigger StringBytes, since the
// key/label/value bytes intern into the same pool).
template <std::size_t Topics = 8, std::size_t Fields = 64,
          std::size_t StringBytes = 1536, std::size_t SourceInfoEntries = 0>
class StaticCatalog
    : private detail::CatalogStorage<Topics, Fields, StringBytes,
                                     SourceInfoEntries>,
      public Catalog {
    using Storage = detail::CatalogStorage<Topics, Fields, StringBytes,
                                           SourceInfoEntries>;

public:
    StaticCatalog() noexcept
        : Storage(),
          Catalog(Storage::topics, Topics, Storage::field_pool, Fields,
                  Storage::name_ptr_pool, Fields, Storage::string_pool,
                  StringBytes, Storage::unit_ptr_pool, Fields,
                  Storage::description_ptr_pool, Fields,
                  Storage::source_info_pool(), SourceInfoEntries) {}
};

}  // namespace btp

#endif  // BTP_CATALOG_HPP
