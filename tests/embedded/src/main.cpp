#include <Arduino.h>
#include <btp/codec.hpp>
#include <btp/endpoint.hpp>
#include <btp/fragmentation.hpp>
#include <btp/messages.hpp>
#include <btp/receiver.hpp>
#include <btp/session.hpp>
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
    (void)sample_writer.begin(1U, btp::SampleLayout::BodyOnly);
    (void)sample_writer.put_f64(12.5);
    (void)sample_writer.put_f64(3.14);
    (void)sample_writer.finish(&message_written);

    btp::SampleReader sample_reader(message_buffer, message_written,
                                    sample_fields, 2U, btp::kEncodingPackedLe,
                                    btp::SampleLayout::BodyOnly);
    btp::SampleValue sample_value = {};
    while (sample_reader.next(&sample_value) == btp::SampleStep::Item) {
        (void)sample_value.f64(0);
    }
    (void)sample_reader.finish();

    // btp::session compiles and links on the embedded target: a fixed slot
    // array, one caller storage region per slot and a small requester table,
    // no allocation, no exceptions.
    static btp::DedupSlot dedup_slots[2];
    static std::uint8_t dedup_bytes[2][64];
    static btp::DedupStorage dedup_storage[2] = {
        {dedup_bytes[0], sizeof(dedup_bytes[0])},
        {dedup_bytes[1], sizeof(dedup_bytes[1])},
    };
    static btp::DedupRequester dedup_requesters[2];
    btp::DedupCache dedup(dedup_slots, dedup_storage, 2U, dedup_requesters, 2U);
    const btp::DedupKey dedup_key = {1U, 1U, 1U};
    std::size_t dedup_slot = 0U;
    btp::ByteView dedup_result = {};
    if (dedup.classify(dedup_key, payload, sizeof(payload), &dedup_slot,
                       &dedup_result) == btp::DedupVerdict::Fresh) {
        (void)dedup.record_result(dedup_slot, payload, sizeof(payload));
    }
    (void)dedup.classify(dedup_key, payload, sizeof(payload), &dedup_slot,
                         &dedup_result);
    dedup.clear();

    // btp::endpoint compiles and links on the embedded target: identity, the
    // atomic sequence counter and the seal -> fragment -> encode pipeline, no
    // allocation, no exceptions. The send callback copies each frame into a
    // static buffer so nothing here needs a radio.
    static btp::Endpoint endpoint;
    (void)endpoint.configure(0x0A0B0C0DU, 0x01020304U);
    std::uint32_t endpoint_sequence = 0U;
    (void)endpoint.reserve_sequence(&endpoint_sequence);
    (void)endpoint.try_reserve_sequence(&endpoint_sequence);

    static std::uint8_t endpoint_last_frame[btp::kEspNowMaxFrameSize];
    static std::size_t endpoint_last_frame_size = 0U;
    struct EndpointSink {
        static bool send(void*, const std::uint8_t* frame, std::size_t size) {
            endpoint_last_frame_size =
                size <= sizeof(endpoint_last_frame) ? size : 0U;
            for (std::size_t i = 0U; i < endpoint_last_frame_size; ++i) {
                endpoint_last_frame[i] = frame[i];
            }
            return endpoint_last_frame_size != 0U;
        }
    };
    const btp::LogicalMessage endpoint_message = {
        btp::MessageType::Telemetry, 1U, 0U, {payload, sizeof(payload)}};
    (void)endpoint.send_logical(endpoint_message, btp::TransportProfile::EspNow,
                                &EndpointSink::send, nullptr, nullptr, 0U);
    (void)endpoint.send_fragment(endpoint_message, btp::TransportProfile::EspNow,
                                 endpoint_sequence, 0U, 1U, &EndpointSink::send,
                                 nullptr);

    // btp::receiver compiles and links on the embedded target: decode + CRC +
    // reassembly against caller-owned slots, the timeout sweep and the STATUS
    // counters, no allocation, no exceptions. Feed it the frame the endpoint
    // just produced.
    static btp::ReassemblySlot receiver_slots[2];
    static std::uint8_t receiver_bytes[2][btp::kEspNowMaxPayloadSize];
    static btp::ReassemblyStorage receiver_storage[2] = {
        {receiver_bytes[0], sizeof(receiver_bytes[0])},
        {receiver_bytes[1], sizeof(receiver_bytes[1])},
    };
    static btp::Receiver receiver(receiver_slots, receiver_storage, 2U, 4000U,
                                  btp::TransportProfile::EspNow);
    static std::uint8_t receiver_out[btp::kEspNowMaxPayloadSize];
    btp::ReceivedMessage received = {};
    (void)receiver.submit(endpoint_last_frame, endpoint_last_frame_size, 1U,
                          receiver_out, sizeof(receiver_out), &received);
    (void)receiver.expire(10000U);
    (void)receiver.stats();
    receiver.clear();
}

void loop() {}
