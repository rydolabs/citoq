#include <cito/demand_summary.hpp>

#include <array>
#include <cassert>
#include <stdexcept>

int main() {
    cito::RemoteSummaryTracker tracker;

    const cito::DemandSummaryStamp v2{
        100,
        2,
        2};

    assert(tracker.needs_snapshot(7, v2));
    assert(tracker.mark_applied(7, v2));
    assert(!tracker.needs_snapshot(7, v2));

    const cito::DemandSummaryStamp stale{
        100,
        1,
        2};

    assert(!tracker.needs_snapshot(7, stale));
    assert(!tracker.mark_applied(7, stale));

    const cito::DemandSummaryStamp v3{
        100,
        3,
        1};
    assert(tracker.needs_snapshot(7, v3));

    const cito::DemandSummaryStamp restarted{
        101,
        0,
        0};

    assert(tracker.needs_snapshot(7, restarted));
    assert(tracker.mark_applied(7, restarted));
    assert(!tracker.needs_snapshot(7, restarted));

    // Delayed advertisements from the retired incarnation must not
    // cause a snapshot-pull storm after restart.
    assert(!tracker.needs_snapshot(7, v3));

    const cito::InterestKey position{
        1,
        10,
        100};
    const cito::InterestKey battery{
        1,
        10,
        200};

    cito::RemoteDemandIndex index;

    std::array keys1{
        position,
        battery};
    assert(index.apply_snapshot(
        1,
        {10, 1, 2},
        keys1));

    std::array keys2{
        position};
    assert(index.apply_snapshot(
        2,
        {20, 1, 1},
        keys2));

    assert(index.lookup_hosts(position).size() == 2);
    assert(index.lookup_hosts(battery).size() == 1);

    // Host 1 drops Position while retaining Battery.
    std::array keys1v2{
        battery};
    assert(index.apply_snapshot(
        1,
        {10, 2, 1},
        keys1v2));

    assert(index.lookup_hosts(position).size() == 1);
    assert(index.lookup_hosts(position).contains(2));
    assert(index.lookup_hosts(battery).contains(1));

    // Same or stale snapshots do no index work.
    assert(!index.apply_snapshot(
        1,
        {10, 2, 1},
        keys1v2));
    assert(!index.apply_snapshot(
        1,
        {10, 1, 1},
        keys1v2));

    assert(index.remove_host(2));
    assert(index.lookup_hosts(position).empty());

    bool mismatch_failed = false;
    try {
        (void)index.apply_snapshot(
            3,
            {30, 1, 2},
            keys2);
    } catch (const std::invalid_argument&) {
        mismatch_failed = true;
    }
    assert(mismatch_failed);

    return 0;
}
