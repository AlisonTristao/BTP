// Unit tests for btp::TxQueue / btp::StaticTxQueue (include/btp/txqueue.hpp):
// the send-side priority queue. Frames are either opaque bytes pushed under an
// explicit class, or real encoded frames classified from their own header.

#include "btp/txqueue.hpp"

#include "btp/codec.hpp"
#include "btp/messages.hpp"
#include "btp/node.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

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

using btp::PriorityClass;
using btp::TxPush;

// One-octet "frames" tagged by value, pushed under an explicit class.
TxPush push_tag(btp::TxQueue& queue, std::uint8_t tag, PriorityClass priority) {
    return queue.push(&tag, 1U, priority);
}

std::uint8_t pop_tag(btp::TxQueue& queue) {
    const std::uint8_t* frame = nullptr;
    std::size_t size = 0U;
    if (!queue.front(&frame, &size) || size != 1U) return 0xFFU;
    const std::uint8_t tag = frame[0];
    queue.pop();
    return tag;
}

std::vector<std::uint8_t> encoded_frame(btp::MessageType type, std::uint16_t object_id) {
    const std::uint8_t body[4] = {1, 2, 3, 4};
    btp::Header header{};
    header.type = type;
    header.flags = 0U;
    header.source_id = 0x00CAFE01U;
    header.boot_id = 0x0000B001U;
    header.sequence = 7U;
    header.timestamp_us = 0U;
    header.object_id = object_id;
    header.fragment_index = 0U;
    header.fragment_count = 1U;
    const btp::Frame frame{header, {body, sizeof(body)}};
    std::vector<std::uint8_t> out(btp::kEspNowMaxFrameSize);
    std::size_t n = 0U;
    if (btp::encode(frame, btp::kEspNowTransport, out.data(), out.size(), &n) != btp::Error::Ok) {
        return std::vector<std::uint8_t>();
    }
    out.resize(n);
    return out;
}

void test_strict_order_is_class_then_fifo() {
    btp::StaticTxQueue<8, 16> queue;
    CHECK(queue.valid());
    CHECK(push_tag(queue, 1, PriorityClass::Telemetry) == TxPush::Queued);
    CHECK(push_tag(queue, 2, PriorityClass::Telemetry) == TxPush::Queued);
    CHECK(push_tag(queue, 3, PriorityClass::Command) == TxPush::Queued);
    CHECK(push_tag(queue, 4, PriorityClass::Session) == TxPush::Queued);
    CHECK(push_tag(queue, 5, PriorityClass::Terminal) == TxPush::Queued);
    CHECK(queue.size() == 5U);
    CHECK(pop_tag(queue) == 4);
    CHECK(pop_tag(queue) == 3);
    CHECK(pop_tag(queue) == 5);
    CHECK(pop_tag(queue) == 1);
    CHECK(pop_tag(queue) == 2);
    CHECK(queue.empty());
    CHECK(queue.stats().sent[btp::tx_class_index(PriorityClass::Telemetry)] == 2U);
}

void test_frames_are_classified_from_their_header() {
    btp::StaticTxQueue<8> queue;
    const std::vector<std::uint8_t> telemetry = encoded_frame(btp::MessageType::Telemetry, 0x0101U);
    const std::vector<std::uint8_t> result =
        encoded_frame(btp::MessageType::Command, btp::object_id::kCommandResult);
    const std::vector<std::uint8_t> terminal =
        encoded_frame(btp::MessageType::Terminal, btp::object_id::kTerminalOut);
    CHECK(!telemetry.empty() && !result.empty() && !terminal.empty());

    CHECK(queue.push(telemetry.data(), telemetry.size()) == TxPush::Queued);
    CHECK(queue.push(terminal.data(), terminal.size()) == TxPush::Queued);
    CHECK(queue.push(result.data(), result.size()) == TxPush::Queued);
    CHECK(queue.size(PriorityClass::Session) == 1U);  // COMMAND_RESULT is class 1
    CHECK(queue.size(PriorityClass::Terminal) == 1U);
    CHECK(queue.size(PriorityClass::Telemetry) == 1U);

    const std::uint8_t garbage[8] = {0};
    CHECK(queue.push(garbage, sizeof(garbage)) == TxPush::Invalid);

    const std::uint8_t* frame = nullptr;
    std::size_t size = 0U;
    PriorityClass priority = PriorityClass::Telemetry;
    CHECK(queue.front(&frame, &size, &priority));
    CHECK(priority == PriorityClass::Session);
    CHECK(size == result.size() && std::memcmp(frame, result.data(), size) == 0);
}

