#include <cito/detail/udp_runtime_adapter.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <string_view>

namespace {

void exchange(
    cito::detail::UdpRuntimeAdapter& receiver,
    const cito::detail::UdpEndpoint& receiver_address,
    cito::detail::UdpRuntimeAdapter& sender,
    const cito::detail::UdpEndpoint& sender_address,
    std::vector<cito::detail::UdpSend> sends,
    std::uint64_t now_ms) {
    for (const auto& send : sends) {
        assert(
            send.endpoint ==
            receiver_address);

        auto replies =
            receiver.process_control(
                send.bytes,
                sender_address,
                now_ms);

        for (const auto& reply :
             replies) {
            assert(
                reply.endpoint ==
                sender_address);

            auto follow =
                sender.process_control(
                    reply.bytes,
                    receiver_address,
                    now_ms);

            if (!follow.empty()) {
                exchange(
                    receiver,
                    receiver_address,
                    sender,
                    sender_address,
                    std::move(follow),
                    now_ms);
            }
        }
    }
}

} // namespace

int main() {
    const cito::InterestKey position{
        7, 17, 100};
    const cito::InterestKey battery{
        7, 17, 200};

    std::size_t pos_received = 0;
    std::size_t bat_received = 0;

    cito::detail::HostRuntimeCore
        pos_host;
    pos_host.register_app(
        1,
        [&](auto type,
            std::string_view resource,
            auto payload) {
            assert(type == 100);
            assert(resource == "drone_17");
            assert(payload.size() == 5);
            ++pos_received;
            return std::size_t{1};
        });
    pos_host.add_interest(
        1,
        position);

    cito::detail::HostRuntimeCore
        bat_host;
    bat_host.register_app(
        2,
        [&](auto type,
            std::string_view,
            auto) {
            assert(type == 200);
            ++bat_received;
            return std::size_t{1};
        });
    bat_host.add_interest(
        2,
        battery);

    cito::detail::HostRuntimeCore
        pub_host;

    cito::detail::UdpRuntimeAdapter
        pos_udp(
            pos_host,
            11,
            11001);
    cito::detail::UdpRuntimeAdapter
        bat_udp(
            bat_host,
            12,
            12001);
    cito::detail::UdpRuntimeAdapter
        pub_udp(
            pub_host,
            20,
            20001);

    const cito::detail::UdpEndpoint
        pub_control{
            0x0a000001u,
            47120};
    const cito::detail::UdpEndpoint
        pos_control{
            0x0a000002u,
            47111};
    const cito::detail::UdpEndpoint
        bat_control{
            0x0a000003u,
            47112};

    pos_udp.set_local_route_endpoints(
        std::array<
            cito::RouteEndpoint,
            1>{{{111, 48111}}});

    bat_udp.set_local_route_endpoints(
        std::array<
            cito::RouteEndpoint,
            1>{{{112, 48112}}});

    exchange(
        pos_udp,
        pos_control,
        pub_udp,
        pub_control,
        pub_udp.process_control(
            pos_udp.announcement(
                47111,
                500),
            pos_control,
            1000),
        1000);

    exchange(
        bat_udp,
        bat_control,
        pub_udp,
        pub_control,
        pub_udp.process_control(
            bat_udp.announcement(
                47112,
                500),
            bat_control,
            1000),
        1000);

    auto route_sends =
        pub_udp.ensure_routes(
            position);

    assert(route_sends.size() == 1);
    assert(
        route_sends[0].endpoint ==
        pos_control);

    exchange(
        pos_udp,
        pos_control,
        pub_udp,
        pub_control,
        std::move(route_sends),
        1000);

    assert(
        pub_host.remote_destinations(
            position).contains(111));
    assert(
        pub_host.remote_destinations(
            battery).empty());

    const std::array<
        std::uint8_t,
        5> hello{
            'h','e','l','l','o'};

    std::size_t udp_send_calls = 0;

    const auto published =
        pub_udp.publish(
            position,
            "drone_17",
            hello,
            [&](const auto& endpoint,
                auto packet) {
                ++udp_send_calls;

                assert(
                    endpoint.ipv4 ==
                    pos_control.ipv4);
                assert(
                    endpoint.port ==
                    48111);

                assert(
                    pos_udp.receive_data(
                        packet,
                        "drone_17") ==
                    1);
                return true;
            });

    assert(
        published.local_delivered ==
        0);
    assert(
        published.remote_sent ==
        1);
    assert(
        published.packet_encodes ==
        1);
    assert(udp_send_calls == 1);

    assert(pos_received == 1);
    assert(bat_received == 0);

    // A completely unrelated publish does zero framing/network work.
    const cito::InterestKey unrelated{
        7, 18, 100};

    const auto none =
        pub_udp.publish(
            unrelated,
            "drone_18",
            hello,
            [&](const auto&, auto) {
                assert(false);
                return true;
            });

    assert(none.delivered() == 0);
    assert(none.packet_encodes == 0);
    assert(udp_send_calls == 1);

    return 0;
}
