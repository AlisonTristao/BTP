// Unit tests for btp::Catalog -- the schema catalogue of docs/library.md
// section 11.2, the piece between btp::messages (MANIFEST_DATA layout) and
// btp::telemetry (the sample codec against a FieldSpec[]).
//
// State above the wire, not a wire layout, so no vector tree: the suite fills
// a Catalog by hand, round-trips it through a real MANIFEST_DATA
// (ManifestWriter -> ingest) and checks every topic comes back.

#include "btp/catalog.hpp"

#include "btp/messages.hpp"
#include "btp/telemetry.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << __FILE__ << ':' << __LINE__                            \
                      << ": check failed: " #condition << '\n';                 \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

using btp::Catalog;
using btp::CatalogTopic;
using btp::FieldRecord;
using btp::MessageError;
using btp::TelemetryEncoding;
using btp::WireType;

btp::ByteView text(const char* s) {
    return btp::ByteView{reinterpret_cast<const std::uint8_t*>(s),
                         std::strlen(s)};
}

const FieldRecord kDriveStatus[] = {
    btp::f32("left_rpm"),
    btp::f32("right_rpm"),
    btp::u16("battery_v", 0.001),
    btp::nullable(btp::i16("temp_c", 0.1)),
};

// Serialise `source` into a MANIFEST_DATA payload. Returns 0 on failure.
std::size_t serialise(const Catalog& source, std::uint8_t* out,
                      std::size_t capacity, std::uint8_t manifest_flags = 0U) {
    btp::ManifestHeader header = {};
    header.status = static_cast<std::uint8_t>(btp::ResultStatus::Success);
    header.flags = manifest_flags;
    header.manifest_format_version = 1U;
    header.config_revision = source.config_revision();
    header.described_source_id = 0x00CAFE01U;
    header.described_boot_id = 0x0000B001U;
    header.source_role = static_cast<std::uint8_t>(btp::Role::Producer);
    header.source_flags = btp::kSourceOnline;
    header.catalog_count = 1U;
    const bool not_modified = (manifest_flags & btp::kManifestNotModified) != 0U;
    header.topic_count =
        not_modified ? 0U : static_cast<std::uint16_t>(source.topic_count());
    header.source_name = text("test-source");

    btp::ManifestWriter w(out, capacity);
    if (w.begin(header) != MessageError::Ok) return 0U;
    if (!not_modified) {
        if (source.write_topics(&w) != MessageError::Ok) return 0U;
    }
    std::size_t written = 0U;
    return w.finish(&written) == MessageError::Ok ? written : 0U;
}

// ---------------------------------------------------------------------------

void test_add_and_query() {
    btp::StaticCatalog<> cat;
    CHECK(cat.valid());
    CHECK(cat.topic_count() == 0U);

    cat.set_config_revision(7U);
    CHECK(cat.add_topic(0x0101U, 3U, TelemetryEncoding::PackedLe, true, 100000U,
                        "drive_status", kDriveStatus, 4U) == MessageError::Ok);
    CHECK(cat.topic_count() == 1U);

    const CatalogTopic* t = cat.topic(0x0101U);
    CHECK(t != nullptr);
    CHECK(t->schema_version == 3U);
    CHECK(t->encoding ==
          static_cast<std::uint8_t>(TelemetryEncoding::PackedLe));
    CHECK((t->flags & btp::kTopicSubscribable) != 0U);
    CHECK(t->max_rate_millihz == 100000U);
    CHECK(t->field_count == 4U);
    CHECK(t->fields[2].field_id == 3U);
    CHECK(t->fields[2].type == static_cast<std::uint8_t>(WireType::Uint16));
    CHECK(t->fields[2].scale == 0.001);
    CHECK((t->fields[3].flags & btp::kFieldNullable) != 0U);
    CHECK(std::strcmp(t->name, "drive_status") == 0);
    CHECK(std::strcmp(cat.field_name(*t, 0), "left_rpm") == 0);
    CHECK(std::strcmp(cat.field_name(*t, 3), "temp_c") == 0);

    CHECK(cat.topic(0x0999U) == nullptr);

    // duplicate topic_id
    CHECK(cat.add_topic(0x0101U, 1U, TelemetryEncoding::PackedLe, false, 0U, "x",
                        kDriveStatus, 4U) == MessageError::InvalidArgument);
    // an explicit order that disagrees with the position
    FieldRecord bad[] = {btp::u8("a"), btp::u8("b")};
    bad[1].order = 5U;
    CHECK(cat.add_topic(0x0202U, 1U, TelemetryEncoding::PackedLe, false, 0U, "y",
                        bad, 2U) == MessageError::InvalidArgument);
}

