#include <cito/detail/request_network_adapter.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using Clock =
    cito::detail::
        RequestNetworkAdapter::Clock;

void add_route(
    cito::detail::HostRuntimeCore& host,
    const cito::detail::RequestKey& key,
    cito::DestinationId destination,
    std::uint64_t version) {
    const auto capability =
        cito::detail::
            request_handler_interest_key(
                key);

    if (!host.coordinator_is_current(
            50,
            5001)) {
        assert(
            host.observe_coordinator(
                50,
                5001,
                10000,
                0) ==
            cito::
                CoordinatorLeaseObservation::
                    Added);
    }

    const std::array<
        cito::InterestKey,
        1> keys{capability};

    assert(
        host.apply_remote_snapshot(
            50,
            {5001, version, 1},
            keys));

    const std::array<
        cito::DestinationId,
        1> destinations{
            destination};

    assert(
        host.replace_remote_routes(
            50,
            5001,
            capability,
            destinations));
}

} // namespace

int main() {
    using namespace cito::detail;

    const RequestKey fast{
        7,
        17,
        100};

    const RequestKey missing{
        7,
        18,
        100};

    const RequestKey slow{
        7,
        19,
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

    int ensure_calls = 0;
    RequestKey ensured{};

    client.set_wait_hook(
        [&](const RequestKey& key) {
            ++ensure_calls;
            ensured = key;
        });

    const auto now =
        Clock::time_point{} +
        std::chrono::seconds(1);

    int encodes = 0;
    int completions = 0;

    const auto wait_id =
        client.defer(
            1,
            fast,
            [&] {
                ++encodes;
                return
                    std::vector<
                        std::uint8_t>{
                        7};
            },
            [&](RequestStatus status,
                std::span<
                    const std::uint8_t>
                    payload) {
                ++completions;
                assert(
                    status ==
                    RequestStatus::Ok);
                assert(payload.size() == 1);
                assert(payload[0] == 8);
            },
            now +
                std::chrono::
                    milliseconds(100));

    assert(wait_id != 0);
    assert(ensure_calls == 1);
    assert(ensured == fast);
    assert(client.waiting_count() == 1);
    assert(encodes == 0);
    assert(completions == 0);

    // No route/session: progress is zero-work for serialization.
    assert(
        client.progress(
            now +
            std::chrono::
                milliseconds(10)) ==
        0);
    assert(encodes == 0);

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

    add_route(
        client_host,
        fast,
        77,
        1);

    assert(
        client.attach_session(
            77,
            [&](std::span<
                    const std::uint8_t>
                    bytes) {
                return
                    server.receive(
                        66,
                        bytes,
                        [&](std::span<
                                const std::uint8_t>
                                reply) {
                            return
                                client.receive(
                                    77,
                                    reply);
                        });
            }));

    assert(
        client.progress(
            now +
            std::chrono::
                milliseconds(20)) ==
        1);

    assert(encodes == 1);
    assert(completions == 1);
    assert(client.waiting_count() == 0);
    assert(
        client_requests
            .pending_count() == 0);

    // Missing route: deadline completes without calling the encoder.
    int deadline_encodes = 0;
    int deadlines = 0;

    const auto deadline_id =
        client.defer(
            1,
            missing,
            [&] {
                ++deadline_encodes;
                return
                    std::vector<
                        std::uint8_t>{
                        1};
            },
            [&](RequestStatus status,
                std::span<
                    const std::uint8_t>) {
                ++deadlines;
                assert(
                    status ==
                    RequestStatus::
                        DeadlineExceeded);
            },
            now +
                std::chrono::
                    milliseconds(30));

    assert(deadline_id != 0);
    assert(deadline_encodes == 0);

    assert(
        client.progress(
            now +
            std::chrono::
                milliseconds(30)) ==
        1);

    assert(deadlines == 1);
    assert(deadline_encodes == 0);
    assert(client.waiting_count() == 0);

    // Explicit cancellation before readiness also preserves encode-zero.
    int cancelled_encodes = 0;
    int cancelled = 0;

    const auto cancel_id =
        client.defer(
            1,
            missing,
            [&] {
                ++cancelled_encodes;
                return
                    std::vector<
                        std::uint8_t>{
                        2};
            },
            [&](RequestStatus status,
                std::span<
                    const std::uint8_t>) {
                ++cancelled;
                assert(
                    status ==
                    RequestStatus::
                        Cancelled);
            },
            now +
                std::chrono::seconds(1));

    assert(client.cancel_wait(cancel_id));
    assert(cancelled == 1);
    assert(cancelled_encodes == 0);
    assert(client.waiting_count() == 0);

    // A deadline after send emits Cancel and rejects the late handler reply.
    RequestReplyCore::Reply
        late_reply;

    server_requests.register_handler(
        3,
        slow,
        [&](RequestId,
            AppId,
            std::span<
                const std::uint8_t>,
            RequestReplyCore::Reply
                reply) {
            late_reply =
                std::move(reply);
        });

    add_route(
        client_host,
        slow,
        77,
        2);

    int slow_encodes = 0;
    int slow_deadlines = 0;

    const auto slow_id =
        client.defer(
            1,
            slow,
            [&] {
                ++slow_encodes;
                return
                    std::vector<
                        std::uint8_t>{
                        3};
            },
            [&](RequestStatus status,
                std::span<
                    const std::uint8_t>) {
                ++slow_deadlines;
                assert(
                    status ==
                    RequestStatus::
                        DeadlineExceeded);
            },
            now +
                std::chrono::
                    milliseconds(60));

    assert(slow_id != 0);

    assert(
        client.progress(
            now +
            std::chrono::
                milliseconds(40)) ==
        1);

    assert(slow_encodes == 1);
    assert(client.waiting_count() == 1);
    assert(
        client.transport()
            .outbound_count() == 1);
    assert(
        server.transport()
            .inbound_count() == 1);

    assert(
        client.progress(
            now +
            std::chrono::
                milliseconds(60)) ==
        1);

    assert(slow_deadlines == 1);
    assert(client.waiting_count() == 0);
    assert(
        client.transport()
            .outbound_count() == 0);
    assert(
        server.transport()
            .inbound_count() == 0);

    const std::array<
        std::uint8_t,
        1> late{9};

    assert(
        !late_reply(
            RequestStatus::Ok,
            late));

    // Manager shutdown-style failure completes waiting requests.
    int unavailable = 0;

    const auto shutdown_id =
        client.defer(
            4,
            missing,
            [] {
                return
                    std::vector<
                        std::uint8_t>{
                        4};
            },
            [&](RequestStatus status,
                std::span<
                    const std::uint8_t>) {
                ++unavailable;
                assert(
                    status ==
                    RequestStatus::
                        Unavailable);
            },
            now +
                std::chrono::seconds(2));

    assert(shutdown_id != 0);
    assert(
        client.fail_waiting() == 1);
    assert(unavailable == 1);
    assert(client.waiting_count() == 0);

    client.clear_wait_hook();

    int fail_fast = 0;

    const auto disabled =
        client.defer(
            5,
            missing,
            [] {
                return
                    std::vector<
                        std::uint8_t>{};
            },
            [&](RequestStatus status,
                std::span<
                    const std::uint8_t>) {
                ++fail_fast;
                assert(
                    status ==
                    RequestStatus::
                        Unavailable);
            },
            now +
                std::chrono::seconds(1));

    assert(disabled == 0);
    assert(fail_fast == 1);

    return 0;
}
