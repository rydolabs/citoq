#include <cito/demand_summary.hpp>
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>
int main() {
    constexpr std::uint64_t hosts = 200;
    const cito::InterestKey target{1, 17, 9999};
    cito::RemoteDemandIndex index;
    for (std::uint64_t host = 1; host <= hosts; ++host) {
        std::vector<cito::InterestKey> keys;
        keys.reserve(5);
        for (std::uint64_t i = 0; i < 4; ++i) {
            keys.push_back({1, 1 + ((host * 13 + i * 7) % 64), 1 + ((host * 17 + i * 11) % 128)});
        }
        if (host % 40 == 0) keys.push_back(target);
        const cito::DemandSummaryStamp stamp{host * 1000 + 1, 1, static_cast<std::uint32_t>(keys.size())};
        assert(index.apply_snapshot(host, stamp, keys));
    }
    assert(index.host_count() == 200);
    const auto& candidates = index.lookup_hosts(target);
    assert(candidates.size() == 5);
    for (const auto id : std::array<std::uint64_t, 5>{40,80,120,160,200}) assert(candidates.contains(id));
    const std::vector<cito::InterestKey> replacement{{1,99,12345}};
    assert(index.apply_snapshot(40, {40001,2,1}, replacement));
    assert(index.host_count() == 200);
    assert(index.lookup_hosts(target).size() == 4);
    assert(!index.lookup_hosts(target).contains(40));
    assert(index.remove_host(80));
    assert(index.host_count() == 199);
    assert(index.lookup_hosts(target).size() == 3);
    return 0;
}
