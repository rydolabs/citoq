#include <cito/detail/udp_runtime_adapter.hpp>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<
            std::chrono::milliseconds>(
            Clock::now().time_since_epoch())
            .count());
}

int make_udp() {
    const int fd =
        ::socket(
            AF_INET,
            SOCK_DGRAM,
            0);

    if (fd < 0) {
        perror("socket");
        std::exit(20);
    }

    return fd;
}

bool bind_udp(
    int fd,
    std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr =
        htonl(INADDR_ANY);
    address.sin_port =
        htons(port);

    return ::bind(
        fd,
        reinterpret_cast<
            sockaddr*>(&address),
        sizeof(address)) == 0;
}

cito::detail::UdpEndpoint
to_endpoint(
    const sockaddr_in& address) {
    return {
        ntohl(
            address.sin_addr.s_addr),
        ntohs(
            address.sin_port)};
}

sockaddr_in to_sockaddr(
    const cito::detail::UdpEndpoint&
        endpoint) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr =
        htonl(endpoint.ipv4);
    address.sin_port =
        htons(endpoint.port);
    return address;
}

cito::detail::UdpEndpoint
parse_endpoint(
    const std::string& ip,
    std::uint16_t port) {
    in_addr address{};

    if (::inet_pton(
            AF_INET,
            ip.c_str(),
            &address) != 1) {
        throw std::runtime_error(
            "invalid IPv4 address");
    }

    return {
        ntohl(address.s_addr),
        port};
}

bool send_span(
    int fd,
    const cito::detail::UdpEndpoint&
        endpoint,
    std::span<const std::uint8_t> bytes) {
    const auto address =
        to_sockaddr(endpoint);

    return ::sendto(
        fd,
        bytes.data(),
        bytes.size(),
        0,
        reinterpret_cast<
            const sockaddr*>(&address),
        sizeof(address)) ==
        static_cast<ssize_t>(
            bytes.size());
}

bool send_owned(
    int fd,
    const cito::detail::UdpSend& send) {
    return send_span(
        fd,
        send.endpoint,
        send.bytes);
}

std::vector<std::uint8_t>
receive_bytes(
    int fd,
    sockaddr_in* from = nullptr) {
    std::vector<std::uint8_t>
        bytes(65535);

    sockaddr_in temporary{};
    socklen_t size =
        sizeof(temporary);

    const auto received =
        ::recvfrom(
            fd,
            bytes.data(),
            bytes.size(),
            0,
            reinterpret_cast<
                sockaddr*>(
                from
                    ? from
                    : &temporary),
            &size);

    if (received < 0) {
        return {};
    }

    bytes.resize(
        static_cast<std::size_t>(
            received));
    return bytes;
}

bool send_actions(
    int fd,
    const std::vector<
        cito::detail::UdpSend>& sends) {
    for (const auto& send :
         sends) {
        if (!send_owned(
                fd,
                send)) {
            return false;
        }
    }

    return true;
}

bool is_route_request(
    std::span<const std::uint8_t>
        bytes) {
    return
        bytes.size() >= 4 &&
        bytes[0] == 'C' &&
        bytes[1] == 'T' &&
        bytes[2] == 'R' &&
        bytes[3] == '3';
}

