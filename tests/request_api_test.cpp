#include <cito/cito.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

struct Ping {
    std::uint8_t value{};
};

struct Pong {
    std::uint8_t value{};
};

namespace cito {

template <>
struct StaticType<Ping> {
    static Type type() {
        return TypeBuilder("tests.Ping")
            .member<std::uint32_t>(
                1,
                "value")
            .build();
    }
};

template <>
struct StaticType<Pong> {
    static Type type() {
        return TypeBuilder("tests.Pong")
            .member<std::uint32_t>(
                1,
                "value")
            .build();
    }
};

template <>
struct ReplyType<Ping> {
    using type = Pong;
};

template <>
struct StaticCodec<Ping> {
    static constexpr bool direct = true;
    static inline std::size_t encodes = 0;
    static inline std::size_t decodes = 0;

    static std::vector<std::uint8_t>
    encode(const Ping& value) {
        ++encodes;
        return {value.value};
    }

    static Ping decode(
        std::span<const std::uint8_t>
            bytes) {
        ++decodes;
        if (bytes.size() != 1) {
            throw std::invalid_argument(
                "bad Ping");
        }
        return Ping{bytes[0]};
    }
};

template <>
struct StaticCodec<Pong> {
    static constexpr bool direct = true;
    static inline std::size_t encodes = 0;
    static inline std::size_t decodes = 0;

    static std::vector<std::uint8_t>
    encode(const Pong& value) {
        ++encodes;
        return {value.value};
    }

    static Pong decode(
        std::span<const std::uint8_t>
            bytes) {
        ++decodes;
        if (bytes.size() != 1) {
            throw std::invalid_argument(
                "bad Pong");
        }
        return Pong{bytes[0]};
    }
};

} // namespace cito

int main() {
    using PingCodec =
        cito::StaticCodec<Ping>;
    using PongCodec =
        cito::StaticCodec<Pong>;

    cito::Context requester("acme");

    const cito::detail::RequestKey request_key{
        cito::stable_name_id("acme"),
        cito::stable_name_id("robot_1"),
        cito::static_type_ref<Ping>()
            .type_id()};

    const auto capability =
        cito::detail::
            request_handler_interest_key(
                request_key);

    const cito::InterestKey ordinary_key{
        request_key.scope,
        request_key.resource,
        request_key.request_type};

    auto runtime =
        cito::detail::
            acquire_embedded_host_runtime();

    cito::Context ordinary_subscriber(
        "acme");
    auto ordinary_subscription =
        ordinary_subscriber
            .subscribe<Ping>(
                "robot_1",
                [](const Ping&) {});

    assert(
        runtime->host()
            .has_local_demand(
                ordinary_key));
    assert(
        !runtime->host()
             .has_local_demand(
                 capability));

    PingCodec::encodes = 0;
    auto missing =
        requester.request(
            "robot_1",
            Ping{7});

    assert(missing.ready());
    assert(!missing.result());
    assert(
        missing.result().status() ==
        cito::RequestStatus::
            Unavailable);

    // No handler demand means no request serialization.
    assert(PingCodec::encodes == 0);

    auto missing_with_options =
        requester.request(
            "robot_1",
            Ping{8},
            cito::RequestOptions{
                std::chrono::
                    milliseconds(25)});

    assert(missing_with_options.ready());
    assert(!missing_with_options.result());
    assert(
        missing_with_options.result()
            .status() ==
        cito::RequestStatus::
            Unavailable);
    assert(PingCodec::encodes == 0);

    bool negative_timeout_rejected =
        false;

    try {
        (void)requester.request(
            "robot_1",
            Ping{9},
            cito::RequestOptions{
                std::chrono::
                    milliseconds(-1)});
    } catch (
        const std::invalid_argument&) {
        negative_timeout_rejected =
            true;
    }

    assert(negative_timeout_rejected);
    assert(PingCodec::encodes == 0);

    cito::Context provider("acme");

    auto provider_handler =
        provider.handle<Ping>(
            "robot_1",
            [](const Ping& ping) {
                return Pong{
                    static_cast<
                        std::uint8_t>(
                        ping.value + 1)};
            });

    assert(
        runtime->host()
            .has_local_demand(
                capability));

    PingCodec::encodes = 0;
    PingCodec::decodes = 0;
    PongCodec::encodes = 0;
    PongCodec::decodes = 0;

    auto present =
        requester.request(
            "robot_1",
            Ping{7});

    assert(present.ready());
    assert(present.result());
    assert(
        present.result()
            .value()
            .value == 8);

    assert(PingCodec::encodes == 1);
    assert(PingCodec::decodes == 1);
    assert(PongCodec::encodes == 1);
    assert(PongCodec::decodes == 1);

    provider_handler.close();

    assert(
        !runtime->host()
             .has_local_demand(
                 capability));
    assert(
        runtime->host()
            .has_local_demand(
                ordinary_key));

    ordinary_subscription.close();

    return 0;
}
