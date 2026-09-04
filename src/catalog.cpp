#include "btp/catalog.hpp"

#include <cstring>

namespace btp {

namespace {

// A manifest field name / topic name that could not be kept (no string pool,
// or the pool is full) reads back as this rather than nullptr, so a consumer
// never has to null-check before printing.
const char* const kEmptyName = "";

}  // namespace

Catalog::Catalog(CatalogTopic* topics, std::size_t topic_capacity,
                 FieldSpec* field_pool, std::size_t field_pool_capacity,
                 const char** name_ptr_pool, std::size_t name_ptr_capacity,
                 char* string_pool, std::size_t string_pool_capacity,
                 const char** unit_ptr_pool, std::size_t unit_ptr_capacity,
                 const char** description_ptr_pool,
                 std::size_t description_ptr_capacity,
                 SourceInfoEntry* source_info_pool,
                 std::size_t source_info_capacity) noexcept
    : topics_(topics),
      topic_capacity_(topic_capacity),
      topic_count_(0U),
      field_pool_(field_pool),
      field_pool_capacity_(field_pool_capacity),
      field_pool_used_(0U),
      name_ptr_pool_(name_ptr_pool),
      name_ptr_capacity_(name_ptr_capacity),
      name_ptr_used_(0U),
      unit_ptr_pool_(unit_ptr_pool),
      unit_ptr_capacity_(unit_ptr_capacity),
      unit_ptr_used_(0U),
      description_ptr_pool_(description_ptr_pool),
      description_ptr_capacity_(description_ptr_capacity),
      description_ptr_used_(0U),
      string_pool_(string_pool),
      string_pool_capacity_(string_pool_capacity),
      string_pool_used_(0U),
      source_info_pool_(source_info_pool),
      source_info_capacity_(source_info_capacity),
      source_info_count_(0U),
      config_revision_(0U),
      valid_(topics != nullptr && topic_capacity != 0U &&
             field_pool != nullptr && field_pool_capacity != 0U) {}

void Catalog::clear() noexcept {
    topic_count_ = 0U;
    field_pool_used_ = 0U;
    name_ptr_used_ = 0U;
    unit_ptr_used_ = 0U;
    description_ptr_used_ = 0U;
    string_pool_used_ = 0U;
    // source_info interns into the same string pool this just reset -- dropping
    // the count is what keeps its ByteViews from dangling into reused bytes.
    source_info_count_ = 0U;
}

void Catalog::clear_source_info() noexcept { source_info_count_ = 0U; }

bool Catalog::has_topic(std::uint16_t topic_id) const noexcept {
    for (std::size_t i = 0U; i < topic_count_; ++i) {
        if (topics_[i].topic_id == topic_id) return true;
    }
    return false;
}

const CatalogTopic* Catalog::topic(std::uint16_t topic_id) const noexcept {
    for (std::size_t i = 0U; i < topic_count_; ++i) {
        if (topics_[i].topic_id == topic_id) return &topics_[i];
    }
    return nullptr;
}

const CatalogTopic* Catalog::topic_at(std::size_t index) const noexcept {
    return index < topic_count_ ? &topics_[index] : nullptr;
}

const char* Catalog::intern(const char* s) noexcept {
    if (s == nullptr) return kEmptyName;
    return intern(reinterpret_cast<const std::uint8_t*>(s), std::strlen(s));
}

const char* Catalog::intern(const std::uint8_t* data, std::size_t len) noexcept {
    if (len == 0U || string_pool_ == nullptr) return kEmptyName;
    if (string_pool_used_ + len + 1U > string_pool_capacity_) return kEmptyName;
    char* dst = string_pool_ + string_pool_used_;
    std::memcpy(dst, data, len);
    dst[len] = '\0';
    string_pool_used_ += len + 1U;
    return dst;
}

ByteView Catalog::intern_view(const std::uint8_t* data,
                              std::size_t len) noexcept {
    const char* s = intern(data, len);
    // intern() returns the static "" both for an empty input and for a pool
    // that had no room -- either way the ByteView is {"", 0}, so a consumer
    // reading it back never dereferences past a zero length.
    const std::size_t n = (s == kEmptyName) ? 0U : len;
    return ByteView{reinterpret_cast<const std::uint8_t*>(s), n};
}

const char* Catalog::field_name(const CatalogTopic& t,
                                std::size_t index) const noexcept {
    if (t.field_names == nullptr || index >= t.field_count) return kEmptyName;
    return t.field_names[index];
}

const char* Catalog::field_unit(const CatalogTopic& t,
                                std::size_t index) const noexcept {
    if (t.field_units == nullptr || index >= t.field_count) return kEmptyName;
    return t.field_units[index];
}

const char* Catalog::field_description(const CatalogTopic& t,
                                       std::size_t index) const noexcept {
    if (t.field_descriptions == nullptr || index >= t.field_count) {
        return kEmptyName;
    }
    return t.field_descriptions[index];
}

// ---------------------------------------------------------------------------
// Producer: fill by hand
// ---------------------------------------------------------------------------

MessageError Catalog::add_topic(std::uint16_t topic_id,
                                std::uint16_t schema_version,
                                TelemetryEncoding encoding, bool subscribable,
                                std::uint32_t max_rate_millihz, const char* name,
                                const FieldRecord* fields,
                                std::size_t field_count) noexcept {
    // A null `fields` is only invalid when it would actually be read (some
    // field_count > 0); field_count == 0 alone describes a legitimate
    // body-only topic (OpaqueBytes/Utf8/JsonUtf8/CsvUtf8) and is accepted --
    // see this method's own doc comment.
    if (!valid_ || (field_count != 0U && fields == nullptr) ||
        has_topic(topic_id)) {
        return MessageError::InvalidArgument;
    }
    // field_id / order default to the array position (the schema helpers leave
    // them 0). An explicit field_id is kept -- a schema that must survive a
    // rename -- but an explicit order must still equal the position.
    for (std::size_t i = 0U; i < field_count; ++i) {
        if (fields[i].order != 0U &&
            fields[i].order != static_cast<std::uint16_t>(i)) {
            return MessageError::InvalidArgument;
        }
    }
    if (topic_count_ >= topic_capacity_ ||
        field_pool_used_ + field_count > field_pool_capacity_) {
        return MessageError::BufferTooSmall;
    }
    const bool keep_names = name_ptr_pool_ != nullptr;
    const bool keep_units = unit_ptr_pool_ != nullptr;
    const bool keep_descriptions = description_ptr_pool_ != nullptr;
    if ((keep_names && name_ptr_used_ + field_count > name_ptr_capacity_) ||
        (keep_units && unit_ptr_used_ + field_count > unit_ptr_capacity_) ||
        (keep_descriptions &&
         description_ptr_used_ + field_count > description_ptr_capacity_)) {
        return MessageError::BufferTooSmall;
    }

    CatalogTopic& t = topics_[topic_count_];
    t.topic_id = topic_id;
    t.schema_version = schema_version;
    t.encoding = static_cast<std::uint8_t>(encoding);
    t.flags = subscribable ? kTopicSubscribable : 0U;
    t.max_rate_millihz = max_rate_millihz;
    t.fields = &field_pool_[field_pool_used_];
    t.field_count = field_count;
    t.name = intern(name);
    t.field_names = keep_names ? &name_ptr_pool_[name_ptr_used_] : nullptr;
    t.field_units = keep_units ? &unit_ptr_pool_[unit_ptr_used_] : nullptr;
    t.field_descriptions =
        keep_descriptions ? &description_ptr_pool_[description_ptr_used_]
                          : nullptr;

    for (std::size_t i = 0U; i < field_count; ++i) {
        FieldSpec spec = field_spec(fields[i]);
        spec.order = static_cast<std::uint16_t>(i);
        if (spec.field_id == 0U) {
            spec.field_id = static_cast<std::uint16_t>(i + 1U);
        }
        field_pool_[field_pool_used_ + i] = spec;
        if (keep_names) {
            name_ptr_pool_[name_ptr_used_ + i] =
                intern(fields[i].name.data, fields[i].name.size);
        }
        if (keep_units) {
            unit_ptr_pool_[unit_ptr_used_ + i] =
                intern(fields[i].unit.data, fields[i].unit.size);
        }
        if (keep_descriptions) {
            description_ptr_pool_[description_ptr_used_ + i] =
                intern(fields[i].description.data, fields[i].description.size);
        }
    }
    field_pool_used_ += field_count;
    if (keep_names) name_ptr_used_ += field_count;
    if (keep_units) unit_ptr_used_ += field_count;
    if (keep_descriptions) description_ptr_used_ += field_count;
    ++topic_count_;
    return MessageError::Ok;
}

// ---------------------------------------------------------------------------
// Consumer: learn from a MANIFEST_DATA payload
// ---------------------------------------------------------------------------

MessageError Catalog::ingest(const std::uint8_t* payload,
                             std::size_t size) noexcept {
    if (!valid_ || (payload == nullptr && size != 0U)) {
        return MessageError::InvalidArgument;
    }

    ManifestReader reader(payload, size);
    ManifestHeader header = {};
    const MessageError he = reader.header(&header);
    if (he != MessageError::Ok) return he;

    if ((header.flags & kManifestNotModified) != 0U) {
        // The responder confirmed we already hold this revision -- keep it.
        return MessageError::Ok;
    }

    clear();
    config_revision_ = header.config_revision;

    // The format-2 source_info block comes before the topics; walk it into our
    // own pool when the caller kept one. Skipping this call is fine too --
    // next_topic() steps over any source_info left un-iterated.
    if (source_info_pool_ != nullptr) {
        SourceInfoEntry si = {};
        while (reader.next_source_info(&si) == ManifestStep::Item) {
            if (source_info_count_ >= source_info_capacity_) {
                clear();
                return MessageError::BufferTooSmall;
            }
            source_info_pool_[source_info_count_].key =
                intern_view(si.key.data, si.key.size);
            source_info_pool_[source_info_count_].label =
                intern_view(si.label.data, si.label.size);
            source_info_pool_[source_info_count_].value =
                intern_view(si.value.data, si.value.size);
            ++source_info_count_;
        }
        if (reader.error() != MessageError::Ok) {
            const MessageError se = reader.error();
            clear();
            return se;
        }
    }

    const bool keep_names = name_ptr_pool_ != nullptr;
    const bool keep_units = unit_ptr_pool_ != nullptr;
    const bool keep_descriptions = description_ptr_pool_ != nullptr;
    TopicRecord topic = {};
    ByteView field_bytes = {};
    while (reader.next_topic(&topic, &field_bytes) == ManifestStep::Item) {
        if (topic_count_ >= topic_capacity_ ||
            field_pool_used_ + topic.field_count > field_pool_capacity_ ||
            (keep_names &&
             name_ptr_used_ + topic.field_count > name_ptr_capacity_) ||
            (keep_units &&
             unit_ptr_used_ + topic.field_count > unit_ptr_capacity_) ||
            (keep_descriptions && description_ptr_used_ + topic.field_count >
                                      description_ptr_capacity_)) {
            clear();
            return MessageError::BufferTooSmall;
        }

        CatalogTopic& t = topics_[topic_count_];
        t.topic_id = topic.topic_id;
        t.schema_version = topic.schema_version;
        t.encoding = topic.encoding;
        t.flags = topic.flags;
        t.max_rate_millihz = topic.max_rate_millihz;
        t.fields = &field_pool_[field_pool_used_];
        t.name = intern(topic.name.data, topic.name.size);
        t.field_names = keep_names ? &name_ptr_pool_[name_ptr_used_] : nullptr;
        t.field_units = keep_units ? &unit_ptr_pool_[unit_ptr_used_] : nullptr;
        t.field_descriptions =
            keep_descriptions ? &description_ptr_pool_[description_ptr_used_]
                              : nullptr;

        FieldRecordReader fields(field_bytes, topic.field_count);
        FieldRecord record = {};
        ByteView enum_bytes = {};
        std::size_t n = 0U;
        while (fields.next(&record, &enum_bytes) == ManifestStep::Item) {
            field_pool_[field_pool_used_ + n] = field_spec(record);
            if (keep_names) {
                name_ptr_pool_[name_ptr_used_ + n] =
                    intern(record.name.data, record.name.size);
            }
            if (keep_units) {
                unit_ptr_pool_[unit_ptr_used_ + n] =
                    intern(record.unit.data, record.unit.size);
            }
            if (keep_descriptions) {
                description_ptr_pool_[description_ptr_used_ + n] =
                    intern(record.description.data, record.description.size);
            }
            ++n;
        }
        if (fields.error() != MessageError::Ok) {
            const MessageError fe = fields.error();
            clear();
            return fe;
        }

        t.field_count = n;
        field_pool_used_ += n;
        if (keep_names) name_ptr_used_ += n;
        if (keep_units) unit_ptr_used_ += n;
        if (keep_descriptions) description_ptr_used_ += n;
        ++topic_count_;
    }

    // Walk past any action records so finish() can confirm the payload was
    // consumed exactly. v1 keeps none of them.
    ActionRecord action = {};
    ByteView params = {};
    ByteView results = {};
    ByteView errors = {};
    while (reader.next_action(&action, &params, &results, &errors) ==
           ManifestStep::Item) {
    }

    const MessageError fe = reader.finish();
    if (fe != MessageError::Ok) {
        clear();
        return fe;
    }
    return MessageError::Ok;
}

// ---------------------------------------------------------------------------
// Producer: serialise into an open ManifestWriter
// ---------------------------------------------------------------------------

MessageError Catalog::write_topics(ManifestWriter* writer) const noexcept {
    if (writer == nullptr) return MessageError::InvalidArgument;

    for (std::size_t ti = 0U; ti < topic_count_; ++ti) {
        const CatalogTopic& t = topics_[ti];

        TopicRecord tr = {};
        tr.topic_id = t.topic_id;
        tr.schema_version = t.schema_version;
        tr.encoding = t.encoding;
        tr.flags = t.flags;
        tr.field_count = static_cast<std::uint16_t>(t.field_count);
        tr.max_rate_millihz = t.max_rate_millihz;
        tr.name = ByteView{reinterpret_cast<const std::uint8_t*>(t.name),
                           std::strlen(t.name)};
        tr.description = ByteView{nullptr, 0U};

        const MessageError be = writer->begin_topic(tr);
        if (be != MessageError::Ok) return be;

        for (std::size_t fi = 0U; fi < t.field_count; ++fi) {
            const FieldSpec& spec = t.fields[fi];
            const char* fname = field_name(t, fi);
            const char* funit = field_unit(t, fi);
            const char* fdesc = field_description(t, fi);

            FieldRecord fr = {};
            fr.field_id = spec.field_id;
            fr.order = spec.order;
            fr.type = spec.type;
            fr.flags = spec.flags;
            fr.element_count = spec.element_count;
            fr.max_element_count = spec.max_element_count;
            fr.scale = spec.scale;
            fr.offset = spec.offset;
            fr.enum_count = 0U;
            fr.name = ByteView{reinterpret_cast<const std::uint8_t*>(fname),
                               std::strlen(fname)};
            fr.unit = ByteView{reinterpret_cast<const std::uint8_t*>(funit),
                               std::strlen(funit)};
            fr.description =
                ByteView{reinterpret_cast<const std::uint8_t*>(fdesc),
                        std::strlen(fdesc)};

            const MessageError ae = writer->add_field(fr);
            if (ae != MessageError::Ok) return ae;
        }

        const MessageError ee = writer->end_topic();
        if (ee != MessageError::Ok) return ee;
    }
    return MessageError::Ok;
}

// ---------------------------------------------------------------------------
// Producer / consumer: the format-2 source_info block
// ---------------------------------------------------------------------------

MessageError Catalog::add_source_info(const char* key, const char* label,
                                     const char* value) noexcept {
    // An unset value carries no row (commands.md 3.12: informational, and the
    // consumer side already drops empties) -- not an error, just nothing to do.
    if (value == nullptr || value[0] == '\0') return MessageError::Ok;
    if (source_info_pool_ == nullptr) return MessageError::InvalidArgument;
    if (source_info_count_ >= source_info_capacity_) {
        return MessageError::BufferTooSmall;
    }

    const ByteView k = intern_view(reinterpret_cast<const std::uint8_t*>(key),
                                   key != nullptr ? std::strlen(key) : 0U);
    const ByteView l = intern_view(reinterpret_cast<const std::uint8_t*>(label),
                                   label != nullptr ? std::strlen(label) : 0U);
    const ByteView v = intern_view(reinterpret_cast<const std::uint8_t*>(value),
                                   std::strlen(value));
    // key / value were non-empty but interned to "" -> the string pool is full.
    if ((key != nullptr && key[0] != '\0' && k.size == 0U) || v.size == 0U) {
        return MessageError::BufferTooSmall;
    }

    source_info_pool_[source_info_count_].key = k;
    source_info_pool_[source_info_count_].label = l;
    source_info_pool_[source_info_count_].value = v;
    ++source_info_count_;
    return MessageError::Ok;
}

const SourceInfoEntry* Catalog::source_info_at(std::size_t index) const noexcept {
    return index < source_info_count_ ? &source_info_pool_[index] : nullptr;
}

MessageError Catalog::write_source_info(ManifestWriter* writer) const noexcept {
    if (writer == nullptr) return MessageError::InvalidArgument;
    for (std::size_t i = 0U; i < source_info_count_; ++i) {
        const SourceInfoEntry& e = source_info_pool_[i];
        // key/label/value each get a u16 length prefix on the wire (6 octets).
        const std::size_t entry_size =
            6U + e.key.size + e.label.size + e.value.size;
        if (writer->size() + entry_size > writer->capacity()) {
            break;  // no room left -- the topic records that follow win it
        }
        const MessageError we = writer->add_source_info(e);
        if (we != MessageError::Ok) return we;
    }
    return MessageError::Ok;
}

}  // namespace btp
