#include <cito/detail/request_network_adapter.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <span>
#include <vector>

int main() {
    using namespace cito::detail;

    const RequestKey fast{
        7,
        17,
        100};

    const RequestKey slow{
        7,
        18,
        100};

    HostRuntimeCore client_host;
    RequestReplyCore client_requests;
    RequestNetworkAdapter client(
        client_host,
        client_requests);

    HostRuntimeCore server_host;
    RequestReplyCore server_requests;
    RequestNetworkAdapter server(
        server_host,
        server_requests);

    assert(
        client_host.observe_coordinator(
            50,
            5001,
            10000,
            0) ==
        cito::
            CoordinatorLeaseObservation::
                Added);

    const std::array<
        cito::InterestKey,
        2> capabilities{
            request_handler_interest_key(
                fast),
            request_handler_interest_key(
                slow)};

    assert(
        client_host.apply_remote_snapshot(
            50,
            {5001, 1, 2},
            capabilities));

    const std::array<
        cito::DestinationId,
        2> destinations{
            88,
            77};

    for (const auto& capability :
         capabilities) {
        assert(
            client_host
                .replace_remote_routes(
                    50,
                    5001,
                    capability,
                    destinations));
    }

    server_requests.register_handler(
        2,
        fast,
        [](RequestId,
           AppId,
           std::span<
               const std::uint8_t>
               payload,
           RequestReplyCore::Reply
               reply) {
            assert(payload.size() == 1);

            const std::array<
                std::uint8_t,
                1> response{
                    static_cast<
                        std::uint8_t>(
                        payload[0] + 1)};

            assert(
                reply(
                    RequestStatus::Ok,
                    response));
        });

    RequestReplyCore::Reply
        slow_reply;

    server_requests.register_handler(
        3,
        slow,
        [&](RequestId,
            AppId,
            std::span<
                const std::uint8_t>,
            RequestReplyCore::Reply
                reply) {
            slow_reply =
                std::move(reply);
        });

    std::size_t sends_88 = 0;

    assert(
        client.attach_session(
            88,
            [&](std::span<
                    const std::uint8_t>
                    bytes) {
                ++sends_88;

                return server.receive(
                    66,
                    bytes,
                    [&](std::span<
                            const std::uint8_t>
                            reply) {
                        return client.receive(
                            88,
                            reply);
                    });
            }));

    assert(client.can_send(fast));
    assert(
        client.ready_destination(
            fast) ==
        std::optional<
            cito::DestinationId>(
                88));

    const std::array<
        std::uint8_t,
        1> request{7};

    int completions = 0;

    auto first =
        client.start(
            1,
            fast,
            request,
            [&](RequestStatus status,
                std::span<
                    const std::uint8_t>
                    payload) {
                ++completions;
                assert(
                    status ==
                    RequestStatus::Ok);
                assert(
                    payload.size() == 1);
                assert(payload[0] == 8);
            });

    assert(first.sent);
    assert(first.destination == 88);
    assert(sends_88 == 1);
    assert(completions == 1);
    assert(
        client_requests
            .pending_count() == 0);

    std::size_t sends_77 = 0;

    assert(
        client.attach_session(
            77,
            [&](std::span<
                    const std::uint8_t>
                    bytes) {
                ++sends_77;

                return server.receive(
                    66,
                    bytes,
                    [&](std::span<
                            const std::uint8_t>
                            reply) {
                        return client.receive(
                            77,
                            reply);
                    });
            }));

    // v1 policy chooses the smallest ready DestinationId.
    assert(
        client.ready_destination(
            fast) ==
        std::optional<
            cito::DestinationId>(
                77));

    auto second =
        client.start(
            1,
            fast,
            request,
            [&](RequestStatus status,
                std::span<
                    const std::uint8_t>
                    payload) {
                ++completions;
                assert(
                    status ==
                    RequestStatus::Ok);
                assert(payload[0] == 8);
            });

    assert(second.sent);
    assert(second.destination == 77);
    assert(sends_77 == 1);
    assert(completions == 2);

    assert(client.detach_session(77));
    assert(
        client.ready_destination(
            fast) ==
        std::optional<
            cito::DestinationId>(
                88));

    // A ready route with a session whose send fails completes Unavailable
    // and leaves no pending correlation.
    assert(
        !client.attach_session(
            88,
            [](std::span<
                   const std::uint8_t>) {
                return false;
            }));

    int unavailable = 0;

    auto failed =
        client.start(
            1,
            fast,
            request,
            [&](RequestStatus status,
                std::span<
                    const std::uint8_t>) {
                ++unavailable;
                assert(
                    status ==
                    RequestStatus::
                        Unavailable);
            });

    assert(!failed.sent);
    assert(unavailable == 1);
    assert(
        client.transport()
            .outbound_count() == 0);
    assert(
        client_requests
            .pending_count() == 0);

    // Reattach and leave a request outstanding to exercise connection loss.
    assert(
        client.attach_session(
            77,
            [&](std::span<
                    const std::uint8_t>
                    bytes) {
                return server.receive(
                    66,
                    bytes,
                    [&](std::span<
                            const std::uint8_t>
                            reply) {
                        return client.receive(
                            77,
                            reply);
                    });
            }));

    int lost = 0;

    auto pending =
        client.start(
            1,
            slow,
            request,
            [&](RequestStatus status,
                std::span<
                    const std::uint8_t>) {
                ++lost;
                assert(
                    status ==
                    RequestStatus::
                        Unavailable);
            });

    assert(pending.sent);
    assert(pending.destination == 77);
    assert(
        client.transport()
            .outbound_count() == 1);
    assert(
        server.transport()
            .inbound_count() == 1);

    assert(client.detach_session(77));
    assert(lost == 1);
    assert(
        client.transport()
            .outbound_count() == 0);

    // The server-side QUIC session close drops only that peer's inbound IDs.
    assert(server.drop_peer(66) == 1);
    assert(
        server.transport()
            .inbound_count() == 0);

    const std::array<
        std::uint8_t,
        1> late{9};

    assert(
        !slow_reply(
            RequestStatus::Ok,
            late));

    return 0;
}
