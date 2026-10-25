#include <cito/discovery.hpp>

#include <cassert>

int main() {
    cito::InterestIndex index;
    cito::InterestLeaseTable leases(index);

    const cito::InterestAdvertisement position{
        cito::InterestOp::Add,
        48001,
        1000,
        11,
        {7, 17, 100}};

    const cito::InterestAdvertisement battery{
        cito::InterestOp::Add,
        48002,
        1000,
        12,
        {7, 17, 200}};

    leases.observe(
        cito::decode_interest(cito::encode_interest(position)),
        0);
    leases.observe(
        cito::decode_interest(cito::encode_interest(battery)),
        0);

    assert(index.lookup({7, 17, 100}).size() == 1);
    assert(index.lookup({7, 17, 100}).contains(11));
    assert(index.lookup({7, 17, 200}).contains(12));

    return 0;
}
