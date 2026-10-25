#include <citoq/demand_control.hpp>
#include <citoq/demand_summary.hpp>
#include <citoq/detail/demand_router.hpp>

#include <array>
#include <cassert>
#include <vector>

int main() {
    const citoq::InterestKey position{
        1,
        17,
        100};
    const citoq::InterestKey battery{
        1,
        17,
        200};

    // Two remote coordinators advertise cheap version stamps.
    citoq::RemoteSummaryTracker tracker;
    citoq::RemoteDemandIndex hosts;

    const citoq::DemandSummaryAnnouncement pos_announce{
        10,
        47010,
        600,
        {1001, 1, 1}};
    const citoq::DemandSummaryAnnouncement bat_announce{
        20,
        47020,
        600,
        {2001, 1, 1}};

    assert(tracker.needs_snapshot(
        pos_announce.coordinator,
        pos_announce.stamp));
    assert(tracker.needs_snapshot(
        bat_announce.coordinator,
        bat_announce.stamp));

    // Pull exact DemandKeys only because the cached stamps differ.
    std::array pos_keys{position};
    std::array bat_keys{battery};

    const auto pos_batch =
        citoq::decode_snapshot_batch(
            citoq::encode_snapshot_batch(
                citoq::make_snapshot_batch(
                    10,
                    pos_announce.stamp,
                    pos_keys,
                    0)));
    const auto bat_batch =
        citoq::decode_snapshot_batch(
            citoq::encode_snapshot_batch(
                citoq::make_snapshot_batch(
                    20,
                    bat_announce.stamp,
                    bat_keys,
                    0)));

    assert(hosts.apply_snapshot(
        10,
        pos_batch.stamp,
        pos_batch.keys));
    assert(hosts.apply_snapshot(
        20,
        bat_batch.stamp,
        bat_batch.keys));

    tracker.mark_applied(
        10,
        pos_batch.stamp);
    tracker.mark_applied(
        20,
        bat_batch.stamp);

    // GIS-like candidate lookup: only the Position host survives.
    const auto& candidates =
        hosts.lookup_hosts(position);
    assert(candidates.size() == 1);
    assert(candidates.contains(10));
    assert(!candidates.contains(20));

    // Only the matching host returns detailed direct endpoints.
    const citoq::RouteBatch direct_routes{
        10,
        pos_announce.stamp.incarnation,
        position,
        {{101, 48001}}};
    const auto routes =
        citoq::decode_route_batch(
            citoq::encode_route_batch(
                direct_routes));

    citoq::InterestIndex direct;
    for (const auto& endpoint :
         routes.endpoints) {
        direct.add(
            routes.key,
            endpoint.destination);
    }

    citoq::detail::DemandRouter router(
        direct);

    int encodes = 0;
    int sends = 0;

    assert(router.deliver(
        position,
        [&] {
            ++encodes;
            return std::vector<std::uint8_t>{
                1, 2, 3};
        },
        [&](citoq::DestinationId id, const auto&) {
            ++sends;
            assert(id == 101);
        }) == 1);

    assert(encodes == 1);
    assert(sends == 1);

    return 0;
}
