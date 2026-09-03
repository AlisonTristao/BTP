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

FieldRecord field(std::uint16_t id, std::uint16_t order, WireType type,
                  std::uint8_t flags, double scale, const char* name) {
    FieldRecord f = {};
    f.field_id = id;
    f.order = order;
    f.type = static_cast<std::uint8_t>(type);
    f.flags = flags;
    f.element_count = 1U;
    f.scale = scale;
    f.offset = 0.0;
    f.name = text(name);
    f.unit = text("");
    f.description = text("");
    return f;
}

const FieldRecord kDriveStatus[] = {
    field(1, 0, WireType::Float32, 0U, 1.0, "left_rpm"),
    field(2, 1, WireType::Float32, 0U, 1.0, "right_rpm"),
    field(3, 2, WireType::Uint16, 0U, 0.001, "battery_v"),
    field(4, 3, WireType::Int16, btp::kFieldNullable, 0.1, "temp_c"),
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
    // field order not contiguous
    FieldRecord bad[] = {field(1, 0, WireType::Uint8, 0U, 1.0, "a"),
                         field(2, 5, WireType::Uint8, 0U, 1.0, "b")};
    CHECK(cat.add_topic(0x0202U, 1U, TelemetryEncoding::PackedLe, false, 0U, "y",
                        bad, 2U) == MessageError::InvalidArgument);
}

void test_manifest_roundtrip() {
    btp::StaticCatalog<> producer;
    producer.set_config_revision(42U);
    producer.add_topic(0x0101U, 3U, TelemetryEncoding::PackedLe, true, 100000U,
                       "drive_status", kDriveStatus, 4U);

    static const FieldRecord kPose[] = {
        field(1, 0, WireType::Float64, 0U, 1.0, "x_m"),
        field(2, 1, WireType::Float64, 0U, 1.0, "y_m"),
        field(3, 2, WireType::Float32, 0U, 1.0, "heading_rad"),
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

}  // namespace

int main() {
    test_add_and_query();
    test_manifest_roundtrip();
    test_not_modified();
    test_capacity();
    test_bad_manifest();

    if (failures == 0) {
        std::cout << "test_catalog: all checks passed\n";
        return 0;
    }
    std::cerr << "test_catalog: " << failures << " check(s) failed\n";
    return 1;
}
