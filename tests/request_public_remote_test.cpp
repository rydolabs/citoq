#include <cito/cito.hpp>
#include <cito/detail/embedded_host_runtime.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

struct RemotePing {
    std::uint8_t value{};
};

struct RemotePong {
    std::uint8_t value{};
};

namespace cito {

template <>
struct StaticType<RemotePing> {
    static Type type() {
        return TypeBuilder(
            "tests.RemotePing")
            .member<std::uint32_t>(
                1,
                "value")
            .build();
    }
};

template <>
struct StaticType<RemotePong> {
    static Type type() {
        return TypeBuilder(
            "tests.RemotePong")
            .member<std::uint32_t>(
                1,
                "value")
            .build();
    }
};

template <>
struct ReplyType<RemotePing> {
    using type = RemotePong;
};

template <>
struct StaticCodec<RemotePing> {
    static constexpr bool direct = true;
    static inline std::size_t encodes = 0;
    static inline std::size_t decodes = 0;

    static std::vector<std::uint8_t>
    encode(const RemotePing& value) {
        ++encodes;
        return {value.value};
    }

    static RemotePing decode(
        std::span<const std::uint8_t>
            bytes) {
        ++decodes;

        if (bytes.size() != 1) {
            throw std::invalid_argument(
                "bad RemotePing");
        }

        return RemotePing{
            bytes[0]};
    }
};

template <>
struct StaticCodec<RemotePong> {
    static constexpr bool direct = true;
    static inline std::size_t encodes = 0;
    static inline std::size_t decodes = 0;

    static std::vector<std::uint8_t>
    encode(const RemotePong& value) {
        ++encodes;
        return {value.value};
    }

    static RemotePong decode(
        std::span<const std::uint8_t>
            bytes) {
        ++decodes;

        if (bytes.size() != 1) {
            throw std::invalid_argument(
                "bad RemotePong");
        }

        return RemotePong{
            bytes[0]};
    }
};

} // namespace cito

namespace {

cito::detail::RequestKey
request_key(
    std::string_view resource) {
    return {
        cito::stable_name_id(
            "acme"),
        cito::stable_name_id(
            resource),
        cito::static_type_ref<
            RemotePing>()
            .type_id()};
}

void add_remote_routes(
    cito::detail::EmbeddedHostRuntime&
        runtime,
    std::span<
        const cito::detail::RequestKey>
        keys) {
    assert(
        runtime.host()
            .observe_coordinator(
                50,
                5001,
                10000,
                0) ==
        cito::
            CoordinatorLeaseObservation::
                Added);

    std::vector<cito::InterestKey>
        capabilities;

    capabilities.reserve(
        keys.size());

    for (const auto& key : keys) {
        capabilities.push_back(
            cito::detail::
                request_handler_interest_key(
                    key));
    }

    assert(
        runtime.host()
            .apply_remote_snapshot(
                50,
                {5001,
                 1,
                 static_cast<
                     std::uint32_t>(
                     capabilities.size())},
                capabilities));

    const std::array<
        cito::DestinationId,
        1> destination{77};

    for (const auto& capability :
         capabilities) {
        assert(
            runtime.host()
                .replace_remote_routes(
                    50,
                    5001,
                    capability,
                    destination));
    }
}

} // namespace

