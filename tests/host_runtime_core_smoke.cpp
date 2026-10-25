#include <cito/detail/host_runtime_core.hpp>
#include <cito/identity.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <string_view>

int main() {
    cito::detail::HostRuntimeCore host;

    const auto scope =
        cito::stable_name_id("acme");

    const cito::InterestKey position{
        scope,
        cito::stable_name_id(
            "drone_17"),
        100};

    const cito::InterestKey battery{
        scope,
        cito::stable_name_id(
            "drone_17"),
        200};

    std::size_t position_delivered = 0;
    std::size_t battery_delivered = 0;

    host.register_app(
        10,
        [&](auto type,
            std::string_view,
            auto) {
            assert(type == 100);
            ++position_delivered;
            return std::size_t{1};
        });

    host.register_app(
        20,
        [&](auto type,
            std::string_view,
            auto) {
            assert(type == 200);
            ++battery_delivered;
            return std::size_t{1};
        });

    host.add_interest(
        10,
        position);
    host.add_interest(
        20,
        battery);

    assert(
        host.local_snapshot().size() ==
        2);

    const std::array<
        std::uint8_t,
        2> payload{9, 8};

    const auto position_result =
        host.publish(
            position,
            "drone_17",
            payload,
            [](auto, auto) {
                return true;
            });

    assert(
        position_result.local_delivered ==
        1);
    assert(position_delivered == 1);
    assert(battery_delivered == 0);

    const cito::InterestKey unrelated{
        scope,
        cito::stable_name_id(
            "drone_18"),
        100};

    const auto unrelated_result =
        host.publish(
            unrelated,
            "drone_18",
            payload,
            [](auto, auto) {
                return true;
            });

    assert(
        unrelated_result.delivered() ==
        0);
    assert(
        unrelated_result.packet_encodes ==
        0);

    return 0;
}
