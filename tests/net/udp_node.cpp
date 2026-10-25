#include <cito/data_packet.hpp>
#include <cito/discovery.hpp>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now().time_since_epoch()).count());
}

int make_udp() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        std::exit(20);
    }
    return fd;
}

sockaddr_in any_address(std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    return address;
}

bool bind_udp(int fd, std::uint16_t port) {
    auto address = any_address(port);
    return ::bind(
        fd,
        reinterpret_cast<sockaddr*>(&address),
        sizeof(address)) == 0;
}

std::vector<std::uint8_t> receive_bytes(
    int fd,
    sockaddr_in* from = nullptr) {
    std::vector<std::uint8_t> bytes(65535);
    socklen_t size = sizeof(sockaddr_in);
    sockaddr_in temporary{};

    const auto received = ::recvfrom(
        fd,
        bytes.data(),
        bytes.size(),
        0,
        reinterpret_cast<sockaddr*>(from ? from : &temporary),
        &size);

    if (received < 0) {
        return {};
    }

    bytes.resize(static_cast<std::size_t>(received));
    return bytes;
}

int subscriber(int argc, char** argv) {
    if (argc != 8) {
        std::cerr << "subscriber args\n";
        return 2;
    }

    const auto node = std::stoull(argv[2]);
    const auto type = std::stoull(argv[3]);
    const auto data_port =
        static_cast<std::uint16_t>(std::stoul(argv[4]));
    const auto discovery_port =
        static_cast<std::uint16_t>(std::stoul(argv[5]));
    const std::string broadcast_ip = argv[6];
    const bool expect_data = std::stoi(argv[7]) != 0;

    const cito::InterestKey key{7, 17, type};

    const int data_fd = make_udp();
    if (!bind_udp(data_fd, data_port)) {
        perror("bind data");
        return 21;
    }

    const int advertisement_fd = make_udp();
    int yes = 1;
    if (::setsockopt(
            advertisement_fd,
            SOL_SOCKET,
            SO_BROADCAST,
            &yes,
            sizeof(yes)) != 0) {
        perror("SO_BROADCAST");
        return 22;
    }

    sockaddr_in broadcast{};
    broadcast.sin_family = AF_INET;
    broadcast.sin_port = htons(discovery_port);
    if (::inet_pton(
            AF_INET,
            broadcast_ip.c_str(),
            &broadcast.sin_addr) != 1) {
        return 23;
    }

    const cito::InterestAdvertisement advertisement{
        cito::InterestOp::Add,
        data_port,
        700,
        static_cast<cito::DestinationId>(node),
        key};

    const auto packet = cito::encode_interest(advertisement);
    const auto start = Clock::now();
    auto next_send = start;

    while (Clock::now() - start < std::chrono::milliseconds(3000)) {
        const auto now = Clock::now();

        if (now >= next_send) {
            const auto sent = ::sendto(
                advertisement_fd,
                packet.data(),
                packet.size(),
                0,
                reinterpret_cast<sockaddr*>(&broadcast),
                sizeof(broadcast));

            if (sent != static_cast<ssize_t>(packet.size())) {
                perror("send advertisement");
                return 24;
            }

            next_send = now + std::chrono::milliseconds(150);
        }

        pollfd descriptor{data_fd, POLLIN, 0};
        const int result = ::poll(&descriptor, 1, 50);
        if (result < 0) {
            perror("poll");
            return 25;
        }

        if (result > 0 && (descriptor.revents & POLLIN)) {
            const auto bytes = receive_bytes(data_fd);
            if (bytes.empty()) {
                continue;
            }

            try {
                const auto data = cito::decode_data_packet_view(bytes);
                constexpr std::string_view expected = "hello";
                if (data.key == key &&
                    data.payload.size() == expected.size() &&
                    std::equal(
                        data.payload.begin(),
                        data.payload.end(),
                        expected.begin())) {
                    ::close(advertisement_fd);
                    ::close(data_fd);

                    if (expect_data) {
                        std::cout << "received expected data\n";
                        return 0;
                    }

                    std::cerr << "received unrelated data\n";
                    return 26;
                }
            } catch (const std::exception& error) {
                std::cerr << error.what() << "\n";
                return 27;
            }
        }
    }

    ::close(advertisement_fd);
    ::close(data_fd);

    if (expect_data) {
        std::cerr << "timed out waiting for expected data\n";
        return 28;
    }

    std::cout << "no unrelated data received\n";
    return 0;
}

int publisher(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "publisher args\n";
        return 2;
    }

    const auto target_type = std::stoull(argv[2]);
    const auto discovery_port =
        static_cast<std::uint16_t>(std::stoul(argv[3]));
    const auto expected_destinations = std::stoull(argv[4]);
    const auto listen_ms = std::stoull(argv[5]);

    const cito::InterestKey target{7, 17, target_type};

    const int fd = make_udp();
    int yes = 1;
    (void)::setsockopt(
        fd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &yes,
        sizeof(yes));

    if (!bind_udp(fd, discovery_port)) {
        perror("bind discovery");
        return 31;
    }

    cito::InterestIndex index;
    cito::InterestLeaseTable leases(index);
    std::unordered_map<
        cito::DestinationId,
        sockaddr_in> endpoints;

    const auto start = Clock::now();

    while (
        Clock::now() - start <
        std::chrono::milliseconds(listen_ms)) {
        pollfd descriptor{fd, POLLIN, 0};
        const int result = ::poll(&descriptor, 1, 100);

        if (result < 0) {
            perror("poll discovery");
            return 32;
        }

        if (result > 0 && (descriptor.revents & POLLIN)) {
            sockaddr_in from{};
            const auto bytes = receive_bytes(fd, &from);
            if (bytes.empty()) {
                continue;
            }

            try {
                const auto advertisement =
                    cito::decode_interest(bytes);

                leases.observe(
                    advertisement,
                    now_ms());

                if (advertisement.op == cito::InterestOp::Add) {
                    from.sin_port =
                        htons(advertisement.data_port);
                    endpoints[advertisement.destination] = from;
                } else {
                    endpoints.erase(
                        advertisement.destination);
                }
            } catch (const std::exception& error) {
                std::cerr
                    << "bad discovery: "
                    << error.what()
                    << "\n";
                return 33;
            }
        }

        leases.expire(now_ms());
    }

    const auto& destinations = index.lookup(target);
    if (destinations.size() != expected_destinations) {
        std::cerr
            << "expected "
            << expected_destinations
            << " destinations, got "
            << destinations.size()
            << "\n";
        return 34;
    }

    const cito::DataPacket data{
        target,
        std::vector<std::uint8_t>{
            'h', 'e', 'l', 'l', 'o'}};

    const auto payload = cito::encode_data_packet(data);

    for (const auto id : destinations) {
        const auto endpoint = endpoints.find(id);
        if (endpoint == endpoints.end()) {
            std::cerr << "missing endpoint\n";
            return 35;
        }

        const auto sent = ::sendto(
            fd,
            payload.data(),
            payload.size(),
            0,
            reinterpret_cast<const sockaddr*>(
                &endpoint->second),
            sizeof(endpoint->second));

        if (sent != static_cast<ssize_t>(payload.size())) {
            perror("send data");
            return 36;
        }
    }

    std::cout
        << "discovered="
        << destinations.size()
        << " sent="
        << destinations.size()
        << "\n";

    ::close(fd);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return 2;
    }

    if (std::strcmp(argv[1], "subscriber") == 0) {
        return subscriber(argc, argv);
    }

    if (std::strcmp(argv[1], "publisher") == 0) {
        return publisher(argc, argv);
    }

    return 2;
}
