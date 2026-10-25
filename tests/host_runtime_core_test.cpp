#include <citoq/detail/host_runtime_core.hpp>
#include <citoq/identity.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

int main() {
    assert(
        citoq::stable_name_id(
            "acme") ==
        514768821131075087ull);
    assert(
        citoq::stable_name_id(
            "drone_17") ==
        1308784374555480002ull);

    citoq::detail::HostRuntimeCore host;

    const citoq::InterestKey position{
        citoq::stable_name_id("acme"),
        citoq::stable_name_id("drone_17"),
        100};

    std::size_t app1_calls = 0;
    std::size_t app2_calls = 0;

    assert(host.register_app(
        1,
        [&](auto type,
            std::string_view resource,
            auto payload) {
            ++app1_calls;
            assert(type == 100);
            assert(resource == "drone_17");
            assert(payload.size() == 3);
            return std::size_t{2};
        }));

    assert(host.register_app(
        2,
        [&](auto,
            std::string_view,
            auto) {
            ++app2_calls;
            return std::size_t{1};
        }));

    const auto first =
        host.add_interest(
            1,
            position);
    assert(first.changed);
    assert(first.summary_changed);
    assert(
        host.local_summary_version() ==
        1);

    const auto duplicate =
        host.add_interest(
            1,
            position);
    assert(duplicate.changed);
    assert(!duplicate.summary_changed);
    assert(
        host.local_summary_version() ==
        1);

    assert(host.has_local_demand(
        position));
    assert(host.has_demand(
        position));

    int remote_calls = 0;

    const std::array<
        std::uint8_t,
        3> payload{1, 2, 3};

    auto result =
        host.publish(
            position,
            "drone_17",
            payload,
            [&](auto, auto) {
                ++remote_calls;
                return true;
            });

    assert(result.local_delivered == 2);
    assert(result.remote_sent == 0);
    assert(result.packet_encodes == 0);
    assert(app1_calls == 1);
    assert(app2_calls == 0);
    assert(remote_calls == 0);

    assert(
        host.observe_coordinator(
            7,
            1001,
            500,
            1000) ==
        citoq::CoordinatorLeaseObservation::
            Added);

    const std::array<
        citoq::InterestKey,
        1> keys{position};

    assert(host.apply_remote_snapshot(
        7,
        {1001, 1, 1},
        keys));

    assert(
        host.candidate_hosts(
            position)
            .contains(7));

    const std::array<
        citoq::DestinationId,
        2> destinations{70, 71};

    assert(host.replace_remote_routes(
        7,
        1001,
        position,
        destinations));

    std::vector<
        citoq::DestinationId> reached;

    result =
        host.publish(
            position,
            "drone_17",
            payload,
            [&](auto destination,
                auto packet) {
                const auto decoded =
                    citoq::
                        decode_data_packet_view(
                            packet);
                assert(
                    decoded.key ==
                    position);
                assert(
                    decoded.payload.size() ==
                    payload.size());
                reached.push_back(
                    destination);
                return true;
            });

    assert(result.local_delivered == 2);
    assert(result.remote_sent == 2);
    assert(result.packet_encodes == 1);
    assert(reached.size() == 2);

    assert(
        host.expire_remote(
            1499) ==
        0);
    assert(
        host.expire_remote(
            1500) ==
        1);
    assert(
        host.remote_destinations(
            position).empty());

    const auto remove_one =
        host.remove_interest(
            1,
            position);
    assert(remove_one.changed);
    assert(!remove_one.summary_changed);
    assert(host.has_local_demand(
        position));

    const auto remove_last =
        host.remove_interest(
            1,
            position);
    assert(remove_last.changed);
    assert(remove_last.summary_changed);
    assert(!host.has_demand(
        position));

    assert(host.unregister_app(1));
    assert(host.unregister_app(2));
    assert(host.app_count() == 0);

    // App destruction from inside delivery must be deferred until the
    // current callback returns.
    citoq::detail::HostRuntimeCore reentrant;
    const citoq::InterestKey self_key{
        9, 9, 9};

    reentrant.register_app(
        99,
        [&](auto, auto, auto) {
            assert(
                reentrant.unregister_app(
                    99));
            return std::size_t{1};
        });
    reentrant.add_interest(
        99,
        self_key);

    const std::array<
        std::uint8_t,
        1> one{1};

    const auto self_result =
        reentrant.publish(
            self_key,
            "self",
            one,
            [](auto, auto) {
                return false;
            });

    assert(
        self_result.local_delivered ==
        1);
    assert(
        reentrant.app_count() == 0);
    assert(
        !reentrant.has_local_demand(
            self_key));

    return 0;
}
