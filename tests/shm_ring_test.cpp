#include <cito/detail/shm_ring.hpp>

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include <unistd.h>

int main() {
    const std::string name =
        "/cito-shm-unit-" +
        std::to_string(::getpid());

    cito::detail::ShmRingWriter writer(
        name,
        4,
        64,
        77);

    cito::detail::ShmRingReader first(
        name);
    cito::detail::ShmRingReader slow(
        name);

    assert(
        first.generation() == 77);
    assert(
        writer.slot_count() == 4);
    assert(
        writer.slot_size() == 64);

    writer.publish("one");
    writer.publish("two");

    std::vector<std::uint8_t> out;
    std::uint64_t dropped = 0;

    assert(
        first.try_read_next(
            out,
            dropped));
    assert(
        std::string(
            out.begin(),
            out.end()) ==
        "one");

    assert(
        first.try_read_next(
            out,
            dropped));
    assert(
        std::string(
            out.begin(),
            out.end()) ==
        "two");
    assert(dropped == 0);

    // The writer never waits for the slow reader. Once the fixed ring is
    // overwritten, that reader reports dropped sequences and continues
    // from the earliest still-available slot.
    for (int i = 0;
         i < 8;
         ++i) {
        writer.publish(
            "x" +
            std::to_string(i));
    }

    std::uint64_t slow_dropped = 0;

    assert(
        slow.try_read_next(
            out,
            slow_dropped));

    // 10 total writes with 4 slots: sequences 1..6 were overwritten.
    assert(slow_dropped == 6);
    assert(
        std::string(
            out.begin(),
            out.end()) ==
        "x4");

    bool too_large = false;
    try {
        const std::string payload(
            65,
            'a');
        writer.publish(payload);
    } catch (
        const std::length_error&) {
        too_large = true;
    }
    assert(too_large);

    return 0;
}
