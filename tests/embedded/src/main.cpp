#include <Arduino.h>
#include <btp/codec.hpp>
#include <btp/fragmentation.hpp>
#include <btp/stream.hpp>

namespace {

std::uint8_t frame_buffer[btp::kEspNowMaxFrameSize];
std::uint8_t cobs_buffer[btp::kEspNowMaxFrameSize + 2U];
std::uint8_t decoded_buffer[btp::kEspNowMaxFrameSize];
std::uint8_t payload[] = {0x00U, 0x0aU, 0x0dU, 0xffU};

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
}

void loop() {}
