#include <cito/detail/request_session_lifecycle.hpp>

#include <array>
#include <cassert>
#include <cstdint>

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

    std::array<
        cito::RouteEndpoint,
        1> provider_endpoint{{
            {77, 0, 4433}}};

    provider_udp
        .set_local_route_endpoints(
            provider_endpoint);

    HostRuntimeCore requester;
    UdpRuntimeAdapter requester_udp(
        requester,
        20,
        2001);

    RequestSessionLifecycleCore
        sessions(
            requester,
            requester_udp);

    assert(sessions.ensure(request));
    assert(!sessions.ensure(request));
    assert(sessions.desired_count() == 1);
    assert(sessions.reconcile().empty());

    const UdpEndpoint provider_source{
        0x0a000002u,
        47010};

    const UdpEndpoint requester_source{
        0x0a000001u,
        47020};

    // Learn the handler capability summary.
    auto sends =
        requester_udp.process_control(
            provider_udp.announcement(
                47010,
                500),
            provider_source,
            1000);

    assert(sends.size() == 1);

    auto replies =
        provider_udp.process_control(
            sends[0].bytes,
            requester_source,
            1000);

    assert(replies.size() == 1);

    sends =
        requester_udp.process_control(
            replies[0].bytes,
            provider_source,
            1000);

    // sessions.ensure(request) expressed route desire before discovery.
    // Once the snapshot reveals a matching handler, CTR3 is emitted
    // automatically without a second explicit ensure_routes() call.
    assert(sends.size() == 1);
    assert(
        requester.candidate_hosts(
            capability)
            .contains(10));

    replies =
        provider_udp.process_control(
            sends[0].bytes,
            requester_source,
            1000);

    assert(replies.size() == 1);
    assert(replies[0].bytes[3] == '5');

    sends =
        requester_udp.process_control(
            replies[0].bytes,
            provider_source,
            1000);

    assert(sends.empty());
    assert(
        requester_udp.data_endpoint(
            77) == nullptr);
    assert(
        requester_udp.quic_endpoint(
            77) != nullptr);
    assert(
        requester_udp.quic_endpoint(
            77)->port == 4433);

    auto actions =
        sessions.reconcile();

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
        provider_source.ipv4);
    assert(
        actions[0].endpoint.port ==
        4433);
    assert(
        sessions.is_connecting(
            77));

    assert(
        sessions.mark_connected(
            77,
            actions[0].endpoint));
    assert(
        sessions.is_connected(
            77));
    assert(sessions.reconcile().empty());

    // A route endpoint change produces Disconnect(old) + Connect(new).
    provider_endpoint[0].quic_port =
        4444;
    provider_udp
        .set_local_route_endpoints(
            provider_endpoint);

    assert(
        sessions.release(
            request));
    assert(
        sessions.ensure(
            request));

    sends =
        sessions.take_control_sends();
    assert(sends.size() == 1);

    replies =
        provider_udp.process_control(
            sends[0].bytes,
            requester_source,
            1010);
    assert(replies.size() == 1);

    (void)requester_udp.process_control(
        replies[0].bytes,
        provider_source,
        1010);

    actions =
        sessions.reconcile();

    assert(actions.size() == 2);
    assert(
        actions[0].kind ==
        RequestSessionActionKind::
            Disconnect);
    assert(
        actions[0].endpoint.port ==
        4433);
    assert(
        actions[1].kind ==
        RequestSessionActionKind::
            Connect);
    assert(
        actions[1].endpoint.port ==
        4444);

    assert(
        sessions.mark_connect_failed(
            77,
            actions[1].endpoint));
    assert(
        !sessions.is_connecting(
            77));

    actions =
        sessions.reconcile();
    assert(actions.size() == 1);
    assert(
        actions[0].kind ==
        RequestSessionActionKind::
            Connect);
    assert(
        actions[0].endpoint.port ==
        4444);

    assert(
        sessions.mark_connected(
            77,
            actions[0].endpoint));

    // Handler withdrawal removes the route endpoint and disconnects session.
    provider.remove_interest(
        1,
        capability);

    sends =
        requester_udp.process_control(
            provider_udp.announcement(
                47010,
                500),
            provider_source,
            1100);
    assert(sends.size() == 1);

    replies =
        provider_udp.process_control(
            sends[0].bytes,
            requester_source,
            1100);
    assert(replies.size() == 1);

    (void)requester_udp.process_control(
        replies[0].bytes,
        provider_source,
        1100);

    assert(
        requester.remote_destinations(
            capability).empty());
    assert(
        requester_udp.quic_endpoint(
            77) == nullptr);

    actions =
        sessions.reconcile();

    assert(actions.size() == 1);
    assert(
        actions[0].kind ==
        RequestSessionActionKind::
            Disconnect);
    assert(actions[0].destination == 77);
    assert(
        sessions.tracked_count() ==
        0);

    assert(sessions.release(request));
    assert(!sessions.release(request));
    assert(
        sessions.take_control_sends()
            .empty());
    assert(sessions.desired_count() == 0);

    return 0;
}
