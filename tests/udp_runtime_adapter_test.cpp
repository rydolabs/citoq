#include <cito/detail/udp_runtime_adapter.hpp>

#include <array>
#include <cassert>

int main() {
    const cito::InterestKey key{
        7, 17, 100};

    cito::detail::HostRuntimeCore
        subscriber;
    subscriber.register_app(
        1,
        [](auto, auto, auto) {
            return std::size_t{1};
        });
    subscriber.add_interest(
        1,
        key);

    cito::detail::UdpRuntimeAdapter
        sub_udp(
            subscriber,
            10,
            1001);

    const std::array<
        cito::RouteEndpoint,
        1> endpoints{{
            {77, 48010, 4433}}};

    sub_udp.set_local_route_endpoints(
        endpoints);

    cito::detail::HostRuntimeCore
        publisher;
    cito::detail::UdpRuntimeAdapter
        pub_udp(
            publisher,
            20,
            2001);

    const cito::detail::UdpEndpoint
        sub_source{
            0x0a000002u,
            47010};

    auto sends =
        pub_udp.process_control(
            sub_udp.announcement(
                47010,
                500),
            sub_source,
            1000);

    assert(sends.size() == 1);
    assert(
        sends[0].endpoint ==
        sub_source);

    const cito::detail::UdpEndpoint
        pub_source{
            0x0a000001u,
            47020};

    auto replies =
        sub_udp.process_control(
            sends[0].bytes,
            pub_source,
            1000);

    assert(replies.size() == 1);
    assert(
        replies[0].endpoint ==
        pub_source);

    sends =
        pub_udp.process_control(
            replies[0].bytes,
            sub_source,
            1000);

    assert(sends.empty());

    sends =
        pub_udp.ensure_routes(
            key);

    assert(sends.size() == 1);
    assert(
        sends[0].endpoint ==
        sub_source);

    replies =
        sub_udp.process_control(
            sends[0].bytes,
            pub_source,
            1000);

    assert(replies.size() == 1);

    sends =
        pub_udp.process_control(
            replies[0].bytes,
            sub_source,
            1000);

    assert(sends.empty());
    assert(
        pub_udp.data_endpoint(77) !=
        nullptr);
    assert(
        pub_udp.data_endpoint(77)->ipv4 ==
        sub_source.ipv4);
    assert(
        pub_udp.data_endpoint(77)->port ==
        48010);
    assert(
        pub_udp.quic_endpoint(77) !=
        nullptr);
    assert(
        pub_udp.quic_endpoint(77)->ipv4 ==
        sub_source.ipv4);
    assert(
        pub_udp.quic_endpoint(77)->port ==
        4433);

    // Recreate the route, then withdraw it through the advertised summary.
    assert(
        pub_udp.release_routes(
            key));
    assert(
        pub_udp.data_endpoint(77) ==
        nullptr);
    assert(
        pub_udp.quic_endpoint(77) ==
        nullptr);

    sends =
        pub_udp.ensure_routes(
            key);
    assert(sends.size() == 1);

    replies =
        sub_udp.process_control(
            sends[0].bytes,
            pub_source,
            1000);
    assert(replies.size() == 1);

    (void)pub_udp.process_control(
        replies[0].bytes,
        sub_source,
        1000);
    assert(
        pub_udp.data_endpoint(77) !=
        nullptr);

    subscriber.remove_interest(
        1,
        key);

    sends =
        pub_udp.process_control(
            sub_udp.announcement(
                47010,
                500),
            sub_source,
            1100);

    assert(sends.size() == 1);

    replies =
        sub_udp.process_control(
            sends[0].bytes,
            pub_source,
            1100);
    assert(replies.size() == 1);

    (void)pub_udp.process_control(
        replies[0].bytes,
        sub_source,
        1100);

    assert(
        publisher.remote_destinations(
            key).empty());
    assert(
        pub_udp.data_endpoint(77) ==
        nullptr);
    assert(
        pub_udp.quic_endpoint(77) ==
        nullptr);

    return 0;
}
