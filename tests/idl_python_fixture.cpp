#include "position.hpp"

#include <cassert>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string to_hex(
    const std::vector<std::uint8_t>& bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        out << std::setw(2)
            << static_cast<unsigned>(byte);
    }
    return out.str();
}

std::vector<std::uint8_t> from_hex(
    const std::string& text) {
    if (text.size() % 2 != 0) {
        throw std::invalid_argument("odd hex length");
    }

    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 2);

    for (std::size_t i = 0;
         i < text.size();
         i += 2) {
        const auto value =
            std::stoul(
                text.substr(i, 2),
                nullptr,
                16);
        out.push_back(
            static_cast<std::uint8_t>(
                value));
    }

    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 &&
        std::string(argv[1]) == "encode") {
        acme::navigation::Position value{};
        value.x = 1.25;
        value.y = -2.5;
        value.frame = "map";
        value.timestamp =
            std::uint64_t{123456};
        value.frame_kind =
            acme::navigation::FrameKind::Map;

        std::cout
            << to_hex(cito::encode(value))
            << "\n";
        return 0;
    }

    if (argc == 3 &&
        std::string(argv[1]) == "decode") {
        const auto bytes =
            from_hex(argv[2]);
        const auto value =
            cito::decode<
                acme::navigation::Position>(
                bytes);

        assert(value.x == 3.5);
        assert(value.y == -7.25);
        assert(value.frame == "odom");
        assert(!value.timestamp.has_value());
        assert(
            value.frame_kind ==
            acme::navigation::FrameKind::Odom);

        std::cout << "ok\n";
        return 0;
    }

    return 2;
}
