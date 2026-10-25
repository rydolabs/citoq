#include <cito/detail/demand_router.hpp>
#include <cassert>
#include <vector>

int main() {
    cito::InterestIndex index;
    const cito::InterestKey position{7, 17, 100};
    const cito::InterestKey battery{7, 17, 200};

    index.add(position, 2);
    index.add(battery, 3);

    cito::detail::DemandRouter router(index);
    std::vector<cito::DestinationId> reached;
    int encodes = 0;

    const auto delivered = router.deliver(
        position,
        [&] { ++encodes; return std::vector<unsigned char>{9, 8, 7}; },
        [&](cito::DestinationId id, const auto&) { reached.push_back(id); });

    assert(delivered == 1);
    assert(encodes == 1);
    assert(reached.size() == 1 && reached[0] == 2);
}
