#include <citoq/citoq.hpp>

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

namespace citoq {

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

} // namespace citoq

int main() {
    using PingCodec =
        citoq::StaticCodec<Ping>;
    using PongCodec =
        citoq::StaticCodec<Pong>;

    citoq::Context requester("acme");

    const citoq::detail::RequestKey request_key{
        citoq::stable_name_id("acme"),
        citoq::stable_name_id("robot_1"),
        citoq::static_type_ref<Ping>()
            .type_id()};

    const auto capability =
        citoq::detail::
            request_handler_interest_key(
                request_key);

    const citoq::InterestKey ordinary_key{
        request_key.scope,
        request_key.resource,
        request_key.request_type};

    auto runtime =
        citoq::detail::
            acquire_embedded_host_runtime();

    citoq::Context ordinary_subscriber(
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
        citoq::RequestStatus::
            Unavailable);

    // No handler demand means no request serialization.
    assert(PingCodec::encodes == 0);

    auto missing_with_options =
        requester.request(
            "robot_1",
            Ping{8},
            citoq::RequestOptions{
                std::chrono::
                    milliseconds(25)});

    assert(missing_with_options.ready());
    assert(!missing_with_options.result());
    assert(
        missing_with_options.result()
            .status() ==
        citoq::RequestStatus::
            Unavailable);
    assert(PingCodec::encodes == 0);

    bool negative_timeout_rejected =
        false;

    try {
        (void)requester.request(
            "robot_1",
            Ping{9},
            citoq::RequestOptions{
                std::chrono::
                    milliseconds(-1)});
    } catch (
        const std::invalid_argument&) {
        negative_timeout_rejected =
            true;
    }

    assert(negative_timeout_rejected);
    assert(PingCodec::encodes == 0);

    citoq::Context provider("acme");

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
