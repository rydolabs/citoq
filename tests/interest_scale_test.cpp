#include <cito/interest.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>

int main() {
    constexpr std::uint64_t hosts = 10'000;
    constexpr std::uint64_t interests_per_host = 4;
    constexpr std::uint64_t type_count = 64;
    constexpr std::uint64_t resource_count = 256;

    cito::InterestIndex index;
    for (std::uint64_t host = 0; host < hosts; ++host) {
        for (std::uint64_t j = 0; j < interests_per_host; ++j) {
            const cito::InterestKey key{
                1,
                1 + ((host * 7 + j * 13) % resource_count),
                1 + ((host * 11 + j * 17) % type_count)
            };
            index.add(key, host + 1);
        }
    }

    assert(index.edge_count() <= hosts * interests_per_host);
    assert(index.key_count() <= type_count * resource_count);

    std::size_t observed = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < 100'000; ++i) {
        const cito::InterestKey key{1, 1 + (i % resource_count), 1 + (i % type_count)};
        observed += index.lookup(key).size();
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();

    std::cout << "hosts=" << hosts
              << " keys=" << index.key_count()
              << " edges=" << index.edge_count()
              << " lookups=100000"
              << " observed=" << observed
              << " elapsed_us=" << elapsed << '\n';

    const auto& one = index.lookup(cito::InterestKey{1, 1, 1});
    assert(one.size() <= hosts);
    return 0;
}
