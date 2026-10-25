#include <cito/detail/reliability.hpp>

#include <array>
#include <cassert>
#include <cstdint>

int main() {
    using namespace cito::detail::reliable;

    SenderHistory history(
        4,
        16);

    const std::array<std::uint8_t, 1>
        byte{7};

    for (int i = 0;
         i < 6;
         ++i) {
        history.publish(byte);
    }

    assert(
        history.sample_capacity() ==
        4);
    assert(
        history.first_available() ==
        3);
    assert(
        history.last_published() ==
        6);
    assert(!history.payload(2));
    assert(history.payload(3));

    // Requests 1..6: 1..2 are outside KEEP_LAST history, 3..6 can
    // retransmit. The unavailable prefix is compacted into one GAP.
    const auto recovery =
        history.plan_recovery(
            Nack{
                1,
                0x3f});

    assert(
        recovery.actions.size() ==
        5);
    assert(
        recovery.actions[0].kind ==
        RecoveryKind::Gap);
    assert(
        recovery.actions[0].first ==
        1);
    assert(
        recovery.actions[0].last ==
        2);

    for (std::size_t i = 1;
         i < recovery.actions.size();
         ++i) {
        assert(
            recovery.actions[i].kind ==
            RecoveryKind::Retransmit);
        assert(
            recovery.actions[i].first ==
            i + 2);
    }

    ReceiverWindow receiver(64);

    assert(
        receiver.on_data(1) ==
        ReceiveResult::Advanced);
    assert(
        receiver.on_data(3) ==
        ReceiveResult::
            AcceptedOutOfOrder);

    const auto missing_two =
        receiver.make_nack();

    assert(missing_two);
    assert(
        missing_two->base ==
        2);
    assert(
        missing_two->bitmap ==
        0x1);

    assert(
        receiver.on_data(2) ==
        ReceiveResult::Advanced);
    assert(
        receiver.next_expected() ==
        4);
    assert(!receiver.make_nack());

    // A tail loss is invisible from DATA alone; heartbeat makes it known.
    receiver.on_heartbeat(
        Heartbeat{
            3,
            5});

    const auto tail =
        receiver.make_nack();

    assert(tail);
    assert(
        tail->base ==
        4);
    assert(
        (tail->bitmap & 0x3) ==
        0x3);

    assert(
        receiver.on_data(4) ==
        ReceiveResult::Advanced);
    assert(
        receiver.on_data(5) ==
        ReceiveResult::Advanced);
    assert(!receiver.make_nack());

    // Far-ahead data cannot grow receiver state.
    ReceiverWindow bounded(64);
    assert(
        bounded.on_data(1000) ==
        ReceiveResult::OutOfWindow);
    assert(
        bounded.next_expected() ==
        1);

    return 0;
}