void test_schema_helpers() {
    // f32 / u16 / nullable produce FieldRecords with field_id/order 0 -- the
    // Catalog assigns them from the array position.
    const FieldRecord fields[] = {
        btp::f32("speed", "m/s"),
        btp::u16("volts", 0.001, "V"),
        btp::nullable(btp::i16("temp", 0.1, "Cel")),
        btp::field(99, WireType::Uint8, "mode"),  // explicit id survives
    };
    CHECK(fields[0].field_id == 0U && fields[0].order == 0U);
    CHECK(fields[0].type == static_cast<std::uint8_t>(WireType::Float32));
    CHECK(fields[1].scale == 0.001);
    CHECK((fields[2].flags & btp::kFieldNullable) != 0U);
    CHECK(fields[3].field_id == 99U);

    btp::StaticCatalog<> cat;
    CHECK(cat.add_topic(0x0300U, 1U, "mixed", fields) == MessageError::Ok);
    const CatalogTopic* t = cat.topic(0x0300U);
    CHECK(t != nullptr);
    CHECK(t->field_count == 4U);
    CHECK(t->fields[0].field_id == 1U);   // position + 1
    CHECK(t->fields[1].field_id == 2U);
    CHECK(t->fields[3].field_id == 99U);  // kept
    CHECK(t->fields[3].order == 3U);      // position
    CHECK(std::strcmp(cat.field_name(*t, 2), "temp") == 0);
}

void test_manifest_roundtrip() {
    btp::StaticCatalog<> producer;
    producer.set_config_revision(42U);
    producer.add_topic(0x0101U, 3U, TelemetryEncoding::PackedLe, true, 100000U,
                       "drive_status", kDriveStatus, 4U);

    static const FieldRecord kPose[] = {
        btp::f64("x_m"),
        btp::f64("y_m"),
        btp::f32("heading_rad"),
    };
    producer.add_topic(0x0102U, 1U, TelemetryEncoding::PackedLe, true, 50000U,
                       "pose", kPose, 3U);

    std::uint8_t wire[1024];
    const std::size_t n = serialise(producer, wire, sizeof(wire));
    CHECK(n != 0U);

    btp::StaticCatalog<> consumer;
    CHECK(consumer.ingest(wire, n) == MessageError::Ok);
    CHECK(consumer.config_revision() == 42U);
    CHECK(consumer.topic_count() == 2U);

    const CatalogTopic* drive = consumer.topic(0x0101U);
    CHECK(drive != nullptr);
    CHECK(drive->schema_version == 3U);
    CHECK(drive->field_count == 4U);
    CHECK(drive->max_rate_millihz == 100000U);
    CHECK((drive->flags & btp::kTopicSubscribable) != 0U);
    CHECK(drive->fields[2].scale == 0.001);
    CHECK(std::strcmp(drive->name, "drive_status") == 0);
    CHECK(std::strcmp(consumer.field_name(*drive, 2), "battery_v") == 0);
    CHECK((drive->fields[3].flags & btp::kFieldNullable) != 0U);

    const CatalogTopic* pose = consumer.topic(0x0102U);
    CHECK(pose != nullptr);
    CHECK(pose->field_count == 3U);
    CHECK(pose->fields[0].type == static_cast<std::uint8_t>(WireType::Float64));
    CHECK(std::strcmp(consumer.field_name(*pose, 2), "heading_rad") == 0);

    // The learned schema decodes a sample -- the point of the catalogue.
    std::uint8_t body[64];
    btp::SampleWriter sw(body, sizeof(body), drive->fields, drive->field_count);
    sw.begin(3U);
    sw.put_f64(1450.0);
    sw.put_f64(-1448.5);
    sw.put_f64(3.72);
    sw.put_null();
    std::size_t body_n = 0U;
    CHECK(sw.finish(&body_n) == MessageError::Ok);

    btp::SampleReader sr(body, body_n, drive->fields, drive->field_count,
                         btp::kEncodingPackedLe);
    btp::SampleValue v = {};
    CHECK(sr.next(&v) == btp::SampleStep::Item);
    CHECK(v.f64(0) == 1450.0);
    CHECK(sr.next(&v) == btp::SampleStep::Item);
    CHECK(sr.next(&v) == btp::SampleStep::Item);
    CHECK(v.f64(0) == 3.72);  // raw 3720 * 0.001
    CHECK(sr.next(&v) == btp::SampleStep::Item);
    CHECK(v.is_null);
    CHECK(sr.next(&v) == btp::SampleStep::End);
    CHECK(sr.finish() == MessageError::Ok);
}

