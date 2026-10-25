#include <cito/detail/discovery_control_core.hpp>
#include <cito/detail/request_reply_core.hpp>

#include <array>
#include <cassert>
#include <cstdint>

namespace {

void learn(
    cito::detail::DiscoveryControlCore& remote,
    cito::detail::DiscoveryControlCore& requester,
    std::uint16_t port,
    std::uint64_t now) {
    const auto announcement =
        remote.announcement(
            port,
            500);

    auto actions =
        requester.on_summary(
            announcement,
            now);

    assert(actions.size() == 1);

    const auto snapshot_request =
        cito::decode_snapshot_request(
            actions[0].bytes);

    const auto batch_action =
        remote.on_snapshot_request(
            snapshot_request);

    const auto batch =
        cito::decode_snapshot_batch(
            batch_action.bytes);

    actions =
        requester.on_snapshot_batch(
            batch);

    assert(actions.empty());
}

} // namespace

int main() {
    const cito::detail::RequestKey request{
        7,
        17,
        100};

    const cito::InterestKey ordinary{
        request.scope,
        request.resource,
        request.request_type};

    const auto capability =
        cito::detail::
            request_handler_interest_key(
                request);

    // Golden identity: independent of native endian/integer layout.
    assert(
        cito::detail::
            request_handler_capability_type(
                100) ==
        9713800924229500400ull);

    assert(capability.scope == ordinary.scope);
    assert(capability.resource == ordinary.resource);
    assert(capability.type != ordinary.type);

    cito::detail::HostRuntimeCore provider;
    cito::detail::HostRuntimeCore subscriber;
    cito::detail::HostRuntimeCore requester;

    assert(provider.register_app(
        10,
        [](auto, auto, auto) {
            return std::size_t{0};
        }));

    assert(subscriber.register_app(
        11,
        [](auto, auto, auto) {
            return std::size_t{0};
        }));

    provider.add_interest(
        10,
        capability);

    // An ordinary subscriber for the request message is deliberately not a
    // Request/Reply provider.
    subscriber.add_interest(
        11,
        ordinary);

    cito::detail::DiscoveryControlCore
        provider_control(
            provider,
            10,
            1001);

    cito::detail::DiscoveryControlCore
        subscriber_control(
            subscriber,
            11,
            1101);

    cito::detail::DiscoveryControlCore
        requester_control(
            requester,
            20,
            2001);

    learn(
        provider_control,
        requester_control,
        47010,
        1000);

    learn(
        subscriber_control,
        requester_control,
        47011,
        1010);

    assert(
        requester.candidate_hosts(
            capability)
            .size() == 1);
    assert(
        requester.candidate_hosts(
            capability)
            .contains(10));
    assert(
        !requester.candidate_hosts(
            capability)
             .contains(11));

    assert(
        requester.candidate_hosts(
            ordinary)
            .size() == 1);
    assert(
        requester.candidate_hosts(
            ordinary)
            .contains(11));

    const auto actions =
        requester_control.ensure_routes(
            capability);

    assert(actions.size() == 1);
    assert(actions[0].coordinator == 10);

    const auto route =
        cito::decode_route_request(
            actions[0].bytes);

    assert(route.key == capability);

    const std::array<
        cito::RouteEndpoint,
        1> endpoint{{
            {77, 48010}}};

    const auto route_action =
        provider_control
            .on_route_request(
                route,
                endpoint);

    assert(!route_action.bytes.empty());

    const auto route_batch =
        cito::decode_route_batch(
            route_action.bytes);

    assert(
        requester_control
            .on_route_batch(
                route_batch));

    assert(
        requester.remote_destinations(
            capability)
            .contains(77));

    return 0;
}