int subscriber(
    int argc,
    char** argv) {
    if (argc != 9 &&
        argc != 10) {
        std::cerr
            << "subscriber args\n";
        return 2;
    }

    const auto node =
        std::stoull(argv[2]);
    const auto type =
        std::stoull(argv[3]);
    const auto data_port =
        static_cast<std::uint16_t>(
            std::stoul(argv[4]));
    const auto control_port =
        static_cast<std::uint16_t>(
            std::stoul(argv[5]));
    const auto announce_port =
        static_cast<std::uint16_t>(
            std::stoul(argv[6]));
    const std::string announce_ip =
        argv[7];
    const int behavior =
        std::stoi(argv[8]);
    const bool expect_data =
        behavior == 1;
    const bool lease_source_only =
        behavior == 2;
    const auto lifetime_ms =
        argc == 10
            ? std::stoull(argv[9])
            : 3200ull;

    const cito::InterestKey key{
        7,
        17,
        type};

    std::size_t delivered = 0;

    cito::detail::HostRuntimeCore host;
    host.register_app(
        node,
        [&](auto received_type,
            std::string_view resource,
            auto payload) {
            if (
                received_type != type ||
                resource != "drone_17" ||
                payload.size() != 5 ||
                !std::equal(
                    payload.begin(),
                    payload.end(),
                    std::string_view(
                        "hello").begin())) {
                return std::size_t{0};
            }

            ++delivered;
            return std::size_t{1};
        });

    host.add_interest(
        node,
        key);

    cito::detail::UdpRuntimeAdapter
        udp(
            host,
            node,
            node * 1000 + 1);

    const std::array<
        cito::RouteEndpoint,
        1> routes{{
            {node, data_port}}};

    udp.set_local_route_endpoints(
        routes);

    const int control_fd =
        make_udp();
    const int data_fd =
        make_udp();

    int yes = 1;
    (void)::setsockopt(
        control_fd,
        SOL_SOCKET,
        SO_BROADCAST,
        &yes,
        sizeof(yes));

    if (!bind_udp(
            control_fd,
            control_port) ||
        !bind_udp(
            data_fd,
            data_port)) {
        perror("bind");
        return 21;
    }

    const auto announce_to =
        parse_endpoint(
            announce_ip,
            announce_port);

    const auto announcement =
        udp.announcement(
            control_port,
            600);

    const auto start =
        Clock::now();
    auto next_announce =
        start;

    std::size_t route_requests = 0;
    std::size_t control_packets = 0;

    while (
        Clock::now() - start <
        std::chrono::milliseconds(
            lifetime_ms)) {
        const auto now =
            Clock::now();

        if (now >= next_announce) {
            if (!send_span(
                    control_fd,
                    announce_to,
                    announcement)) {
                perror(
                    "send announcement");
                return 22;
            }

            next_announce =
                now +
                std::chrono::milliseconds(
                    150);
        }

        pollfd fds[2]{
            {control_fd, POLLIN, 0},
            {data_fd, POLLIN, 0}};

        const auto polled =
            ::poll(
                fds,
                2,
                50);

        if (polled < 0) {
            perror("poll");
            return 23;
        }

        if (
            fds[0].revents &
            POLLIN) {
            sockaddr_in from{};
            const auto bytes =
                receive_bytes(
                    control_fd,
                    &from);

            if (!bytes.empty()) {
                ++control_packets;

                if (is_route_request(
                        bytes)) {
                    ++route_requests;
                }

                try {
                    const auto sends =
                        udp.process_control(
                            bytes,
                            to_endpoint(
                                from),
                            now_ms());

                    if (!send_actions(
                            control_fd,
                            sends)) {
                        return 24;
                    }
                } catch (
                    const std::exception&
                        error) {
                    std::cerr
                        << error.what()
                        << "\n";
                    return 25;
                }
            }
        }

        if (
            fds[1].revents &
            POLLIN) {
            const auto bytes =
                receive_bytes(
                    data_fd);

            if (bytes.empty()) {
                continue;
            }

            try {
                const auto count =
                    udp.receive_data(
                        bytes,
                        "drone_17");

                if (count != 0) {
                    if (!expect_data) {
                        std::cerr
                            << "unexpected data\n";
                        return 26;
                    }

                    std::cout
                        << "received=1 "
                        << "route_requests="
                        << route_requests
                        << " control_packets="
                        << control_packets
                        << "\n";
                    return 0;
                }
            } catch (
                const std::exception&
                    error) {
                std::cerr
                    << error.what()
                    << "\n";
                return 27;
            }
        }
    }

    if (expect_data) {
        std::cerr
            << "data timeout\n";
        return 28;
    }

    if (lease_source_only) {
        if (route_requests == 0) {
            std::cerr
                << "lease source was not routed\n";
            return 29;
        }

        std::cout
            << "lease_source_end "
            << "route_requests="
            << route_requests
            << " control_packets="
            << control_packets
            << "\n";
        return 0;
    }

    if (route_requests != 0) {
        std::cerr
            << "unrelated route request\n";
        return 30;
    }

    std::cout
        << "received=0 "
        << "route_requests=0 "
        << "control_packets="
        << control_packets
        << "\n";
    return 0;
}

