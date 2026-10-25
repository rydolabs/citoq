#include "position.hpp"
#include <cito/detail/shm_ring.hpp>
#include <cito/static.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int writer(
    const std::string& name,
    std::uint64_t count) {
    cito::detail::ShmRingWriter ring(
        name,
        256,
        512,
        1);

    std::this_thread::sleep_for(
        std::chrono::milliseconds(300));

    for (std::uint64_t i = 1;
         i <= count;
         ++i) {
        acme::navigation::Position value{};
        value.x = static_cast<double>(i);
        value.y = -static_cast<double>(i);
        value.frame = "map";
        value.timestamp = i;
        value.frame_kind =
            acme::navigation::FrameKind::Map;

        const auto bytes =
            cito::encode(value);

        ring.publish(
            bytes.data(),
            bytes.size());

        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }

    std::this_thread::sleep_for(
        std::chrono::milliseconds(250));

    std::cout
        << "typed writer sent="
        << count
        << "\n";
    return 0;
}

int reader(
    const std::string& name,
    std::uint64_t expected) {
    cito::detail::ShmRingReader ring(
        name);

    std::vector<std::uint8_t> bytes;
    std::uint64_t dropped = 0;
    std::uint64_t received = 0;

    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(5);

    while (
        received < expected &&
        std::chrono::steady_clock::now() <
            deadline) {
        if (!ring.try_read_next(
                bytes,
                dropped)) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(100));
            continue;
        }

        const auto value =
            cito::decode<
                acme::navigation::Position>(
                    bytes);

        ++received;

        if (
            value.x !=
                static_cast<double>(
                    received) ||
            value.y !=
                -static_cast<double>(
                    received) ||
            value.frame != "map" ||
            !value.timestamp ||
            *value.timestamp != received ||
            !value.frame_kind ||
            *value.frame_kind !=
                acme::navigation::
                    FrameKind::Map) {
            std::cerr
                << "typed SHM value mismatch at "
                << received
                << "\n";
            return 20;
        }
    }

    if (
        received != expected ||
        dropped != 0) {
        std::cerr
            << "typed reader received="
            << received
            << " dropped="
            << dropped
            << "\n";
        return 21;
    }

    std::cout
        << "typed reader received="
        << received
        << " dropped=0\n";
    return 0;
}

} // namespace

int main(
    int argc,
    char** argv) {
    if (argc != 4) {
        return 2;
    }

    const std::string role =
        argv[1];
    const std::string name =
        argv[2];
    const auto count =
        std::stoull(argv[3]);

    if (role == "writer") {
        return writer(
            name,
            count);
    }

    if (role == "reader") {
        return reader(
            name,
            count);
    }

    return 2;
}