// Full pool: an incoming frame evicts the oldest of the LOWEST class below
// it; with nothing lower queued, the incoming frame is the one dropped.
void test_full_pool_sheds_lowest_priority_first() {
    btp::StaticTxQueue<3, 4> queue;
    CHECK(push_tag(queue, 1, PriorityClass::Telemetry) == TxPush::Queued);
    CHECK(push_tag(queue, 2, PriorityClass::Bulk) == TxPush::Queued);
    CHECK(push_tag(queue, 3, PriorityClass::Telemetry) == TxPush::Queued);

    CHECK(push_tag(queue, 4, PriorityClass::Command) == TxPush::QueuedEvicting);
    CHECK(queue.size(PriorityClass::Telemetry) == 1U);  // tag 1 evicted, tag 3 kept
    CHECK(queue.stats().dropped[btp::tx_class_index(PriorityClass::Telemetry)] == 1U);

    CHECK(push_tag(queue, 5, PriorityClass::Terminal) == TxPush::QueuedEvicting);
    CHECK(queue.size(PriorityClass::Telemetry) == 0U);

    // Now: Bulk(2), Command(4), Terminal(5). A telemetry frame has nothing
    // below it to evict -- it is dropped itself.
    CHECK(push_tag(queue, 6, PriorityClass::Telemetry) == TxPush::Dropped);
    // A session frame evicts the lowest present: Bulk.
    CHECK(push_tag(queue, 7, PriorityClass::Session) == TxPush::QueuedEvicting);
    CHECK(queue.size(PriorityClass::Bulk) == 0U);

    CHECK(pop_tag(queue) == 7);
    CHECK(pop_tag(queue) == 4);
    CHECK(pop_tag(queue) == 5);
    CHECK(queue.empty());
}

// The frame front() handed out is mid-transmission: eviction must not pull it.
void test_front_frame_is_never_evicted() {
    btp::StaticTxQueue<2, 4> queue;
    push_tag(queue, 1, PriorityClass::Telemetry);
    push_tag(queue, 2, PriorityClass::Telemetry);
    const std::uint8_t* frame = nullptr;
    std::size_t size = 0U;
    CHECK(queue.front(&frame, &size) && frame[0] == 1);

    CHECK(push_tag(queue, 3, PriorityClass::Command) == TxPush::QueuedEvicting);  // evicts 2
    CHECK(queue.front(&frame, &size) && frame[0] == 1);  // still the same frame
    queue.pop();
    CHECK(pop_tag(queue) == 3);

    // With only the in-flight frame below it, nothing can be evicted.
    btp::StaticTxQueue<1, 4> single;
    push_tag(single, 9, PriorityClass::Telemetry);
    CHECK(single.front(&frame, &size));
    CHECK(push_tag(single, 10, PriorityClass::Session) == TxPush::Dropped);
    CHECK(pop_tag(single) == 9);
}

// Weighted: a busy high class still leaves telemetry its share.
void test_weighted_drain_does_not_starve_telemetry() {
    btp::StaticTxQueue<32, 4> queue(btp::TxDrainPolicy::Weighted);
    for (std::uint8_t i = 0; i < 20U; ++i) push_tag(queue, 100U + i, PriorityClass::Session);
    for (std::uint8_t i = 0; i < 3U; ++i) push_tag(queue, i, PriorityClass::Telemetry);

    std::vector<std::uint8_t> order;
    while (!queue.empty()) order.push_back(pop_tag(queue));
    CHECK(order.size() == 23U);
    // Session's share is 8 per round, telemetry's 1.
    CHECK(order[8] == 0);
    CHECK(order[17] == 1);
    for (std::size_t i = 0; i < 8U; ++i) CHECK(order[i] >= 100U);

    // Strict, the same load sends all 20 session frames first.
    queue.set_policy(btp::TxDrainPolicy::Strict);
    for (std::uint8_t i = 0; i < 20U; ++i) push_tag(queue, 100U + i, PriorityClass::Session);
    push_tag(queue, 0, PriorityClass::Telemetry);
    for (std::size_t i = 0; i < 20U; ++i) CHECK(pop_tag(queue) >= 100U);
    CHECK(pop_tag(queue) == 0);
}

