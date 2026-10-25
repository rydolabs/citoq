#include <citoq/detail/discovery_control_core.hpp>
#include <citoq/detail/request_reply_core.hpp>

#include <array>
#include <cassert>
#include <cstdint>

namespace {

void learn(
    citoq::detail::DiscoveryControlCore& remote,
    citoq::detail::DiscoveryControlCore& requester,
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
        citoq::decode_snapshot_request(
            actions[0].bytes);

    const auto batch_action =
        remote.on_snapshot_request(
            snapshot_request);

    const auto batch =
        citoq::decode_snapshot_batch(
            batch_action.bytes);

    actions =
        requester.on_snapshot_batch(
            batch);

    assert(actions.empty());
}

} // namespace

int main() {
    const citoq::detail::RequestKey request{
        7,
        17,
        100};

    const citoq::InterestKey ordinary{
        request.scope,
        request.resource,
        request.request_type};

    const auto capability =
        citoq::detail::
            request_handler_interest_key(
                request);

    // Golden identity: independent of native endian/integer layout.
    assert(
        citoq::detail::
            request_handler_capability_type(
                100) ==
        9713800924229500400ull);

    assert(capability.scope == ordinary.scope);
    assert(capability.resource == ordinary.resource);
    assert(capability.type != ordinary.type);

    citoq::detail::HostRuntimeCore provider;
    citoq::detail::HostRuntimeCore subscriber;
    citoq::detail::HostRuntimeCore requester;

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

    citoq::detail::DiscoveryControlCore
        provider_control(
            provider,
            10,
            1001);

    citoq::detail::DiscoveryControlCore
        subscriber_control(
            subscriber,
            11,
            1101);

    citoq::detail::DiscoveryControlCore
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
        citoq::decode_route_request(
            actions[0].bytes);

    assert(route.key == capability);

    const std::array<
        citoq::RouteEndpoint,
        1> endpoint{{
            {77, 48010}}};

    const auto route_action =
        provider_control
            .on_route_request(
                route,
                endpoint);

    assert(!route_action.bytes.empty());

    const auto route_batch =
        citoq::decode_route_batch(
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
