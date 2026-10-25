#include <cito/interest.hpp>

#include <cassert>
#include <cstdint>

int main() {
    const cito::InterestKey position{1, 17, 100};
    const cito::InterestKey battery{1, 17, 200};
    const cito::InterestKey other_scope{2, 17, 100};

    cito::HostInterests host;
    assert(host.version() == 0);

    assert(host.add(position));
    assert(host.version() == 1);

    assert(!host.add(position));
    assert(host.version() == 1);
    assert(host.unique_interest_count() == 1);

    assert(host.add(battery));
    assert(host.version() == 2);

    const auto snapshot = host.snapshot();
    assert(snapshot.size() == 2);
    assert(snapshot[0] == position);
    assert(snapshot[1] == battery);

    assert(!host.remove(position));
    assert(host.version() == 2);
    assert(host.contains(position));

    assert(host.remove(position));
    assert(host.version() == 3);
    assert(!host.contains(position));

    cito::RemoteSummaryTracker summaries;
    const cito::DemandSummaryStamp first_stamp{
        1001,
        host.version(),
        host.unique_interest_count()};

    assert(summaries.needs_snapshot(9, first_stamp));
    summaries.mark_applied(9, first_stamp);
    assert(!summaries.needs_snapshot(9, first_stamp));

    auto changed_stamp = first_stamp;
    ++changed_stamp.version;
    assert(summaries.needs_snapshot(9, changed_stamp));

    const cito::DemandSummaryStamp restarted{
        1002,
        0,
        0};
    assert(summaries.needs_snapshot(9, restarted));
    summaries.mark_applied(9, restarted);
    assert(!summaries.needs_snapshot(9, restarted));

    cito::InterestIndex index;
    assert(index.add(position, 10));
    assert(!index.add(position, 10));
    assert(index.add(position, 11));
    assert(index.add(battery, 12));
    assert(index.add(other_scope, 13));

    const auto& pos = index.lookup(position);
    assert(pos.size() == 2);
    assert(pos.contains(10));
    assert(pos.contains(11));
    assert(!pos.contains(12));

    assert(index.lookup(cito::InterestKey{1, 18, 100}).empty());
    assert(index.lookup(other_scope).contains(13));
    assert(index.edge_count() == 4);

    assert(index.remove(position, 10));
    assert(index.lookup(position).size() == 1);
    assert(index.remove(position, 11));
    assert(index.lookup(position).empty());
    assert(index.key_count() == 2);

    return 0;
}