// v1.1: a field's unit and description survive add_topic() -> field_unit()/
// field_description() directly, AND round-trip through a real MANIFEST_DATA
// (write_topics() -> ingest()) the same way name already did. The topic's OWN
// description does not (see catalog.hpp's top comment) -- not checked here.
void test_field_unit_and_description() {
    // btp::f32/u16 (telemetry.hpp) take `unit` but never `description` --
    // that one only arrives via a hand-built FieldRecord, so this schema
    // covers both paths: units from the usual helpers, one description set
    // directly.
    FieldRecord fields[] = {
        btp::f32("speed", "m/s"),
        btp::u16("volts", 0.001, "V"),
    };
    fields[0].description = text("Ground speed, forward positive");

    btp::StaticCatalog<> producer;
    CHECK(producer.add_topic(0x0400U, 1U, "kinematics", fields) ==
          MessageError::Ok);
    const CatalogTopic* pt = producer.topic(0x0400U);
    CHECK(pt != nullptr);
    CHECK(std::strcmp(producer.field_unit(*pt, 0), "m/s") == 0);
    CHECK(std::strcmp(producer.field_unit(*pt, 1), "V") == 0);
    CHECK(std::strcmp(producer.field_description(*pt, 0),
                      "Ground speed, forward positive") == 0);
    CHECK(std::strcmp(producer.field_description(*pt, 1), "") == 0);
    // Out of range / no field_units at all -- still "", never a crash.
    CHECK(std::strcmp(producer.field_unit(*pt, 99U), "") == 0);

    std::uint8_t wire[512];
    const std::size_t n = serialise(producer, wire, sizeof(wire));
    CHECK(n != 0U);

    btp::StaticCatalog<> consumer;
    CHECK(consumer.ingest(wire, n) == MessageError::Ok);
    const CatalogTopic* ct = consumer.topic(0x0400U);
    CHECK(ct != nullptr);
    CHECK(std::strcmp(consumer.field_unit(*ct, 0), "m/s") == 0);
    CHECK(std::strcmp(consumer.field_unit(*ct, 1), "V") == 0);
    CHECK(std::strcmp(consumer.field_description(*ct, 0),
                      "Ground speed, forward positive") == 0);
}

void test_not_modified() {
    btp::StaticCatalog<> consumer;
    consumer.add_topic(0x0101U, 3U, TelemetryEncoding::PackedLe, true, 1000U,
                       "drive_status", kDriveStatus, 4U);
    consumer.set_config_revision(5U);

    std::uint8_t wire[256];
    const std::size_t n =
        serialise(consumer, wire, sizeof(wire), btp::kManifestNotModified);
    CHECK(n != 0U);

    CHECK(consumer.ingest(wire, n) == MessageError::Ok);
    CHECK(consumer.topic_count() == 1U);  // kept
    CHECK(consumer.topic(0x0101U) != nullptr);
}