int main() {
    using namespace cito::detail;

    auto client =
        acquire_embedded_host_runtime();

    EmbeddedHostRuntime server;

    const std::array<
        RequestKey,
        3> keys{
            request_key("robot_1"),
            request_key("slow"),
            request_key("destroy")};

    add_remote_routes(
        *client,
        keys);

    RequestReplyCore::Reply slow_reply;
    RequestReplyCore::Reply destroy_reply;

    server.requests()
        .register_handler(
            200,
            keys[0],
            [](RequestId,
               AppId requester,
               std::span<
                   const std::uint8_t>
                   payload,
               RequestReplyCore::Reply
                   reply) {
                assert(requester == 0);

                const auto ping =
                    cito::StaticCodec<
                        RemotePing>::
                        decode(
                            payload);

                const auto bytes =
                    cito::StaticCodec<
                        RemotePong>::
                        encode(
                            RemotePong{
                                static_cast<
                                    std::uint8_t>(
                                    ping.value +
                                    1)});

                assert(
                    reply(
                        RequestStatus::Ok,
                        bytes));
            });

    server.requests()
        .register_handler(
            201,
            keys[1],
            [&](RequestId,
                AppId,
                std::span<
                    const std::uint8_t>,
                RequestReplyCore::Reply
                    reply) {
                slow_reply =
                    std::move(reply);
            });

    server.requests()
        .register_handler(
            202,
            keys[2],
            [&](RequestId,
                AppId,
                std::span<
                    const std::uint8_t>,
                RequestReplyCore::Reply
                    reply) {
                destroy_reply =
                    std::move(reply);
            });

    std::size_t session_sends = 0;

    auto attach =
        [&] {
            assert(
                client->request_network()
                    .attach_session(
                        77,
                        [&](std::span<
                                const std::uint8_t>
                                bytes) {
                            ++session_sends;

                            return server
                                .request_network()
                                .receive(
                                    66,
                                    bytes,
                                    [&](std::span<
                                            const std::uint8_t>
                                            reply) {
                                        return client->
                                            request_network()
                                            .receive(
                                                77,
                                                reply);
                                    });
                        }));
        };

    attach();

    cito::Context requester(
        "acme");

    cito::StaticCodec<
        RemotePing>::encodes = 0;
    cito::StaticCodec<
        RemotePing>::decodes = 0;
    cito::StaticCodec<
        RemotePong>::encodes = 0;
    cito::StaticCodec<
        RemotePong>::decodes = 0;

    auto remote =
        requester.request(
            "robot_1",
            RemotePing{7});

    assert(remote.ready());
    assert(remote.result());
    assert(
        remote.result()
            .value()
            .value == 8);
    assert(session_sends == 1);
    assert(
        cito::StaticCodec<
            RemotePing>::encodes ==
        1);
    assert(
        cito::StaticCodec<
            RemotePing>::decodes ==
        1);
    assert(
        cito::StaticCodec<
            RemotePong>::encodes ==
        1);
    assert(
        cito::StaticCodec<
            RemotePong>::decodes ==
        1);

    // No exact remote route means no request serialization.
    const auto before_missing =
        cito::StaticCodec<
            RemotePing>::encodes;

    auto missing =
        requester.request(
            "robot_2",
            RemotePing{9});

    assert(missing.ready());
    assert(!missing.result());
    assert(
        missing.result()
            .status() ==
        cito::RequestStatus::
            Unavailable);
    assert(
        cito::StaticCodec<
            RemotePing>::encodes ==
        before_missing);
    assert(session_sends == 1);

    // A discovered route without a ready session also stays encode-zero.
    assert(
        client->request_network()
            .detach_session(
                77));

    const auto before_no_session =
        cito::StaticCodec<
            RemotePing>::encodes;

    auto no_session =
        requester.request(
            "robot_1",
            RemotePing{10});

    assert(no_session.ready());
    assert(!no_session.result());
    assert(
        no_session.result()
            .status() ==
        cito::RequestStatus::
            Unavailable);
    assert(
        cito::StaticCodec<
            RemotePing>::encodes ==
        before_no_session);

    attach();

    // A same-process handler wins over a ready remote session.
    cito::Context local_provider(
        "acme");

    auto local_handler =
        local_provider
            .handle<RemotePing>(
                "robot_1",
                [](const RemotePing&) {
                    return RemotePong{
                        99};
                });

    const auto sends_before_local =
        session_sends;

    auto local =
        requester.request(
            "robot_1",
            RemotePing{1});

    assert(local.ready());
    assert(local.result());
    assert(
        local.result()
            .value()
            .value == 99);
    assert(
        session_sends ==
        sends_before_local);

    local_handler.close();

    // Remote cancellation emits CTX1 Cancel and suppresses a late reply.
    auto slow =
        requester.request(
            "slow",
            RemotePing{3});

    assert(!slow.ready());
    assert(
        client->request_network()
            .transport()
            .outbound_count() == 1);
    assert(
        server.request_network()
            .transport()
            .inbound_count() == 1);

    assert(slow.cancel());
    assert(slow.ready());
    assert(!slow.result());
    assert(
        slow.result()
            .status() ==
        cito::RequestStatus::
            Cancelled);
    assert(
        client->request_network()
            .transport()
            .outbound_count() == 0);
    assert(
        server.request_network()
            .transport()
            .inbound_count() == 0);

    const std::array<
        std::uint8_t,
        1> late{42};

    assert(
        !slow_reply(
            RequestStatus::Ok,
            late));

    // Destroying a Context cancels only its outstanding remote requests and
    // leaves the shared runtime/session usable by other Contexts.
    std::optional<
        cito::Request<RemotePong>>
        outstanding;

    {
        cito::Context temporary(
            "acme");

        outstanding.emplace(
            temporary.request(
                "destroy",
                RemotePing{4}));

        assert(
            !outstanding->ready());
        assert(
            client->request_network()
                .transport()
                .outbound_count() ==
            1);
        assert(
            server.request_network()
                .transport()
                .inbound_count() ==
            1);
    }

    assert(outstanding->ready());
    assert(!outstanding->result());
    assert(
        outstanding->result()
            .status() ==
        cito::RequestStatus::
            Cancelled);
    assert(
        client->request_network()
            .transport()
            .outbound_count() == 0);
    assert(
        server.request_network()
            .transport()
            .inbound_count() == 0);

    assert(
        !destroy_reply(
            RequestStatus::Ok,
            late));

    // The original requester still uses the same shared session afterwards.
    auto final =
        requester.request(
            "robot_1",
            RemotePing{11});

    assert(final.ready());
    assert(final.result());
    assert(
        final.result()
            .value()
            .value == 12);

    assert(
        client->request_network()
            .detach_session(
                77));

    return 0;
}
