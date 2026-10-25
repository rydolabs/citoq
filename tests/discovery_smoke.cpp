#include <citoq/discovery.hpp>

#include <cassert>

int main() {
    citoq::InterestIndex index;
    citoq::InterestLeaseTable leases(index);

    const citoq::InterestAdvertisement position{
        citoq::InterestOp::Add,
        48001,
        1000,
        11,
        {7, 17, 100}};

    const citoq::InterestAdvertisement battery{
        citoq::InterestOp::Add,
        48002,
        1000,
        12,
        {7, 17, 200}};

    leases.observe(
        citoq::decode_interest(citoq::encode_interest(position)),
        0);
    leases.observe(
        citoq::decode_interest(citoq::encode_interest(battery)),
        0);

    assert(index.lookup({7, 17, 100}).size() == 1);
    assert(index.lookup({7, 17, 100}).contains(11));
    assert(index.lookup({7, 17, 200}).contains(12));

    return 0;
}