void test_capacity() {
    // Room for one topic, four field specs.
    btp::StaticCatalog<1, 4, 128> cat;
    CHECK(cat.add_topic(0x0101U, 1U, TelemetryEncoding::PackedLe, false, 0U, "a",
                        kDriveStatus, 4U) == MessageError::Ok);
    CHECK(cat.add_topic(0x0102U, 1U, TelemetryEncoding::PackedLe, false, 0U, "b",
                        kDriveStatus, 4U) == MessageError::BufferTooSmall);

    btp::StaticCatalog<4, 3, 128> tight_fields;
    CHECK(tight_fields.add_topic(0x0101U, 1U, TelemetryEncoding::PackedLe, false,
                                 0U, "a", kDriveStatus, 4U) ==
          MessageError::BufferTooSmall);
}

void test_bad_manifest() {
    btp::StaticCatalog<> cat;
    const std::uint8_t junk[6] = {1, 2, 3, 4, 5, 6};
    CHECK(cat.ingest(junk, sizeof(junk)) != MessageError::Ok);
    CHECK(cat.topic_count() == 0U);
}

// ===========================================================================
// TopicBuilder -- the chained alternative to a FieldRecord[]
// ===========================================================================

using btp::NamedSampleWriter;

void test_topic_builder_matches_the_array_form() {
    btp::StaticCatalog<> chained;
    CHECK(chained.topic(0x0101U, 3U, "drive_status")
             .f32("left_rpm")
             .f32("right_rpm")
             .u16("battery_v", 0.001)
             .i16("temp_c", 0.1, "", /*is_nullable=*/true)
             .end() == MessageError::Ok);

    btp::StaticCatalog<> by_hand;
    CHECK(by_hand.add_topic(0x0101U, 3U, "drive_status", kDriveStatus) ==
          MessageError::Ok);

    const CatalogTopic* a = chained.topic(0x0101U);
    const CatalogTopic* b = by_hand.topic(0x0101U);
    CHECK(a != nullptr && b != nullptr);
    CHECK(a->field_count == b->field_count);
    for (std::size_t i = 0U; i < a->field_count; ++i) {
        CHECK(a->fields[i].type == b->fields[i].type);
        CHECK(a->fields[i].scale == b->fields[i].scale);
        CHECK(a->fields[i].flags == b->fields[i].flags);
        CHECK(std::strcmp(chained.field_name(*a, i), by_hand.field_name(*b, i)) ==
              0);
    }
}

void test_topic_builder_propagates_add_topic_errors() {
    btp::StaticCatalog<> cat;
    CHECK(cat.add_topic(0x0101U, 1U, "first", kDriveStatus) == MessageError::Ok);

    // add_topic() itself rejects the duplicate id -- end() surfaces it.
    CHECK(cat.topic(0x0101U, 2U, "again").u8("x").end() ==
          MessageError::InvalidArgument);
}

void test_topic_builder_caps_fields_per_declaration() {
    btp::StaticCatalog<1, 64, 512> cat;
    btp::TopicBuilder b = cat.topic(0x0101U, 1U, "wide");
    char names[btp::TopicBuilder::kMaxFields + 1U][4];
    for (std::size_t i = 0U; i < btp::TopicBuilder::kMaxFields + 1U; ++i) {
        names[i][0] = 'a';
        names[i][1] = static_cast<char>('A' + (i % 26U));
        names[i][2] = static_cast<char>('0' + (i / 26U));
        names[i][3] = '\0';
        b.u8(names[i]);
    }
    // One past the cap -- the chain already stuck CountTooLarge.
    CHECK(b.end() == MessageError::CountTooLarge);
}