struct Link {
    std::vector<std::vector<std::uint8_t> > frames;
    std::size_t accept = 1000U;
    static bool send(void* ctx, const std::uint8_t* frame, std::size_t size) {
        Link* link = static_cast<Link*>(ctx);
        if (link->frames.size() >= link->accept) return false;
        link->frames.emplace_back(frame, frame + size);
        return true;
    }
};

void test_drain_stops_on_a_failed_send_and_keeps_the_frame() {
    btp::StaticTxQueue<8, 4> queue;
    for (std::uint8_t i = 1; i <= 5U; ++i) push_tag(queue, i, PriorityClass::Bulk);
    Link link;
    link.accept = 2U;
    CHECK(queue.drain(&Link::send, &link) == 2U);
    CHECK(queue.size() == 3U);
    link.accept = 1000U;
    CHECK(queue.drain(&Link::send, &link, /*max_frames=*/2U) == 2U);
    CHECK(queue.drain(&Link::send, &link) == 1U);
    CHECK(link.frames.size() == 5U);
    for (std::uint8_t i = 0; i < 5U; ++i) CHECK(link.frames[i][0] == i + 1U);
}

void test_too_large_and_invalid() {
    btp::StaticTxQueue<2, 4> queue;
    const std::uint8_t big[5] = {0};
    CHECK(queue.push(big, sizeof(big), PriorityClass::Session) == TxPush::TooLarge);
    CHECK(queue.push(nullptr, 1U, PriorityClass::Session) == TxPush::Invalid);
    CHECK(queue.push(big, 0U, PriorityClass::Session) == TxPush::Invalid);
    CHECK(queue.empty());
}

// A Node whose send() enqueues instead of transmitting: a large message on a
// TCP node lands as several <= 250-octet frames, drains in order, and
// reassembles on the far end.
class QueueConfig : public btp::NodeConfig {
public:
    explicit QueueConfig(btp::TxQueue& queue) : queue_(queue) {}
    bool send(const std::uint8_t* frame, std::size_t n) override {
        return queue_.push(frame, n) != TxPush::Dropped;
    }

private:
    btp::TxQueue& queue_;
};

class SinkConfig : public btp::NodeConfig {
public:
    bool send(const std::uint8_t*, std::size_t) override { return true; }
};

struct NodeFeed {
    btp::Node* node;
    int completes;
    static bool send(void* ctx, const std::uint8_t* frame, std::size_t size) {
        NodeFeed* feed = static_cast<NodeFeed*>(ctx);
        btp::ReceivedMessage msg{};
        if (feed->node->receive(frame, size, 0U, &msg) == btp::NodeRx::Complete) ++feed->completes;
        return true;
    }
};

void test_node_sends_through_the_queue() {
    btp::StaticTxQueue<16> queue;
    QueueConfig tx_cfg(queue);
    tx_cfg.source_id = 0x00CAFE01U;
    tx_cfg.boot_id = 0x0000B001U;
    tx_cfg.transport = btp::kTcpTransport;
    btp::StaticNode<4, 2048, 2048> sender(tx_cfg);
    CHECK(sender.begin());

    SinkConfig rx_cfg;
    rx_cfg.source_id = 0x00B0B0FEU;
    rx_cfg.boot_id = 0x0000C0DEU;
    rx_cfg.transport = btp::kTcpTransport;
    btp::StaticNode<4, 2048, 2048> receiver(rx_cfg);
    CHECK(receiver.begin());

    std::vector<std::uint8_t> payload(1500);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i);
    CHECK(sender.send(btp::MessageType::Control, btp::object_id::kManifestData, payload.data(),
                      payload.size(), 1ULL));
    CHECK(queue.size(PriorityClass::Bulk) > 1U);

    NodeFeed feed{&receiver, 0};
    CHECK(queue.drain(&NodeFeed::send, &feed) > 1U);
    CHECK(feed.completes == 1);
    CHECK(queue.empty());
}

}  // namespace

int main() {
    test_strict_order_is_class_then_fifo();
    test_frames_are_classified_from_their_header();
    test_full_pool_sheds_lowest_priority_first();
    test_front_frame_is_never_evicted();
    test_weighted_drain_does_not_starve_telemetry();
    test_drain_stops_on_a_failed_send_and_keeps_the_frame();
    test_too_large_and_invalid();
    test_node_sends_through_the_queue();
    if (failures != 0) {
        std::cerr << "test_txqueue: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "test_txqueue: all checks passed\n";
    return 0;
}
