#include <Arduino.h>
#include <btp/codec.hpp>
#include <btp/fragmentation.hpp>
#include <btp/messages.hpp>
#include <btp/stream.hpp>
#include <btp/telemetry.hpp>

namespace {

std::uint8_t frame_buffer[btp::kEspNowMaxFrameSize];
std::uint8_t cobs_buffer[btp::kEspNowMaxFrameSize + 2U];
std::uint8_t decoded_buffer[btp::kEspNowMaxFrameSize];
std::uint8_t payload[] = {0x00U, 0x0aU, 0x0dU, 0xffU};
std::uint8_t message_buffer[128];

}  // namespace

void setup() {
    btp::Header header = {};
    header.type = btp::MessageType::Telemetry;
    header.source_id = 1U;
    header.boot_id = 1U;
    header.fragment_count = 1U;

    const btp::Frame source = {header, {payload, sizeof(payload)}};
    std::size_t written = 0U;
    if (btp::encode(source, btp::TransportProfile::EspNow, frame_buffer,
                    sizeof(frame_buffer), &written) != btp::Error::Ok) {
        abort();
    }

    btp::DecodedFrame decoded = {};
    if (btp::decode(frame_buffer, written, btp::TransportProfile::EspNow,
                    &decoded) != btp::Error::Ok ||
        decoded.payload.size != sizeof(payload)) {
        abort();
    }

    std::size_t cobs_size = 0U;
    std::size_t decoded_size = 0U;
    if (btp::cobs_encode(frame_buffer, written, cobs_buffer,
                         sizeof(cobs_buffer), &cobs_size) !=
            btp::CobsError::Ok ||
        btp::cobs_decode(cobs_buffer, cobs_size, decoded_buffer,
                         sizeof(decoded_buffer), &decoded_size) !=
            btp::CobsError::Ok ||
        decoded_size != written) {
        abort();
    }

    std::uint8_t fragments = 0U;
    if (btp::fragment_count(sizeof(payload), btp::TransportProfile::EspNow,
                            &fragments) != btp::Error::Ok ||
        fragments != 1U) {
        abort();
    }

    // btp::messages compiles and links on the embedded target. Phase 0: the
    // bodies are stubs, so the calls are exercised only for compilation, not
    // for a result.
    btp::Hello hello = {};
    std::size_t message_written = 0U;
    (void)btp::encode_hello(hello, message_buffer, sizeof(message_buffer),
                            &message_written);
    btp::Hello parsed_hello = {};
    (void)btp::decode_hello(message_buffer, sizeof(message_buffer),
                            &parsed_hello);

    btp::ManifestReader reader(message_buffer, sizeof(message_buffer));
    btp::ManifestHeader manifest_header = {};
    (void)reader.header(&manifest_header);

    btp::ByteView raw_info = {};
    btp::ByteView raw_topics = {};
    btp::ByteView raw_actions = {};
    (void)reader.raw_source_info(&raw_info);
    (void)reader.raw_records(&raw_topics, &raw_actions);

    btp::ManifestWriter writer(message_buffer, sizeof(message_buffer));
    (void)writer.begin(manifest_header);
    (void)writer.put_raw_source_info(raw_info);
    (void)writer.put_raw_records(raw_topics, raw_actions);
    (void)writer.finish(&message_written);

    // btp::telemetry compiles and links on the embedded target.
    static const btp::FieldSpec sample_fields[] = {
        {1U, 0U, static_cast<std::uint8_t>(btp::WireType::Float32), 0U, 1U, 0U, 1.0, 0.0},
        {2U, 1U, static_cast<std::uint8_t>(btp::WireType::Int16), 0U, 1U, 0U, 0.01, 0.0},
    };
    btp::SampleWriter sample_writer(message_buffer, sizeof(message_buffer),
                                    sample_fields, 2U);
    (void)sample_writer.begin(1U);
    (void)sample_writer.put_f64(12.5);
    (void)sample_writer.put_f64(3.14);
    (void)sample_writer.finish(&message_written);

    btp::SampleReader sample_reader(message_buffer, message_written,
                                    sample_fields, 2U, btp::kEncodingPackedLe);
    btp::SampleValue sample_value = {};
    while (sample_reader.next(&sample_value) == btp::SampleStep::Item) {
        (void)sample_value.f64(0);
    }
    (void)sample_reader.finish();
}

void loop() {}
