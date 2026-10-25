#include <cito/coordinator_lifecycle.hpp>

#include <cassert>

int main() {
    cito::CoordinatorLeaseTable leases;

    assert(
        leases.observe(
            7,
            100,
            500,
            1000) ==
        cito::CoordinatorLeaseObservation::Added);

    assert(leases.is_current(7, 100));
    assert(leases.size() == 1);
    assert(
        leases.scheduled_expiry_count() ==
        1);

    // Refresh replaces the previous expiry instead of growing timer work.
    for (std::uint64_t now = 1100;
         now <= 1500;
         now += 100) {
        assert(
            leases.observe(
                7,
                100,
                500,
                now) ==
            cito::CoordinatorLeaseObservation::Refreshed);

        assert(
            leases.scheduled_expiry_count() ==
            1);
    }

    // A new incarnation is a restart and retires the old incarnation.
    assert(
        leases.observe(
            7,
            101,
            500,
            1600) ==
        cito::CoordinatorLeaseObservation::Restarted);
    assert(leases.is_current(7, 101));

    // A delayed advertisement from the old process must not flip state back.
    assert(
        leases.observe(
            7,
            100,
            500,
            1650) ==
        cito::CoordinatorLeaseObservation::Stale);
    assert(leases.is_current(7, 101));
    assert(
        leases.scheduled_expiry_count() ==
        1);

    assert(leases.expire(2099).empty());

    const auto expired =
        leases.expire(2100);
    assert(expired.size() == 1);
    assert(expired[0] == 7);
    assert(!leases.current_incarnation(7));
    assert(leases.size() == 0);
    assert(
        leases.scheduled_expiry_count() ==
        0);

    return 0;
}
