#include <citoq/detail/discovery_control_core.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <span>
#include <vector>

namespace {

citoq::DemandSnapshotRequest snapshot_request(
    const citoq::detail::ControlAction& action) {
    assert(
        action.target ==
        citoq::detail::
            ControlTargetKind::Coordinator);
    return citoq::decode_snapshot_request(
        action.bytes);
}

citoq::RouteRequest route_request(
    const citoq::detail::ControlAction& action) {
    assert(
        action.target ==
        citoq::detail::
            ControlTargetKind::Coordinator);
    return citoq::decode_route_request(
        action.bytes);
}

} // namespace

int main() {
    const citoq::InterestKey position{
        7,
        17,
        100};

    citoq::detail::HostRuntimeCore subscriber;
    subscriber.register_app(
        10,
        [](auto, auto, auto) {
            return std::size_t{1};
        });
    subscriber.add_interest(
        10,
        position);

    citoq::detail::HostRuntimeCore publisher;

    citoq::detail::DiscoveryControlCore
        subscriber_control(
            subscriber,
            10,
            1001);

    citoq::detail::DiscoveryControlCore
        publisher_control(
            publisher,
            20,
            2001);

    const auto announcement =
        subscriber_control.announcement(
            47010,
            500);

    auto actions =
        publisher_control.on_summary(
            announcement,
            1000);

    assert(actions.size() == 1);
    assert(actions[0].coordinator == 10);

    // Drop the first CTS2 snapshot request/response (UDP is unreliable):
    // repeated unchanged announcements must eventually retry the missing page.
    actions = publisher_control.on_summary(announcement, 1100);
    assert(actions.empty()); // bounded retry: no 150ms control storm
    actions = publisher_control.on_summary(announcement, 1301);
    assert(actions.size() == 1);
    assert(snapshot_request(actions[0]).offset == 0);

    const auto request =
        snapshot_request(
            actions[0]);

    const auto batch_action =
        subscriber_control
            .on_snapshot_request(
                request);

    assert(
        batch_action.target ==
        citoq::detail::
            ControlTargetKind::Source);

    const auto batch =
        citoq::decode_snapshot_batch(
            batch_action.bytes);

    actions =
        publisher_control
            .on_snapshot_batch(
                batch);

    // Snapshot learning alone does not create route traffic until a
    // publisher asks for this DemandKey.
    assert(actions.empty());
    assert(
        publisher.candidate_hosts(
            position)
            .contains(10));

    actions =
        publisher_control
            .ensure_routes(
                position);

    assert(actions.size() == 1);
    // Lose the initial CTR3 request or CTR5 response. Repeated
    // unchanged advertisements must re-send only an unconfirmed route.
    actions = publisher_control.on_summary(announcement, 1400);
    assert(actions.size() == 1);
    assert(route_request(actions[0]).key == position);

    const auto route =
        route_request(
            actions[0]);

    const std::array<
        citoq::RouteEndpoint,
        1> endpoint{{
            {77, 48010}}};

    const auto route_action =
        subscriber_control
            .on_route_request(
                route,
                endpoint);

    const auto route_batch =
        citoq::decode_route_batch(
            route_action.bytes);

    assert(
        publisher_control
            .on_route_batch(
                route_batch));

    assert(
        publisher.has_remote_demand(
            position));
    assert(
        publisher.remote_destinations(
            position)
            .contains(77));

    // A successfully acknowledged route should not be re-requested.
    assert(publisher_control.on_summary(announcement, 1450).empty());

    // Duplicate ensure must not create control churn.
    assert(
        publisher_control
            .ensure_routes(
                position)
            .empty());

    // Subscriber withdraws its final local interest.
    subscriber.remove_interest(
        10,
        position);

    const auto withdrawn_announcement =
        subscriber_control.announcement(
            47010,
            500);

    actions =
        publisher_control.on_summary(
            withdrawn_announcement,
            1510);

    assert(actions.size() == 1);

    const auto empty_request =
        snapshot_request(
            actions[0]);

    const auto empty_batch_action =
        subscriber_control
            .on_snapshot_request(
                empty_request);

    const auto empty_batch =
        citoq::decode_snapshot_batch(
            empty_batch_action.bytes);

    actions =
        publisher_control
            .on_snapshot_batch(
                empty_batch);

    assert(actions.empty());
    assert(
        publisher.candidate_hosts(
            position).empty());
    assert(
        publisher.remote_destinations(
            position).empty());

    // A delayed old route response must not recreate a withdrawn route.
    assert(
        !publisher_control
             .on_route_batch(
                 route_batch));
    assert(
        publisher.remote_destinations(
            position).empty());

    // Explicit release is idempotent after withdrawal.
    assert(
        publisher_control.release_routes(
            position));
    assert(
        !publisher_control.release_routes(
            position));

    assert(
        publisher_control.stats()
            .snapshot_requests == 3);
    assert(publisher_control.stats().snapshot_retries == 1);
    assert(
        publisher_control.stats()
            .route_requests == 2);
    assert(publisher_control.stats().route_retries == 1);

    return 0;
}
