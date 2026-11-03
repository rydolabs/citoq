#include <citoq/detail/reliable_quic_delivery.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <stdexcept>

using namespace citoq;
using namespace citoq::detail::reliable;

static QuicDeliveryTarget target(
    DestinationId destination,
    InterestKey key = {11, 22, 33},
    std::uint64_t stream = 800,
    std::uint64_t sequence = 1) {
    return {destination, {key, stream, sequence}};
}

int main() {
    constexpr std::array<std::uint8_t, 2> frame{0x10, 0x20};

    // No background sends without a connected destination. A disconnected
    // peer only replays its own unacknowledged bytes after reconnect.
    {
        QuicDeliveryLedger ledger(4, 8, 10, 40);
        auto a = target(101);
        assert(ledger.track(a, frame, 0));
        assert(!ledger.track(a, frame, 0));
        assert(ledger.due(0, 10).empty());
        ledger.connected(101, 0);
        auto first = ledger.due(0, 1);
        assert(first.size() == 1 && first[0].attempt == 1);
        assert(first[0].target == a);
        assert(first[0].payload.size() == 2 &&
               first[0].payload[1] == 0x20);
        assert(ledger.due(9, 10).empty());
        assert(ledger.due(10, 1).size() == 1);
        assert(ledger.due(29, 10).empty()); // backoff 10 then 20
        ledger.disconnected(101);
        assert(ledger.due(100, 10).empty());
        ledger.connected(101, 100);
        auto replay = ledger.due(100, 10);
        assert(replay.size() == 1 && replay[0].attempt == 3);
        assert(ledger.acknowledge(a));
        assert(!ledger.acknowledge(a)); // duplicate ACK is harmless
        assert(ledger.pending_count() == 0 && ledger.pending_bytes() == 0);
        assert(ledger.due(200, 10).empty());
    }

    // A lost application ACK can cause replay; the existing bounded
    // ReceiverWindow must suppress a second application callback.
    {
        QuicDeliveryLedger ledger(2, 8, 10, 40);
        auto a = target(201);
        ledger.connected(201, 0);
        assert(ledger.track(a, frame, 0));
        auto first = ledger.due(0, 10);
        assert(first.size() == 1);
        ReceiverWindow receiver(64);
        int callbacks = 0;
        if (receiver.on_data(1) != ReceiveResult::Duplicate) ++callbacks;
        // Simulate lost ACK and a retransmitted frame.
        auto replay = ledger.due(10, 10);
        assert(replay.size() == 1);
        if (receiver.on_data(1) != ReceiveResult::Duplicate) ++callbacks;
        assert(callbacks == 1);
        assert(ledger.acknowledge(a));
        assert(ledger.due(100, 10).empty());
    }

    // ACKs and replay are scoped to destination, DemandKey, and stream.
    // Never retransmit a message to a peer that already acknowledged it.
    {
        QuicDeliveryLedger ledger(4, 8, 10, 40);
        auto a = target(101);
        auto b = target(102);
        assert(ledger.track(a, frame, 0));
        assert(ledger.track(b, frame, 0));
        ledger.connected(101, 0);
        ledger.connected(102, 0);
        assert(ledger.due(0, 10).size() == 2);
        assert(!ledger.acknowledge(target(101, {11, 22, 34})));
        assert(!ledger.acknowledge(target(101, {11, 22, 33}, 801)));
        assert(!ledger.acknowledge(target(101, {11, 22, 33}, 800, 2)));
        assert(!ledger.acknowledge(target(999)));
        assert(ledger.acknowledge(a));
        ledger.disconnected(102);
        assert(ledger.due(100, 10).empty());
        ledger.connected(102, 100);
        const auto due = ledger.due(100, 10);
        assert(due.size() == 1 && due[0].target == b);
        assert(ledger.pending_count() == 1);
        assert(ledger.cancel_destination(102) == 1);
        assert(ledger.pending_count() == 0);
    }

    // Pending storage is strictly bounded. An exhausted ledger returns
    // backpressure rather than evicting unacknowledged data.
    {
        QuicDeliveryLedger ledger(2, 3);
        constexpr std::array<std::uint8_t, 1> small{0x33};
        assert(ledger.track(target(101), frame, 0));
        assert(ledger.track(target(102), small, 0));
        assert(!ledger.track(target(103), small, 0));
        assert(ledger.pending_count() == 2 && ledger.pending_bytes() == 3);
        assert(ledger.acknowledge(target(101)));
        assert(ledger.track(target(103), frame, 0));
        assert(ledger.pending_bytes() == 3);
        assert(ledger.expire(4, 5).empty());
        const auto expired = ledger.expire(5, 5);
        assert(expired.size() == 2);
        assert(ledger.pending_count() == 0 && ledger.pending_bytes() == 0);
        assert(!ledger.acknowledge(target(103))); // late receipt after expiry
    }

    // Reject invalid identity, zero bounds, and silent zero-TTL expiry.
    {
        bool bad = false;
        try { QuicDeliveryLedger ledger(0, 8); }
        catch (const std::invalid_argument&) { bad = true; }
        assert(bad);
        QuicDeliveryLedger ledger(1, 2);
        bad = false;
        try { (void)ledger.track(target(0), frame, 0); }
        catch (const std::invalid_argument&) { bad = true; }
        assert(bad);
        bad = false;
        try { (void)ledger.expire(10, 0); }
        catch (const std::invalid_argument&) { bad = true; }
        assert(bad);
    }
    return 0;
}