// ===========================================================================
// NamedSampleWriter -- SampleWriter, checked by field name
// ===========================================================================

void test_named_sample_writer_round_trips_by_name() {
    btp::StaticCatalog<> cat;
    CHECK(cat.add_topic(0x0101U, 3U, "drive_status", kDriveStatus) ==
          MessageError::Ok);
    const CatalogTopic* topic = cat.topic(0x0101U);
    CHECK(topic != nullptr);

    std::uint8_t buffer[64];
    NamedSampleWriter w(buffer, sizeof(buffer), *topic);
    CHECK(w.begin(topic->schema_version) == MessageError::Ok);
    CHECK(w.put("left_rpm", 1450.0) == MessageError::Ok);
    CHECK(w.put("right_rpm", -1448.5) == MessageError::Ok);
    CHECK(w.put("battery_v", 3.72) == MessageError::Ok);
    CHECK(w.put_null("temp_c") == MessageError::Ok);
    std::size_t written = 0U;
    CHECK(w.finish(&written) == MessageError::Ok);

    btp::SampleReader r(buffer, written, topic->fields, topic->field_count,
                        topic->encoding);
    btp::SampleValue v = {};
    CHECK(r.next(&v) == btp::SampleStep::Item);
    CHECK(v.f64(0) == 1450.0);
    CHECK(r.next(&v) == btp::SampleStep::Item);
    CHECK(v.f64(0) == -1448.5);
    CHECK(r.next(&v) == btp::SampleStep::Item);
    CHECK(v.f64(0) > 3.71 && v.f64(0) < 3.73);  // millivolt scale, rounded
    CHECK(r.next(&v) == btp::SampleStep::Item);
    CHECK(v.is_null);
    CHECK(r.next(&v) == btp::SampleStep::End);
    CHECK(r.finish() == MessageError::Ok);
}

void test_named_sample_writer_rejects_the_wrong_field_name() {
    btp::StaticCatalog<> cat;
    CHECK(cat.add_topic(0x0101U, 3U, "drive_status", kDriveStatus) ==
          MessageError::Ok);
    const CatalogTopic* topic = cat.topic(0x0101U);
    CHECK(topic != nullptr);

    std::uint8_t buffer[64];
    NamedSampleWriter w(buffer, sizeof(buffer), *topic);
    CHECK(w.begin(topic->schema_version) == MessageError::Ok);
    CHECK(w.put("left_rpm", 1450.0) == MessageError::Ok);
    // "battery_v" out of order -- the schema's next field is "right_rpm".
    CHECK(w.put("battery_v", 3.72) == MessageError::InvalidArgument);

    std::size_t written = 0U;
    CHECK(w.finish(&written) != MessageError::Ok);  // incomplete: never sent
}

