#include <cito/coordinator_lifecycle.hpp>
#include <cito/demand_control.hpp>
#include <cito/demand_summary.hpp>
#include <cito/remote_routes.hpp>

#include <array>
#include <cassert>

int main() {
    const cito::InterestKey position{
        1,
        17,
        100};

    cito::CoordinatorLeaseTable leases;
    cito::RemoteSummaryTracker tracker;
    cito::RemoteDemandIndex summaries;
    cito::RemoteRouteIndex routes;

    const cito::DemandSummaryAnnouncement first{
        10,
        47010,
        500,
        {1001, 1, 1}};

    assert(
        leases.observe(
            first.coordinator,
            first.stamp.incarnation,
            first.lease_ms,
            1000) ==
        cito::CoordinatorLeaseObservation::Added);

    const std::array first_keys{
        position};

    assert(
        summaries.apply_snapshot(
            first.coordinator,
            first.stamp,
            first_keys));
    assert(
        tracker.mark_applied(
            first.coordinator,
            first.stamp));

    const std::array first_routes{
        cito::DestinationId{101}};

    assert(
        routes.replace(
            first.coordinator,
            position,
            first_routes));
    assert(
        routes.lookup(position).contains(
            101));

    // Same coordinator id, new incarnation: purge all old host-owned state
    // before accepting new detail.
    const cito::DemandSummaryAnnouncement restarted{
        10,
        47010,
        500,
        {1002, 0, 1}};

    assert(
        leases.observe(
            restarted.coordinator,
            restarted.stamp.incarnation,
            restarted.lease_ms,
            1200) ==
        cito::CoordinatorLeaseObservation::Restarted);

    summaries.remove_host(
        restarted.coordinator);
    tracker.forget(
        restarted.coordinator);
    routes.remove_coordinator(
        restarted.coordinator);

    assert(
        summaries.lookup_hosts(
            position).empty());
    assert(
        routes.lookup(
            position).empty());

    // A delayed route response from the retired incarnation must be ignored.
    const cito::RouteBatch stale_route{
        10,
        1001,
        position,
        {{999, 49999}}};

    assert(
        !leases.is_current(
            stale_route.coordinator,
            stale_route.incarnation));

    if (leases.is_current(
            stale_route.coordinator,
            stale_route.incarnation)) {
        const std::array stale_ids{
            stale_route.endpoints[0]
                .destination};
        routes.replace(
            stale_route.coordinator,
            stale_route.key,
            stale_ids);
    }

    assert(
        routes.lookup(
            position).empty());

    // Apply the restarted incarnation.
    assert(
        tracker.needs_snapshot(
            restarted.coordinator,
            restarted.stamp));
    assert(
        summaries.apply_snapshot(
            restarted.coordinator,
            restarted.stamp,
            first_keys));
    assert(
        tracker.mark_applied(
            restarted.coordinator,
            restarted.stamp));

    const cito::RouteBatch current_route{
        10,
        1002,
        position,
        {{202, 48002}}};

    assert(
        leases.is_current(
            current_route.coordinator,
            current_route.incarnation));

    const std::array current_ids{
        current_route.endpoints[0]
            .destination};

    routes.replace(
        current_route.coordinator,
        current_route.key,
        current_ids);

    assert(
        routes.lookup(position).contains(
            202));
    assert(
        !routes.lookup(position).contains(
            999));

    // Lease expiry removes candidate and direct-route state.
    const auto expired =
        leases.expire(1700);
    assert(expired.size() == 1);
    assert(expired[0] == 10);

    for (const auto coordinator :
         expired) {
        summaries.remove_host(
            coordinator);
        tracker.forget(
            coordinator);
        routes.remove_coordinator(
            coordinator);
    }

    assert(
        summaries.lookup_hosts(
            position).empty());
    assert(
        routes.lookup(
            position).empty());

    return 0;
}
