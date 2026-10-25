#include <cito/detail/shm_ring.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
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
        64,
        1);

    // Give independent reader processes time to map read-only.
    std::this_thread::sleep_for(
        std::chrono::milliseconds(300));

    for (std::uint64_t i = 1;
         i <= count;
         ++i) {
        ring.publish(
            "msg:" +
            std::to_string(i));

        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }

    std::this_thread::sleep_for(
        std::chrono::milliseconds(250));

    std::cout
        << "writer sent="
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

        ++received;
        const std::string value(
            bytes.begin(),
            bytes.end());

        if (
            value !=
            "msg:" +
                std::to_string(received)) {
            std::cerr
                << "unexpected value "
                << value
                << " at "
                << received
                << "\n";
            return 20;
        }
    }

    if (
        received != expected ||
        dropped != 0) {
        std::cerr
            << "reader received="
            << received
            << " dropped="
            << dropped
            << "\n";
        return 21;
    }

    std::cout
        << "reader received="
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
