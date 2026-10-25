#include <citoq/discovery.hpp>

#include <cassert>
#include <cstdint>

int main() {
    citoq::InterestAdvertisement add;
    add.op = citoq::InterestOp::Add;
    add.data_port = 48001;
    add.lease_ms = 500;
    add.destination = 7;
    add.key = {1, 2, 3};

    const auto bytes = citoq::encode_interest(add);
    const auto decoded = citoq::decode_interest(bytes);

    assert(decoded.op == add.op);
    assert(decoded.data_port == add.data_port);
    assert(decoded.lease_ms == add.lease_ms);
    assert(decoded.destination == add.destination);
    assert(decoded.key == add.key);

    citoq::InterestIndex index;
    citoq::InterestLeaseTable leases(index);

    assert(leases.observe(add, 1000));
    assert(index.lookup(add.key).contains(7));
    assert(leases.size() == 1);
    assert(leases.scheduled_expiry_count() == 1);

    // Repeated refreshes replace the old expiry instead of growing a stale queue.
    for (std::uint64_t now = 1100; now <= 1500; now += 100) {
        assert(!leases.observe(add, now));
        assert(leases.size() == 1);
        assert(leases.scheduled_expiry_count() == 1);
    }

    assert(leases.expire(1999) == 0);
    assert(leases.expire(2000) == 1);
    assert(index.lookup(add.key).empty());

    leases.observe(add, 2000);
    auto remove = add;
    remove.op = citoq::InterestOp::Remove;
    remove.lease_ms = 0;
    assert(leases.observe(remove, 2100));
    assert(index.lookup(add.key).empty());
    assert(leases.scheduled_expiry_count() == 0);

    auto truncated = bytes;
    truncated.pop_back();
    bool failed = false;
    try {
        (void)citoq::decode_interest(truncated);
    } catch (const std::runtime_error&) {
        failed = true;
    }
    assert(failed);

    return 0;
}
