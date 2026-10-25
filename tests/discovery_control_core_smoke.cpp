#include <cito/detail/discovery_control_core.hpp>

#include <array>
#include <cassert>

int main() {
    const cito::InterestKey position{
        7, 17, 100};
    const cito::InterestKey battery{
        7, 17, 200};

    cito::detail::HostRuntimeCore
        position_host;
    position_host.register_app(
        1,
        [](auto, auto, auto) {
            return std::size_t{1};
        });
    position_host.add_interest(
        1,
        position);

    cito::detail::HostRuntimeCore
        battery_host;
    battery_host.register_app(
        2,
        [](auto, auto, auto) {
            return std::size_t{1};
        });
    battery_host.add_interest(
        2,
        battery);

    cito::detail::HostRuntimeCore
        publisher;

    cito::detail::DiscoveryControlCore
        pos_control(
            position_host,
            11,
            11001);
    cito::detail::DiscoveryControlCore
        bat_control(
            battery_host,
            12,
            12001);
    cito::detail::DiscoveryControlCore
        pub_control(
            publisher,
            20,
            20001);

    auto pos_actions =
        pub_control.on_summary(
            pos_control.announcement(
                47111,
                500),
            1000);

    auto bat_actions =
        pub_control.on_summary(
            bat_control.announcement(
                47112,
                500),
            1000);

    assert(pos_actions.size() == 1);
    assert(bat_actions.size() == 1);

    auto pos_batch =
        pos_control.on_snapshot_request(
            cito::decode_snapshot_request(
                pos_actions[0].bytes));

    auto bat_batch =
        bat_control.on_snapshot_request(
            cito::decode_snapshot_request(
                bat_actions[0].bytes));

    assert(
        pub_control.on_snapshot_batch(
            cito::decode_snapshot_batch(
                pos_batch.bytes))
            .empty());

    assert(
        pub_control.on_snapshot_batch(
            cito::decode_snapshot_batch(
                bat_batch.bytes))
            .empty());

    const auto route_actions =
        pub_control.ensure_routes(
            position);

    // GIS-like exact candidate filtering: only the Position host is
    // contacted for route detail. Battery host is never on the data path.
    assert(route_actions.size() == 1);
    assert(
        route_actions[0].coordinator ==
        11);

    const auto route =
        cito::decode_route_request(
            route_actions[0].bytes);

    const std::array<
        cito::RouteEndpoint,
        1> endpoint{{
            {111, 48111}}};

    const auto response =
        pos_control.on_route_request(
            route,
            endpoint);

    assert(
        pub_control.on_route_batch(
            cito::decode_route_batch(
                response.bytes)));

    assert(
        publisher.remote_destinations(
            position).size() == 1);
    assert(
        publisher.remote_destinations(
            battery).empty());

    // Lease expiry removes both candidate and direct route without scans.
    assert(pub_control.expire(1499) == 0);
    assert(pub_control.expire(1500) == 2);
    assert(
        publisher.remote_destinations(
            position).empty());
    assert(
        publisher.candidate_hosts(
            position).empty());

    return 0;
}
