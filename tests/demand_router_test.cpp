#include <citoq/detail/demand_router.hpp>
#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    citoq::InterestIndex index;
    citoq::detail::DemandRouter router(index);
    const citoq::InterestKey key{1, 10, 100};
    int encode_calls = 0;
    int send_calls = 0;

    auto encode = [&] {
        ++encode_calls;
        return std::vector<std::uint8_t>{1, 2, 3, 4};
    };
    auto send = [&](citoq::DestinationId, const std::vector<std::uint8_t>& payload) {
        ++send_calls;
        assert(payload.size() == 4);
    };

    assert(router.deliver(key, encode, send) == 0);
    assert(encode_calls == 0);
    assert(send_calls == 0);

    index.add(key, 11);
    index.add(key, 12);
    index.add(key, 13);

    assert(router.deliver(key, encode, send) == 3);
    assert(encode_calls == 1);
    assert(send_calls == 3);
}
