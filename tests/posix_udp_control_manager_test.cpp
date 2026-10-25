#include <cito/detail/posix_udp_control_manager.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>

int main() {
    using namespace cito::detail;

    const RequestKey request{
        7,
        17,
        100};

    const auto capability =
        request_handler_interest_key(
            request);

    HostRuntimeCore provider;
    assert(
        provider.register_app(
            1,
            [](auto, auto, auto) {
                return std::size_t{0};
            }));
    provider.add_interest(
        1,
        capability);

    UdpRuntimeAdapter provider_udp(
        provider,
        10,
        1001);

    const std::array<
        cito::RouteEndpoint,
        1> endpoints{{
            {77, 0, 4433}}};

    provider_udp
        .set_local_route_endpoints(
            endpoints);

    HostRuntimeCore requester;
    UdpRuntimeAdapter requester_udp(
        requester,
        20,
        2001);

    RequestSessionLifecycleCore lifecycle(
        requester,
        requester_udp);

    PosixUdpControlManager provider_io(
        provider_udp);
    PosixUdpControlManager requester_io(
        requester_udp,
        &lifecycle);

    assert(provider_io.bind(0));
    assert(requester_io.bind(0));
    assert(provider_io.control_port() != 0);
    assert(requester_io.control_port() != 0);

    provider_io.add_announcement_target(
        UdpEndpoint{
            0x7f000001u,
            requester_io.control_port()});

    provider_io.set_announcement_policy(
        500,
        10);

    // Desire is expressed before any coordinator has been learned.
    assert(lifecycle.ensure(request));
    assert(
        lifecycle.take_control_sends()
            .empty());

    const auto started =
        std::chrono::
            steady_clock::now();

    bool discovered = false;

    while (
        std::chrono::
            steady_clock::now() -
            started <
        std::chrono::
            seconds(2)) {
        const auto now =
            static_cast<
                std::uint64_t>(
                std::chrono::
                    duration_cast<
                        std::chrono::
                            milliseconds>(
                        std::chrono::
                            steady_clock::now()
                            .time_since_epoch())
                    .count());

        (void)provider_io.poll(now);
        (void)requester_io.poll(now);

        const auto* endpoint =
            requester_udp
                .quic_endpoint(77);

        if (
            endpoint &&
            endpoint->ipv4 ==
                0x7f000001u &&
            endpoint->port ==
                4433 &&
            requester
                .remote_destinations(
                    capability)
                .contains(77)) {
            discovered = true;
            break;
        }

        std::this_thread::sleep_for(
            std::chrono::
                milliseconds(1));
    }

    assert(discovered);

    const auto actions =
        lifecycle.reconcile();

    assert(actions.size() == 1);
    assert(
        actions[0].kind ==
        RequestSessionActionKind::
            Connect);
    assert(
        actions[0].destination ==
        77);
    assert(
        actions[0].endpoint.ipv4 ==
        0x7f000001u);
    assert(
        actions[0].endpoint.port ==
        4433);

    assert(
        provider_io.stats()
            .datagrams_received >= 2);
    assert(
        requester_io.stats()
            .datagrams_received >= 3);
    assert(
        provider_io.stats()
            .malformed_datagrams == 0);
    assert(
        requester_io.stats()
            .malformed_datagrams == 0);

    provider_io.stop();
    requester_io.stop();
    return 0;
}
