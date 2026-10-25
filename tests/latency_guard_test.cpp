#include <cito/data_packet.hpp>
#include <cito/detail/demand_router.hpp>
#include <cito/discovery.hpp>

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    const cito::InterestKey key{1, 2, 3};

    // Duplicate local subscribers must not churn the outward host summary.
    cito::HostInterests host;
    assert(host.add(key));
    const auto first_version = host.version();

    for (int i = 0; i < 100; ++i) {
        assert(!host.add(key));
    }
    assert(host.version() == first_version);

    for (int i = 0; i < 100; ++i) {
        assert(!host.remove(key));
    }
    assert(host.version() == first_version);

    assert(host.remove(key));
    assert(host.version() == first_version + 1);

    // Lease refresh must stay bounded: one live lease, one scheduled expiry.
    cito::InterestIndex index;
    cito::InterestLeaseTable leases(index);
    const cito::InterestAdvertisement advertisement{
        cito::InterestOp::Add,
        48000,
        100,
        7,
        key};

    assert(leases.observe(advertisement, 0));
    for (std::uint64_t i = 1; i <= 10'000; ++i) {
        assert(!leases.observe(advertisement, i));
        assert(leases.size() == 1);
        assert(leases.scheduled_expiry_count() == 1);
    }

    assert(leases.expire(10'099) == 0);
    assert(leases.expire(10'100) == 1);

    // No demand means no encode/send. Fan-out encodes once.
    cito::detail::DemandRouter router(index);
    int encode_calls = 0;
    int send_calls = 0;

    auto encode = [&] {
        ++encode_calls;
        return std::vector<std::uint8_t>{1, 2, 3};
    };
    auto send = [&](cito::DestinationId, const auto&) {
        ++send_calls;
    };

    assert(router.deliver(key, encode, send) == 0);
    assert(encode_calls == 0);
    assert(send_calls == 0);

    index.add(key, 11);
    index.add(key, 12);
    assert(router.deliver(key, encode, send) == 2);
    assert(encode_calls == 1);
    assert(send_calls == 2);

    // Packet view must alias the received buffer instead of copying payload.
    const cito::DataPacket packet{
        key,
        {9, 8, 7, 6}};
    const auto bytes = cito::encode_data_packet(packet);
    const auto view = cito::decode_data_packet_view(bytes);

    assert(view.key == key);
    assert(view.payload.size() == 4);
    assert(view.payload.data() == bytes.data() + 32);
    assert(view.payload[0] == 9);

    return 0;
}