// add_source_info() -> write_source_info() -> the wire -> ingest() ->
// source_info_at(), plus the format-2 header bump and the empty-value skip.
void test_source_info_round_trips() {
    btp::StaticCatalog<4, 32, 1024, /*SourceInfoEntries=*/4> producer;
    CHECK(producer.add_topic(0x0101U, 3U, "drive_status", kDriveStatus) ==
          MessageError::Ok);
    CHECK(producer.source_info_count() == 0U);
    CHECK(!producer.has_source_info());

    CHECK(producer.add_source_info("fw_version", "Firmware", "2") ==
          MessageError::Ok);
    CHECK(producer.add_source_info("chip", "", "ESP32-S3") == MessageError::Ok);
    // An empty value carries no row -- not an error.
    CHECK(producer.add_source_info("build", "Build", "") == MessageError::Ok);
    CHECK(producer.source_info_count() == 2U);
    CHECK(producer.has_source_info());

    const btp::SourceInfoEntry* first = producer.source_info_at(0U);
    CHECK(first != nullptr);
    CHECK(first->key.size == 10U &&
          std::memcmp(first->key.data, "fw_version", 10U) == 0);
    CHECK(first->label.size == 8U);
    CHECK(producer.source_info_at(2U) == nullptr);

    // Serialise a format-2 manifest by hand: header, source_info block, topics.
    std::uint8_t wire[512];
    btp::ManifestHeader header = {};
    header.status = static_cast<std::uint8_t>(btp::ResultStatus::Success);
    header.manifest_format_version = 2U;
    header.config_revision = 7U;
    header.described_source_id = 0x00CAFE01U;
    header.described_boot_id = 0x0000B001U;
    header.source_role = static_cast<std::uint8_t>(btp::Role::Producer);
    header.source_flags = btp::kSourceOnline;
    header.catalog_count = 1U;
    header.topic_count = 1U;
    header.source_name = text("robot");
    producer.set_config_revision(7U);

    btp::ManifestWriter w(wire, sizeof(wire));
    CHECK(w.begin(header) == MessageError::Ok);
    CHECK(producer.write_source_info(&w) == MessageError::Ok);
    CHECK(producer.write_topics(&w) == MessageError::Ok);
    std::size_t n = 0U;
    CHECK(w.finish(&n) == MessageError::Ok && n != 0U);

    // The reader sees format 2 and the two rows.
    btp::ManifestReader r(wire, n);
    btp::ManifestHeader parsed = {};
    CHECK(r.header(&parsed) == MessageError::Ok);
    CHECK(parsed.manifest_format_version == 2U);
    btp::SourceInfoEntry si = {};
    CHECK(r.next_source_info(&si) == btp::ManifestStep::Item);
    CHECK(si.value.size == 1U && si.value.data[0] == '2');
    CHECK(r.next_source_info(&si) == btp::ManifestStep::Item);
    CHECK(si.key.size == 4U && std::memcmp(si.key.data, "chip", 4U) == 0);
    CHECK(si.label.size == 0U);
    CHECK(r.next_source_info(&si) == btp::ManifestStep::End);

    // A consumer with its own source_info pool learns the rows through ingest.
    btp::StaticCatalog<4, 32, 1024, 4> consumer;
    CHECK(consumer.ingest(wire, n) == MessageError::Ok);
    CHECK(consumer.config_revision() == 7U);
    CHECK(consumer.topic_count() == 1U);
    CHECK(consumer.source_info_count() == 2U);
    const btp::SourceInfoEntry* c0 = consumer.source_info_at(0U);
    CHECK(c0 != nullptr && c0->value.size == 1U && c0->value.data[0] == '2');
    CHECK(std::memcmp(consumer.source_info_at(1U)->key.data, "chip", 4U) == 0);

    // A consumer WITHOUT a source_info pool still ingests the topics fine --
    // next_topic() steps over the block it cannot keep.
    btp::StaticCatalog<4, 32, 1024> plain_consumer;
    CHECK(plain_consumer.ingest(wire, n) == MessageError::Ok);
    CHECK(plain_consumer.topic_count() == 1U);
    CHECK(plain_consumer.source_info_count() == 0U);

    // No pool on the producer side -> add_source_info is InvalidArgument.
    btp::StaticCatalog<> no_pool;
    CHECK(no_pool.add_source_info("k", "l", "v") == MessageError::InvalidArgument);
}

}  // namespace

int main() {
    test_add_and_query();
    test_schema_helpers();
    test_manifest_roundtrip();
    test_field_unit_and_description();
    test_not_modified();
    test_capacity();
    test_bad_manifest();

    test_topic_builder_matches_the_array_form();
    test_topic_builder_propagates_add_topic_errors();
    test_topic_builder_caps_fields_per_declaration();
    test_named_sample_writer_round_trips_by_name();
    test_named_sample_writer_rejects_the_wrong_field_name();
    test_source_info_round_trips();

    if (failures == 0) {
        std::cout << "test_catalog: all checks passed\n";
        return 0;
    }
    std::cerr << "test_catalog: " << failures << " check(s) failed\n";
    return 1;
}