int publisher(
    int argc,
    char** argv) {
    if (argc != 6) {
        std::cerr
            << "publisher args\n";
        return 2;
    }

    const auto type =
        std::stoull(argv[2]);
    const auto announce_port =
        static_cast<std::uint16_t>(
            std::stoul(argv[3]));
    const auto expected =
        std::stoull(argv[4]);
    const auto listen_ms =
        std::stoull(argv[5]);

    const cito::InterestKey key{
        7,
        17,
        type};

    cito::detail::HostRuntimeCore host;
    cito::detail::UdpRuntimeAdapter
        udp(
            host,
            100,
            100001);

    // Express publisher-side route demand before discovery completes.
    // Later snapshots trigger route detail only for matching hosts.
    (void)udp.ensure_routes(
        key);

    const int fd =
        make_udp();

    int yes = 1;
    (void)::setsockopt(
        fd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &yes,
        sizeof(yes));

    if (!bind_udp(
            fd,
            announce_port)) {
        perror("bind publisher");
        return 40;
    }

    const auto start =
        Clock::now();
    bool sent = false;
    std::size_t sends = 0;

    while (
        Clock::now() - start <
        std::chrono::milliseconds(
            listen_ms)) {
        pollfd descriptor{
            fd,
            POLLIN,
            0};

        const auto polled =
            ::poll(
                &descriptor,
                1,
                50);

        if (polled < 0) {
            perror("poll");
            return 41;
        }

        if (
            polled > 0 &&
            (descriptor.revents &
             POLLIN)) {
            sockaddr_in from{};
            const auto bytes =
                receive_bytes(
                    fd,
                    &from);

            if (!bytes.empty()) {
                try {
                    const auto actions =
                        udp.process_control(
                            bytes,
                            to_endpoint(
                                from),
                            now_ms());

                    if (!send_actions(
                            fd,
                            actions)) {
                        return 42;
                    }
                } catch (
                    const std::exception&
                        error) {
                    std::cerr
                        << error.what()
                        << "\n";
                    return 43;
                }
            }
        }

        const auto& stats =
            udp.control().stats();

        if (
            !sent &&
            stats.snapshot_requests >= 2 &&
            stats.route_requests ==
                expected &&
            host.remote_destinations(
                key).size() ==
                expected) {
            const std::array<
                std::uint8_t,
                5> hello{
                    'h','e','l','l','o'};

            const auto result =
                udp.publish(
                    key,
                    "drone_17",
                    hello,
                    [&](const auto& endpoint,
                        auto packet) {
                        if (send_span(
                                fd,
                                endpoint,
                                packet)) {
                            ++sends;
                            return true;
                        }
                        return false;
                    });

            if (
                result.remote_sent !=
                    expected ||
                result.packet_encodes !=
                    (expected == 0
                         ? 0u
                         : 1u)) {
                return 44;
            }

            sent = true;

            if (expected != 0) {
                break;
            }
        }

        udp.expire(
            now_ms());
    }

    const auto& stats =
        udp.control().stats();

    if (!sent) {
        std::cerr
            << "publisher did not reach route state\n";
        return 45;
    }

    std::cout
        << "snapshot_requests="
        << stats.snapshot_requests
        << " route_requests="
        << stats.route_requests
        << " discovered="
        << host.remote_destinations(
               key).size()
        << " sent="
        << sends
        << "\n";

    return 0;
}

int publisher_expiry(
    int argc,
    char** argv) {
    if (argc != 5) {
        std::cerr
            << "publisher-expiry args\n";
        return 2;
    }

    const auto type =
        std::stoull(argv[2]);
    const auto announce_port =
        static_cast<std::uint16_t>(
            std::stoul(argv[3]));
    const auto listen_ms =
        std::stoull(argv[4]);

    const cito::InterestKey key{
        7,
        17,
        type};

    cito::detail::HostRuntimeCore host;
    cito::detail::UdpRuntimeAdapter
        udp(
            host,
            101,
            101001);

    (void)udp.ensure_routes(
        key);

    const int fd =
        make_udp();

    int yes = 1;
    (void)::setsockopt(
        fd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &yes,
        sizeof(yes));

    if (!bind_udp(
            fd,
            announce_port)) {
        perror("bind expiry publisher");
        return 50;
    }

    const auto start =
        Clock::now();

    bool discovered = false;
    cito::DestinationId discovered_id = 0;

    while (
        Clock::now() - start <
        std::chrono::milliseconds(
            listen_ms)) {
        pollfd descriptor{
            fd,
            POLLIN,
            0};

        const auto polled =
            ::poll(
                &descriptor,
                1,
                50);

        if (polled < 0) {
            perror("poll");
            return 51;
        }

        if (
            polled > 0 &&
            (descriptor.revents &
             POLLIN)) {
            sockaddr_in from{};
            const auto bytes =
                receive_bytes(
                    fd,
                    &from);

            if (!bytes.empty()) {
                try {
                    const auto actions =
                        udp.process_control(
                            bytes,
                            to_endpoint(from),
                            now_ms());

                    if (!send_actions(
                            fd,
                            actions)) {
                        return 52;
                    }
                } catch (
                    const std::exception&
                        error) {
                    std::cerr
                        << error.what()
                        << "\n";
                    return 53;
                }
            }
        }

        const auto& routes =
            host.remote_destinations(
                key);

        if (!routes.empty()) {
            discovered = true;
            discovered_id =
                *routes.begin();
        }

        udp.expire(
            now_ms());
    }

    const auto& routes =
        host.remote_destinations(
            key);

    const auto endpoint =
        discovered_id == 0
            ? nullptr
            : udp.data_endpoint(
                discovered_id);

    const auto& stats =
        udp.control().stats();

    if (
        !discovered ||
        !routes.empty() ||
        endpoint != nullptr ||
        stats.expired_coordinators == 0) {
        std::cerr
            << "expiry cleanup failed "
            << "discovered="
            << discovered
            << " routes="
            << routes.size()
            << " expired="
            << stats.expired_coordinators
            << "\n";
        return 54;
    }

    std::cout
        << "lease_expired=1 "
        << "routes=0 "
        << "endpoint_cache=0 "
        << "expired_coordinators="
        << stats.expired_coordinators
        << "\n";

    return 0;
}

} // namespace

int main(
    int argc,
    char** argv) {
    if (argc < 2) {
        return 2;
    }

    const std::string mode =
        argv[1];

    if (mode == "subscriber") {
        return subscriber(
            argc,
            argv);
    }

    if (mode == "publisher") {
        return publisher(
            argc,
            argv);
    }

    if (mode == "publisher-expiry") {
        return publisher_expiry(
            argc,
            argv);
    }

    return 2;
}
