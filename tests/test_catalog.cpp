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

#include <cmath>
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

using btp::ActionEncoding;
using btp::Catalog;
using btp::CatalogAction;
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
                      std::size_t capacity, std::uint8_t manifest_flags = 0U,
                      std::uint16_t format_version = 1U) {
    btp::ManifestHeader header = {};
    header.status = static_cast<std::uint8_t>(btp::ResultStatus::Success);
    header.flags = manifest_flags;
    header.manifest_format_version = format_version;
    header.config_revision = source.config_revision();
    header.described_source_id = 0x00CAFE01U;
    header.described_boot_id = 0x0000B001U;
    header.source_role = static_cast<std::uint8_t>(btp::Role::Producer);
    header.source_flags = btp::kSourceOnline;
    header.catalog_count = 1U;
    const bool not_modified = (manifest_flags & btp::kManifestNotModified) != 0U;
    header.topic_count =
        not_modified ? 0U : static_cast<std::uint16_t>(source.topic_count());
    header.action_count =
        not_modified ? 0U : static_cast<std::uint16_t>(source.action_count());
    header.source_name = text("test-source");

    btp::ManifestWriter w(out, capacity);
    if (w.begin(header) != MessageError::Ok) return 0U;
    if (!not_modified) {
        if (source.write_topics(&w) != MessageError::Ok) return 0U;
        if (source.write_actions(&w) != MessageError::Ok) return 0U;
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

// min_rate_millihz / default_rate_millihz (library 2.40.0) are local
// SUBSCRIBE-granting policy for SubscriptionTable::handle_subscribe() (see
// btp/subscription.hpp) -- CatalogTopic carries them, but neither is a wire
// field: write_topics() never emits them and ingest() always reads back 0,
// even when the producer side set both.
void test_rate_policy_is_local_and_never_on_the_wire() {
    btp::StaticCatalog<> cat;
    CHECK(cat.add_topic(0x0101U, 3U, TelemetryEncoding::PackedLe, true,
                        /*max_rate_millihz=*/100000U, "drive_status", kDriveStatus, 4U,
                        /*min_rate_millihz=*/1000U,
                        /*default_rate_millihz=*/5000U) == MessageError::Ok);
    const CatalogTopic* t = cat.topic(0x0101U);
    CHECK(t != nullptr);
    CHECK(t->min_rate_millihz == 1000U);
    CHECK(t->default_rate_millihz == 5000U);

    // The template array-form overload and the TopicBuilder chain both reach
    // the same two trailing arguments.
    CHECK(cat.add_topic(0x0102U, 1U, "templated", kDriveStatus,
                        TelemetryEncoding::PackedLe, /*subscribable=*/true,
                        /*max_rate_millihz=*/0U, /*min_rate_millihz=*/200U,
                        /*default_rate_millihz=*/300U) == MessageError::Ok);
    const CatalogTopic* t2 = cat.topic(0x0102U);
    CHECK(t2 != nullptr && t2->min_rate_millihz == 200U && t2->default_rate_millihz == 300U);

    CHECK(cat.topic(0x0103U, 1U, "built").f32("x").min_rate(10U).default_rate(20U).end() ==
          MessageError::Ok);
    const CatalogTopic* t3 = cat.topic(0x0103U);
    CHECK(t3 != nullptr && t3->min_rate_millihz == 10U && t3->default_rate_millihz == 20U);

    // Round-trip: a consumer's ingest()ed copy never carries either.
    std::uint8_t wire[1024];
    const std::size_t n = serialise(cat, wire, sizeof(wire));
    CHECK(n != 0U);
    btp::StaticCatalog<> consumer;
    CHECK(consumer.ingest(wire, n) == MessageError::Ok);
    const CatalogTopic* rt = consumer.topic(0x0101U);
    CHECK(rt != nullptr);
    CHECK(rt->max_rate_millihz == 100000U);  // the one rate field that IS wire
    CHECK(rt->min_rate_millihz == 0U);
    CHECK(rt->default_rate_millihz == 0U);
}

// A body-only topic (docs/commands.md section 3.3: OpaqueBytes/Utf8/
// JsonUtf8/CsvUtf8 carry a raw document, not field-structured samples) has
// field_count == 0 by definition -- add_topic() accepts that, and
// write_topics() serialises a valid, empty field list for it. This is the
// case that used to be hand-encoded around the Catalog entirely (a fieldless
// topic could describe itself on the wire but never entered a Catalog); see
// library 2.39.0.
void test_body_only_topic() {
    btp::StaticCatalog<> cat;
    CHECK(cat.add_topic(0x0003U, 1U, TelemetryEncoding::Utf8, /*subscribable=*/true,
                        0U, "system.monitor", nullptr, 0U) == MessageError::Ok);
    CHECK(cat.topic_count() == 1U);

    const CatalogTopic* t = cat.topic(0x0003U);
    CHECK(t != nullptr);
    CHECK(t->field_count == 0U);
    CHECK(t->encoding == static_cast<std::uint8_t>(TelemetryEncoding::Utf8));
    CHECK(std::strcmp(t->name, "system.monitor") == 0);

    // A second, field-bearing topic still adds normally alongside it.
    CHECK(cat.add_topic(0x0101U, 3U, "drive_status", kDriveStatus) ==
          MessageError::Ok);
    CHECK(cat.topic_count() == 2U);

    // Round-trips through a real MANIFEST_DATA: the body-only topic's record
    // carries field_count 0 and no field rows, the other topic is unaffected.
    std::uint8_t wire[1024];
    const std::size_t n = serialise(cat, wire, sizeof(wire));
    CHECK(n != 0U);

    btp::StaticCatalog<> consumer;
    CHECK(consumer.ingest(wire, n) == MessageError::Ok);
    CHECK(consumer.topic_count() == 2U);
    const CatalogTopic* rt = consumer.topic(0x0003U);
    CHECK(rt != nullptr);
    CHECK(rt->field_count == 0U);
    CHECK(consumer.topic(0x0101U) != nullptr);
    CHECK(consumer.topic(0x0101U)->field_count == 4U);

    // fields==nullptr is only rejected when field_count says otherwise.
    btp::StaticCatalog<> rejects;
    CHECK(rejects.add_topic(0x0202U, 1U, TelemetryEncoding::PackedLe, false, 0U,
                            "bad", nullptr, 1U) == MessageError::InvalidArgument);
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

void test_field_min_and_max() {
    // Three fields: a declared range, a one-sided range (max only), and no
    // range at all -- the last two exercise btp::kNoRangeBound (NaN).
    FieldRecord fields[] = {
        btp::range(btp::f32("current_a", "A"), 0.0, 5.0),
        btp::range(btp::f32("headroom_a", "A"), btp::kNoRangeBound, 5.0),
        btp::f32("gyro_z", "rad/s"),
    };

    btp::StaticCatalog<> producer;
    CHECK(producer.has_field_ranges() == false);
    CHECK(producer.add_topic(0x0401U, 1U, "battery", fields) == MessageError::Ok);
    CHECK(producer.has_field_ranges() == true);
    const CatalogTopic* pt = producer.topic(0x0401U);
    CHECK(pt != nullptr);
    CHECK(producer.field_min(*pt, 0) == 0.0);
    CHECK(producer.field_max(*pt, 0) == 5.0);
    CHECK(std::isnan(producer.field_min(*pt, 1)));
    CHECK(producer.field_max(*pt, 1) == 5.0);
    CHECK(std::isnan(producer.field_min(*pt, 2)));
    CHECK(std::isnan(producer.field_max(*pt, 2)));
    // Out of range -- "unset" (NaN), never a crash.
    CHECK(std::isnan(producer.field_min(*pt, 99U)));

    // Format 1/2: min/max never reach the wire, so a consumer sees no range at
    // all -- has_field_ranges() driven the real producer path (btp::Node) to
    // format 3 (see src/node.cpp), but a Catalog fed a format-1 payload by
    // hand must still come back empty, not corrupted.
    std::uint8_t wire1[512];
    const std::size_t n1 = serialise(producer, wire1, sizeof(wire1), 0U, 1U);
    CHECK(n1 != 0U);
    btp::StaticCatalog<> consumer1;
    CHECK(consumer1.ingest(wire1, n1) == MessageError::Ok);
    const CatalogTopic* ct1 = consumer1.topic(0x0401U);
    CHECK(ct1 != nullptr);
    CHECK(std::isnan(consumer1.field_min(*ct1, 0)));
    CHECK(std::isnan(consumer1.field_max(*ct1, 0)));
    CHECK(consumer1.has_field_ranges() == false);

    // Format 3: the real thing, round-tripped through a wire MANIFEST_DATA.
    std::uint8_t wire3[512];
    const std::size_t n3 = serialise(producer, wire3, sizeof(wire3), 0U, 3U);
    CHECK(n3 != 0U);
    btp::StaticCatalog<> consumer3;
    CHECK(consumer3.ingest(wire3, n3) == MessageError::Ok);
    const CatalogTopic* ct3 = consumer3.topic(0x0401U);
    CHECK(ct3 != nullptr);
    CHECK(consumer3.field_min(*ct3, 0) == 0.0);
    CHECK(consumer3.field_max(*ct3, 0) == 5.0);
    CHECK(std::isnan(consumer3.field_min(*ct3, 1)));
    CHECK(consumer3.field_max(*ct3, 1) == 5.0);
    CHECK(std::isnan(consumer3.field_min(*ct3, 2)));
    CHECK(std::isnan(consumer3.field_max(*ct3, 2)));
    CHECK(consumer3.has_field_ranges() == true);

    // And once more out of the consumer -- write_topics() must serve back
    // what it ingested (producer and consumer roles are symmetric).
    std::uint8_t wire3b[512];
    const std::size_t n3b = serialise(consumer3, wire3b, sizeof(wire3b), 0U, 3U);
    CHECK(n3b != 0U);
    btp::StaticCatalog<> consumer3b;
    CHECK(consumer3b.ingest(wire3b, n3b) == MessageError::Ok);
    const CatalogTopic* ct3b = consumer3b.topic(0x0401U);
    CHECK(ct3b != nullptr);
    CHECK(consumer3b.field_min(*ct3b, 0) == 0.0);
    CHECK(consumer3b.field_max(*ct3b, 0) == 5.0);
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

void test_topic_builder_range() {
    btp::StaticCatalog<> cat;
    CHECK(cat.topic(0x0102U, 1U, "battery")
             .f32("current_a", "A")
             .range(0.0, 5.0)
             .f32("gyro_z", "rad/s")  // no .range(): stays unset (NaN)
             .end() == MessageError::Ok);
    const CatalogTopic* t = cat.topic(0x0102U);
    CHECK(t != nullptr);
    CHECK(cat.field_min(*t, 0) == 0.0);
    CHECK(cat.field_max(*t, 0) == 5.0);
    CHECK(std::isnan(cat.field_min(*t, 1)));
    CHECK(std::isnan(cat.field_max(*t, 1)));

    // .range() before any field is added is WrongOrder, same as a chain call
    // after an earlier error -- both are sticky and end() surfaces them.
    btp::StaticCatalog<> empty_chain;
    CHECK(empty_chain.topic(0x0103U, 1U, "nothing").range(0.0, 1.0).end() ==
          MessageError::WrongOrder);
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


// ---------------------------------------------------------------------------
// Actions (command records)
// ---------------------------------------------------------------------------

// A topic + two actions: a DANGEROUS+IDEMPOTENT one with parameters, results,
// a range, a timeout and all three strings, and a parameters-only STREAM one.
const FieldRecord kArmParams[] = {
    btp::range(btp::f32("speed_mm_s", "mm/s"), 0.0, 500.0),
    btp::u8("mode"),
};
const FieldRecord kArmResults[] = {
    btp::f32("ack_time", "ms"),
};

MessageError add_test_actions(Catalog* c) {
    MessageError e = c->add_action(
        0x0201U, 1U,
        static_cast<std::uint16_t>(btp::kActionDangerous | btp::kActionIdempotent),
        ActionEncoding::PackedLe, "arm", kArmParams, 2U, kArmResults, 1U,
        ActionEncoding::PackedLe, 1500U, "Arm the drive", "Really arm?");
    if (e != MessageError::Ok) return e;
    return c->action(0x0202U, 2U, "set_target")
        .f32("x_mm", "mm")
        .f32("y_mm", "mm")
        .stream()
        .timeout_ms(50U)
        .end();
}

void check_test_actions(const Catalog& c) {
    CHECK(c.action_count() == 2U);

    const CatalogAction* arm = c.action(0x0201U, 1U);
    CHECK(arm != nullptr);
    if (arm != nullptr) {
        CHECK(arm->action_id == 0x0201U && arm->action_version == 1U);
        CHECK(arm->flags == (btp::kActionDangerous | btp::kActionIdempotent));
        CHECK(arm->parameter_encoding == static_cast<std::uint8_t>(ActionEncoding::PackedLe));
        CHECK(arm->result_encoding == static_cast<std::uint8_t>(ActionEncoding::PackedLe));
        CHECK(arm->execution_timeout_ms == 1500U);
        CHECK(std::strcmp(arm->name, "arm") == 0);
        CHECK(std::strcmp(arm->description, "Arm the drive") == 0);
        CHECK(std::strcmp(arm->confirmation_text, "Really arm?") == 0);
        CHECK(arm->parameter_count == 2U && arm->result_count == 1U);
        // Parameters, then results; ids and orders are per side.
        CHECK(arm->fields[0].field_id == 1U && arm->fields[0].order == 0U);
        CHECK(arm->fields[1].field_id == 2U && arm->fields[1].order == 1U);
        CHECK(arm->fields[2].field_id == 1U && arm->fields[2].order == 0U);
        CHECK(arm->fields[0].type == static_cast<std::uint8_t>(WireType::Float32));
        CHECK(arm->fields[1].type == static_cast<std::uint8_t>(WireType::Uint8));
        CHECK(std::strcmp(c.field_name(*arm, 0), "speed_mm_s") == 0);
        CHECK(std::strcmp(c.field_name(*arm, 1), "mode") == 0);
        CHECK(std::strcmp(c.field_name(*arm, 2), "ack_time") == 0);
        CHECK(std::strcmp(c.field_unit(*arm, 0), "mm/s") == 0);
        CHECK(std::strcmp(c.field_unit(*arm, 2), "ms") == 0);
        CHECK(std::strcmp(c.field_unit(*arm, 1), "") == 0);
        CHECK(c.field_min(*arm, 0) == 0.0 && c.field_max(*arm, 0) == 500.0);
        CHECK(std::isnan(c.field_min(*arm, 1)) && std::isnan(c.field_max(*arm, 2)));
        // Out of range -- "" / NaN, never a crash.
        CHECK(std::strcmp(c.field_name(*arm, 3), "") == 0);
        CHECK(std::isnan(c.field_min(*arm, 99U)));
    }

    const CatalogAction* target = c.action(0x0202U, 2U);
    CHECK(target != nullptr);
    if (target != nullptr) {
        CHECK(target->flags == btp::kActionStream);
        CHECK(target->parameter_encoding == static_cast<std::uint8_t>(ActionEncoding::PackedLe));
        CHECK(target->result_encoding == static_cast<std::uint8_t>(ActionEncoding::Empty));
        CHECK(target->execution_timeout_ms == 50U);
        CHECK(target->parameter_count == 2U && target->result_count == 0U);
        CHECK(std::strcmp(target->description, "") == 0);
        CHECK(std::strcmp(c.field_name(*target, 1), "y_mm") == 0);
        CHECK(std::strcmp(c.field_unit(*target, 1), "mm") == 0);
    }

    CHECK(c.action_at(0U) == arm);
    CHECK(c.action_at(1U) == target);
    CHECK(c.action_at(2U) == nullptr);
    // (id, version) is the key: a different version of the id is not the action.
    CHECK(c.action(0x0201U, 2U) == nullptr);
    CHECK(c.action(0x0999U, 1U) == nullptr);
}

void test_action_round_trip() {
    btp::StaticCatalog<4, 32, 1024, 0, 4> producer;
    CHECK(producer.add_topic(0x0101U, 3U, "drive_status", kDriveStatus) == MessageError::Ok);
    CHECK(add_test_actions(&producer) == MessageError::Ok);
    check_test_actions(producer);
    CHECK(producer.has_field_ranges());

    // Format 3: the range travels.
    std::uint8_t wire[1024];
    const std::size_t n = serialise(producer, wire, sizeof(wire), 0U, 3U);
    CHECK(n != 0U);

    btp::StaticCatalog<4, 32, 1024, 0, 4> consumer;
    CHECK(consumer.ingest(wire, n) == MessageError::Ok);
    CHECK(consumer.topic_count() == 1U);
    CHECK(consumer.topic(0x0101U) != nullptr);
    check_test_actions(consumer);

    // Symmetric: a consumer re-serves what it learned.
    std::uint8_t wire2[1024];
    const std::size_t n2 = serialise(consumer, wire2, sizeof(wire2), 0U, 3U);
    CHECK(n2 == n && std::memcmp(wire, wire2, n) == 0);

    // Re-ingest replaces, not appends.
    CHECK(consumer.ingest(wire, n) == MessageError::Ok);
    check_test_actions(consumer);

    // Format 1: the range does not travel; everything else does.
    std::uint8_t wire1[1024];
    const std::size_t n1 = serialise(producer, wire1, sizeof(wire1), 0U, 1U);
    CHECK(n1 != 0U);
    btp::StaticCatalog<4, 32, 1024, 0, 4> consumer1;
    CHECK(consumer1.ingest(wire1, n1) == MessageError::Ok);
    const CatalogAction* arm1 = consumer1.action(0x0201U, 1U);
    CHECK(arm1 != nullptr && std::isnan(consumer1.field_min(*arm1, 0)));
    CHECK(arm1 != nullptr && std::strcmp(consumer1.field_name(*arm1, 2), "ack_time") == 0);

    // A consumer WITHOUT an action pool steps over the records.
    btp::StaticCatalog<4, 32, 1024> plain;
    CHECK(plain.ingest(wire, n) == MessageError::Ok);
    CHECK(plain.topic_count() == 1U);
    CHECK(plain.action_count() == 0U);
    CHECK(plain.action(0x0201U, 1U) == nullptr);

    // A pool that is too small clears the whole catalogue.
    btp::StaticCatalog<4, 32, 1024, 0, 1> small_actions;
    CHECK(small_actions.ingest(wire, n) == MessageError::BufferTooSmall);
    CHECK(small_actions.topic_count() == 0U && small_actions.action_count() == 0U);
    btp::StaticCatalog<4, 7, 1024, 0, 4> small_fields;  // 4 topic + 5 action fields needed
    CHECK(small_fields.ingest(wire, n) == MessageError::BufferTooSmall);
    CHECK(small_fields.topic_count() == 0U && small_fields.action_count() == 0U);

    // A STREAM bit patched onto an action with results is a malformed manifest.
    std::uint8_t bad[1024];
    std::memcpy(bad, wire, n);
    std::size_t flags_at = 0U;
    for (std::size_t i = 0U; i + 6U <= n; ++i) {
        if (bad[i] == 0x01U && bad[i + 1U] == 0x02U && bad[i + 2U] == 0x01U &&
            bad[i + 3U] == 0x00U) {  // action_id 0x0201, version 1
            flags_at = i + 4U;
            break;
        }
    }
    CHECK(flags_at != 0U);
    bad[flags_at] = static_cast<std::uint8_t>(bad[flags_at] | btp::kActionStream);
    btp::StaticCatalog<4, 32, 1024, 0, 4> rejecting;
    CHECK(rejecting.ingest(bad, n) == MessageError::InvalidValue);
    CHECK(rejecting.topic_count() == 0U && rejecting.action_count() == 0U);

    // clear() drops the actions along with everything else.
    consumer.clear();
    CHECK(consumer.action_count() == 0U && consumer.action_at(0U) == nullptr);
    CHECK(consumer.has_field_ranges() == false);
}

void test_action_revision() {
    const FieldRecord drive[] = {
        btp::f32("left_rpm", "rpm"),
        btp::f32("right_rpm", "rpm"),
        btp::u16("battery_v", 0.001, "V"),
        btp::nullable(btp::i16("temp_c", 0.1, "Cel")),
    };
    const FieldRecord pose[] = {
        btp::range(btp::f64("x_m", "m"), -10.0, 10.0),
        btp::f64("y_m", "m"),
        btp::f32("heading_rad", "rad"),
    };
    btp::StaticCatalog<4, 32, 1024, 0, 4> c;
    CHECK(c.add_topic(0x0101U, 3U, TelemetryEncoding::PackedLe, true, 100000U,
                      "drive_status", drive, 4U) == MessageError::Ok);
    CHECK(c.add_topic(0x0102U, 1U, TelemetryEncoding::PackedLe, true, 50000U,
                      "pose", pose, 3U) == MessageError::Ok);

    // Golden value of this exact catalogue, computed before actions existed:
    // actions must not move the revision of a catalogue that has none.
    CHECK(c.content_revision() == 2948512862U);

    CHECK(c.action(0x0010U, 1U, "go").f32("x").end() == MessageError::Ok);
    const std::uint32_t with_one = c.content_revision();
    CHECK(with_one != 2948512862U && with_one != 0U);

    // Any field of the action is covered.
    btp::StaticCatalog<4, 32, 1024, 0, 4> d;
    d.add_topic(0x0101U, 3U, TelemetryEncoding::PackedLe, true, 100000U,
                "drive_status", drive, 4U);
    d.add_topic(0x0102U, 1U, TelemetryEncoding::PackedLe, true, 50000U, "pose", pose, 3U);
    CHECK(d.action(0x0010U, 1U, "go").f32("x").stream().end() == MessageError::Ok);
    CHECK(d.content_revision() != with_one);  // flags
    btp::StaticCatalog<4, 32, 1024, 0, 4> e;
    e.add_topic(0x0101U, 3U, TelemetryEncoding::PackedLe, true, 100000U,
                "drive_status", drive, 4U);
    e.add_topic(0x0102U, 1U, TelemetryEncoding::PackedLe, true, 50000U, "pose", pose, 3U);
    CHECK(e.action(0x0010U, 1U, "go").f32("x", "mm").end() == MessageError::Ok);
    CHECK(e.content_revision() != with_one);  // a field's unit
}

void test_action_validation() {
    const FieldRecord one[] = {btp::f32("x")};
    const FieldRecord two[] = {btp::f32("x"), btp::f32("y")};
    const std::uint16_t none = 0U;

    // No action pool at all.
    btp::StaticCatalog<> no_pool;
    CHECK(no_pool.add_action(1U, 1U, none, ActionEncoding::PackedLe, "a", one, 1U) ==
          MessageError::InvalidArgument);
    CHECK(no_pool.action(1U, 1U, "a").f32("x").end() == MessageError::InvalidArgument);

    btp::StaticCatalog<4, 32, 1024, 0, 2> c;
    CHECK(c.add_action(1U, 1U, none, ActionEncoding::PackedLe, "a", one, 1U) == MessageError::Ok);
    // Duplicate (id, version); the same id at another version is fine.
    CHECK(c.add_action(1U, 1U, none, ActionEncoding::PackedLe, "b", one, 1U) ==
          MessageError::InvalidArgument);
    CHECK(c.add_action(1U, 2U, none, ActionEncoding::PackedLe, "b", one, 1U) == MessageError::Ok);
    // Pool full.
    CHECK(c.add_action(2U, 1U, none, ActionEncoding::PackedLe, "c", one, 1U) ==
          MessageError::BufferTooSmall);
    CHECK(c.action_count() == 2U);

    btp::StaticCatalog<4, 32, 1024, 0, 4> v;
    // Zero id / version.
    CHECK(v.add_action(0U, 1U, none, ActionEncoding::PackedLe, "a", one, 1U) ==
          MessageError::InvalidArgument);
    CHECK(v.add_action(1U, 0U, none, ActionEncoding::PackedLe, "a", one, 1U) ==
          MessageError::InvalidArgument);
    // Null field array with a non-zero count.
    CHECK(v.add_action(1U, 1U, none, ActionEncoding::PackedLe, "a", nullptr, 1U) ==
          MessageError::InvalidArgument);
    CHECK(v.add_action(1U, 1U, none, ActionEncoding::PackedLe, "a", one, 1U, nullptr, 1U,
                       ActionEncoding::PackedLe) == MessageError::InvalidArgument);
    // Field order not 0,1,2... on either side.
    FieldRecord bad_order[] = {btp::f32("x"), btp::f32("y")};
    bad_order[1].order = 5U;
    CHECK(v.add_action(1U, 1U, none, ActionEncoding::PackedLe, "a", bad_order, 2U) ==
          MessageError::InvalidArgument);
    CHECK(v.add_action(1U, 1U, none, ActionEncoding::PackedLe, "a", one, 1U, bad_order, 2U,
                       ActionEncoding::PackedLe) == MessageError::InvalidArgument);
    // An EMPTY encoding cannot carry fields...
    CHECK(v.add_action(1U, 1U, none, ActionEncoding::Empty, "a", one, 1U) ==
          MessageError::InvalidArgument);
    CHECK(v.add_action(1U, 1U, none, ActionEncoding::PackedLe, "a", one, 1U, two, 2U,
                       ActionEncoding::Empty) == MessageError::InvalidArgument);
    // ...but is fine with none (no parameters, no results).
    CHECK(v.add_action(1U, 1U, none, ActionEncoding::Empty, "a", nullptr, 0U) ==
          MessageError::Ok);
    // STREAM: no results, EMPTY result encoding.
    CHECK(v.add_action(2U, 1U, btp::kActionStream, ActionEncoding::PackedLe, "s", one, 1U,
                       two, 2U, ActionEncoding::PackedLe) == MessageError::InvalidArgument);
    CHECK(v.add_action(2U, 1U, btp::kActionStream, ActionEncoding::PackedLe, "s", one, 1U,
                       nullptr, 0U, ActionEncoding::PackedLe) == MessageError::InvalidArgument);
    CHECK(v.add_action(2U, 1U, btp::kActionStream, ActionEncoding::PackedLe, "s", one, 1U) ==
          MessageError::Ok);
    // Nothing the failed calls tried to add was kept.
    CHECK(v.action_count() == 2U);

    // The field pool is shared with the topics: 4 fields in total.
    btp::StaticCatalog<4, 4, 256, 0, 4> shared;
    CHECK(shared.add_topic(0x0101U, 1U, "t", kDriveStatus) == MessageError::Ok);
    CHECK(shared.add_action(1U, 1U, none, ActionEncoding::PackedLe, "a", one, 1U) ==
          MessageError::BufferTooSmall);
    CHECK(shared.action_count() == 0U);
}

void test_action_builder() {
    btp::StaticCatalog<4, 64, 1024, 0, 4> c;
    CHECK(c.action(0x0030U, 1U, "drive")
              .f32("left", "rpm")
              .range(-100.0, 100.0)
              .u16("ramp_ms", 1.0, "ms", /*is_nullable=*/true)
              .i8("i8")
              .boolean("enabled")
              .enum8("gear")
              .field(9U, WireType::Int32, "explicit_id")
              .dangerous()
              .idempotent()
              .description("Drive the wheels")
              .confirmation("Sure?")
              .timeout_ms(250U)
              .end() == MessageError::Ok);
    const CatalogAction* a = c.action(0x0030U, 1U);
    CHECK(a != nullptr);
    if (a != nullptr) {
        CHECK(a->flags == (btp::kActionDangerous | btp::kActionIdempotent));
        CHECK(a->parameter_count == 6U && a->result_count == 0U);
        CHECK(a->parameter_encoding == static_cast<std::uint8_t>(ActionEncoding::PackedLe));
        CHECK(a->result_encoding == static_cast<std::uint8_t>(ActionEncoding::Empty));
        CHECK(a->execution_timeout_ms == 250U);
        CHECK(std::strcmp(a->description, "Drive the wheels") == 0);
        CHECK(std::strcmp(a->confirmation_text, "Sure?") == 0);
        CHECK(c.field_min(*a, 0) == -100.0 && c.field_max(*a, 0) == 100.0);
        CHECK((a->fields[1].flags & btp::kFieldNullable) != 0U);
        CHECK(a->fields[5].field_id == 9U);
        CHECK(a->fields[4].field_id == 5U);
    }

    // No parameters: EMPTY encoding, still a valid (e.g. trigger) action.
    CHECK(c.action(0x0031U, 1U, "ping").end() == MessageError::Ok);
    const CatalogAction* ping = c.action(0x0031U, 1U);
    CHECK(ping != nullptr && ping->parameter_count == 0U);
    CHECK(ping != nullptr &&
          ping->parameter_encoding == static_cast<std::uint8_t>(ActionEncoding::Empty));

    // Errors: add_action()'s, and the sticky builder ones.
    CHECK(c.action(0x0031U, 1U, "again").end() == MessageError::InvalidArgument);
    CHECK(c.action(0x0032U, 1U, "r").range(0.0, 1.0).end() == MessageError::WrongOrder);
    btp::ActionBuilder many = c.action(0x0033U, 1U, "many");
    for (std::size_t i = 0U; i <= btp::ActionBuilder::kMaxFields; ++i) many.u8("f");
    CHECK(many.end() == MessageError::CountTooLarge);
    CHECK(c.action_count() == 2U);
}

void test_static_catalog_actions_template_argument() {
    // Existing instantiations keep compiling and have no action pool.
    btp::StaticCatalog<> plain;
    CHECK(plain.valid() && plain.action_count() == 0U);
    btp::StaticCatalog<4, 16, 256, 2> with_info;
    CHECK(with_info.valid() && with_info.action_count() == 0U);
    CHECK(with_info.action(1U, 1U, "x").end() == MessageError::InvalidArgument);

    // The same instantiation with actions, alongside source_info.
    btp::StaticCatalog<4, 16, 256, 2, 3> both;
    CHECK(both.add_source_info("fw", "", "1") == MessageError::Ok);
    CHECK(both.action(1U, 1U, "x").u8("v").end() == MessageError::Ok);
    CHECK(both.action_count() == 1U && both.source_info_count() == 1U);

    // Fields of an action only: a range anywhere counts for has_field_ranges().
    btp::StaticCatalog<4, 16, 256, 0, 3> only_action;
    CHECK(only_action.has_field_ranges() == false);
    CHECK(only_action.action(1U, 1U, "x").f32("v").end() == MessageError::Ok);
    CHECK(only_action.has_field_ranges() == false);
    CHECK(only_action.action(2U, 1U, "y").f32("v").range(0.0, 1.0).end() == MessageError::Ok);
    CHECK(only_action.has_field_ranges() == true);
}

}  // namespace

int main() {
    test_add_and_query();
    test_rate_policy_is_local_and_never_on_the_wire();
    test_body_only_topic();
    test_schema_helpers();
    test_manifest_roundtrip();
    test_field_unit_and_description();
    test_field_min_and_max();
    test_not_modified();
    test_capacity();
    test_bad_manifest();

    test_topic_builder_matches_the_array_form();
    test_topic_builder_range();
    test_topic_builder_propagates_add_topic_errors();
    test_topic_builder_caps_fields_per_declaration();
    test_named_sample_writer_round_trips_by_name();
    test_named_sample_writer_rejects_the_wrong_field_name();
    test_source_info_round_trips();
    test_action_round_trip();
    test_action_revision();
    test_action_validation();
    test_action_builder();
    test_static_catalog_actions_template_argument();

    if (failures == 0) {
        std::cout << "test_catalog: all checks passed\n";
        return 0;
    }
    std::cerr << "test_catalog: " << failures << " check(s) failed\n";
    return 1;
}
