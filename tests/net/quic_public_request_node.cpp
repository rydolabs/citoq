#include <cito/cito.hpp>
#include <cito/cito_c.h>
#include <cito/detail/linux_request_runtime_driver.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

struct NetPing {
    std::uint32_t value{};
};

struct NetPong {
    std::uint32_t value{};
};

namespace cito {

template <>
struct StaticType<NetPing> {
    static Type type() {
        return TypeBuilder(
            "tests.NetPing")
            .member<std::uint32_t>(
                1,
                "value")
            .build();
    }
};

template <>
struct StaticType<NetPong> {
    static Type type() {
        return TypeBuilder(
            "tests.NetPong")
            .member<std::uint32_t>(
                1,
                "value")
            .build();
    }
};

template <>
struct ReplyType<NetPing> {
    using type = NetPong;
};

template <>
struct StaticCodec<NetPing> {
    static constexpr bool direct = true;
    static inline std::size_t encodes = 0;

    static std::vector<std::uint8_t>
    encode(const NetPing& value) {
        ++encodes;

        return {
            static_cast<std::uint8_t>(
                value.value & 0xffu),
            static_cast<std::uint8_t>(
                (value.value >> 8) &
                0xffu),
            static_cast<std::uint8_t>(
                (value.value >> 16) &
                0xffu),
            static_cast<std::uint8_t>(
                (value.value >> 24) &
                0xffu)};
    }

    static NetPing decode(
        std::span<const std::uint8_t>
            bytes) {
        if (bytes.size() != 4) {
            throw std::invalid_argument(
                "bad NetPing");
        }

        return NetPing{
            static_cast<std::uint32_t>(
                bytes[0]) |
            (static_cast<std::uint32_t>(
                 bytes[1]) << 8) |
            (static_cast<std::uint32_t>(
                 bytes[2]) << 16) |
            (static_cast<std::uint32_t>(
                 bytes[3]) << 24)};
    }
};

template <>
struct StaticCodec<NetPong> {
    static constexpr bool direct = true;

    static std::vector<std::uint8_t>
    encode(const NetPong& value) {
        return {
            static_cast<std::uint8_t>(
                value.value & 0xffu),
            static_cast<std::uint8_t>(
                (value.value >> 8) &
                0xffu),
            static_cast<std::uint8_t>(
                (value.value >> 16) &
                0xffu),
            static_cast<std::uint8_t>(
                (value.value >> 24) &
                0xffu)};
    }

    static NetPong decode(
        std::span<const std::uint8_t>
            bytes) {
        if (bytes.size() != 4) {
            throw std::invalid_argument(
                "bad NetPong");
        }

        return NetPong{
            static_cast<std::uint32_t>(
                bytes[0]) |
            (static_cast<std::uint32_t>(
                 bytes[1]) << 8) |
            (static_cast<std::uint32_t>(
                 bytes[2]) << 16) |
            (static_cast<std::uint32_t>(
                 bytes[3]) << 24)};
    }
};

} // namespace cito

namespace {

std::uint32_t parse_ipv4(
    const std::string& text) {
    unsigned a = 0;
    unsigned b = 0;
    unsigned c = 0;
    unsigned d = 0;

    if (
        std::sscanf(
            text.c_str(),
            "%u.%u.%u.%u",
            &a,
            &b,
            &c,
            &d) != 4 ||
        a > 255 ||
        b > 255 ||
        c > 255 ||
        d > 255) {
        throw std::invalid_argument(
            "invalid IPv4 address");
    }

    return
        (a << 24) |
        (b << 16) |
        (c << 8) |
        d;
}

struct Detached {
    struct promise_type {
        Detached get_return_object() {
            return {};
        }

        std::suspend_never
        initial_suspend()
            const noexcept {
            return {};
        }

        std::suspend_never
        final_suspend()
            const noexcept {
            return {};
        }

        void return_void()
            const noexcept {}

        void unhandled_exception() {
            std::terminate();
        }
    };
};

Detached await_and_stop(
    cito::Context& ctx,
    cito::Request<NetPong> request,
    bool& done,
    bool& ok,
    std::uint32_t& value,
    cito::RequestStatus& status) {
    auto result =
        co_await request;

    status =
        result.status();

    if (result) {
        ok = true;
        value =
            result.value().value;
    }

    done = true;
    ctx.stop();
}

struct CDataCheck {
    std::atomic<int>* seen_count{};
    std::atomic<int>* rejected{};
    std::vector<std::uint8_t>* seen_ids{};
    int expected{};
};

void c_typed_callback(void* opaque, std::uint64_t,
                      const char*, std::size_t,
                      const std::uint8_t* bytes, std::size_t size) {
    auto& s = *static_cast<CDataCheck*>(opaque);
    if (!bytes || size != 4) {
        s.rejected->fetch_add(1);
        return;
    }
    const auto seq = static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
    if (seq == 0 || seq > static_cast<std::uint32_t>(s.expected) ||
        (*s.seen_ids)[seq]++) {
        s.rejected->fetch_add(1);
        return;
    }
    s.seen_count->fetch_add(1);
}

int data_count() {
    const char* value = std::getenv("CITO_PUBSUB_COUNT");
    if (!value) return 10;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end || parsed < 1 || parsed > 1000)
        throw std::invalid_argument("CITO_PUBSUB_COUNT must be 1..1000");
    return static_cast<int>(parsed);
}

int run_server(
    std::uint16_t quic_port,
    std::uint16_t control_port,
    const std::string&
        announce_ip,
    std::uint16_t announce_port,
    const std::string& cert,
    const std::string& key,
    const char* client_ca_file) {
    using namespace cito::detail;

    auto runtime =
        acquire_embedded_host_runtime();

    LinuxRequestRuntimeOptions
        options;

    options.coordinator = 50;
    options.incarnation = 5001;
    options.destination = 77;
    options.control_port =
        control_port;
    options.quic_port =
        quic_port;
    options.lease_ms = 700;
    options.announce_interval_ms =
        100;
    options.announcement_targets
        .push_back(
            UdpEndpoint{
                parse_ipv4(
                    announce_ip),
                announce_port});
    options.certificate_file =
        cert;
    options.private_key_file =
        key;
    options.quic.registration_name =
        "cito-public-server";
    if (client_ca_file) {
        options.quic.require_client_certificate = true;
        options.quic.trusted_client_ca_file = client_ca_file;
    }

    auto driver =
        install_linux_request_runtime(
            *runtime,
            std::move(options));

    cito::Context provider(
        "acme");

    std::atomic<int>
        handler_calls{0};

    provider.handle<NetPing>(
        "robot_1",
        [&](const NetPing& ping) {
            handler_calls.fetch_add(
                1,
                std::memory_order_relaxed);

            return NetPong{
                ping.value + 1};
        });

    const bool pubsub_mode = std::getenv("CITO_PUBSUB_SMOKE") != nullptr;
    const int expected_data = pubsub_mode ? data_count() : 0;
    const bool c_api_mode = std::getenv("CITO_C_API_DATA") != nullptr;
    std::atomic<int> data_seen{0};
    std::atomic<int> data_rejected{0};
    std::vector<std::uint8_t> seen_sequences(static_cast<std::size_t>(expected_data)+1);
    cito::Subscription data_subscription;
    cito_context_t* c_subscriber = nullptr;
    cito_subscription_t* c_subscription = nullptr;
    CDataCheck c_check{&data_seen, &data_rejected, &seen_sequences, expected_data};
    if (pubsub_mode && c_api_mode) {
        if (cito_context_create("acme", 4, &c_subscriber) != CITO_STATUS_OK)
            return 11;
        if (cito_context_subscribe(c_subscriber,
            cito::static_type_ref<NetPing>().type_id(),
            "robot_1", 7, c_typed_callback, &c_check,
            &c_subscription) != CITO_STATUS_OK) return 12;
    } else if (pubsub_mode) {
        data_subscription = provider.on<NetPing>(
            "robot_1", [&](const NetPing& ping) {
                if (ping.value == 0 || ping.value > static_cast<std::uint32_t>(expected_data) ||
                    seen_sequences[ping.value]++) {
                    data_rejected.fetch_add(1, std::memory_order_relaxed);
                } else {
                    data_seen.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }

    std::atomic<bool>
        completed{false};

    std::thread stopper(
        [&] {
            const auto deadline =
                std::chrono::
                    steady_clock::now() +
                std::chrono::
                    seconds(pubsub_mode ? 25 : 10);

            while (
                std::chrono::
                    steady_clock::now() <
                    deadline) {
                if (
                    handler_calls.load(
                        std::memory_order_relaxed) ==
                        1 &&
                    (!pubsub_mode ||
                     data_seen.load(std::memory_order_relaxed) >= expected_data) &&
                    driver->
                        completed_send_count() >=
                        1 &&
                    driver->peer_count() ==
                        0) {
                    completed.store(
                        true,
                        std::memory_order_release);
                    provider.stop();
                    return;
                }

                std::this_thread::
                    sleep_for(
                        std::chrono::
                            milliseconds(1));
            }

            provider.stop();
        });

    provider.run();
    stopper.join();

    const auto stats =
        driver->control_stats();

    const bool ok =
        completed.load(
            std::memory_order_acquire) &&
        handler_calls.load(
            std::memory_order_relaxed) ==
            1 &&
        (!pubsub_mode ||
         (data_seen.load(std::memory_order_relaxed) == expected_data &&
          data_rejected.load(std::memory_order_relaxed) == 0)) &&
        stats.datagrams_received >=
            2 &&
        stats.datagrams_sent >= 2;

    if (c_subscription) cito_subscription_destroy(c_subscription);
    if (c_subscriber) cito_context_destroy(c_subscriber);
    (void)runtime->clear_driver(
        driver.get());
    driver.reset();

    if (!ok) {
        return 10;
    }

    std::cout
        << "quic_public_request_server handler=1 reply=1 discovery=udp run=context"
        << (pubsub_mode ? " pubsub_received=" + std::to_string(expected_data) : "")
        << (c_api_mode ? " c_api=1" : "")
        << std::endl;

    return 0;
}

int run_client(
    std::uint16_t control_port,
    const char* ca_file,
    const char* client_cert,
    const char* client_key) {
    using namespace cito::detail;

    auto runtime =
        acquire_embedded_host_runtime();

    LinuxRequestRuntimeOptions
        options;

    options.coordinator = 20;
    options.incarnation = 2001;
    options.control_port =
        control_port;
    options.quic.registration_name =
        "cito-public-client";
    options.quic.ca_certificate_file = ca_file;
    if (client_cert && client_key) {
        options.quic.client_certificate_file = client_cert;
        options.quic.client_private_key_file = client_key;
    }

    auto driver =
        install_linux_request_runtime(
            *runtime,
            std::move(options));

    cito::Context requester(
        "acme");

    cito::StaticCodec<
        NetPing>::encodes = 0;

    auto request =
        requester.request(
            "robot_1",
            NetPing{41},
            cito::RequestOptions{
                std::chrono::
                    seconds(5)});

    if (
        request.ready() ||
        cito::StaticCodec<
            NetPing>::encodes != 0 ||
        runtime->request_network()
            .waiting_count() != 1) {
        return 20;
    }

    bool done = false;
    bool ok = false;
    std::uint32_t value = 0;
    auto status =
        cito::RequestStatus::
            Internal;

    await_and_stop(
        requester,
        request,
        done,
        ok,
        value,
        status);

    requester.run();

    if (
        !done ||
        !ok ||
        status !=
            cito::RequestStatus::Ok ||
        value != 42 ||
        cito::StaticCodec<
            NetPing>::encodes != 1 ||
        driver->session_count() !=
            1) {
        return 21;
    }

    const auto before_timeout =
        cito::StaticCodec<
            NetPing>::encodes;

    auto timed_out =
        requester.request(
            "missing",
            NetPing{9},
            cito::RequestOptions{
                std::chrono::
                    milliseconds(20)});

    if (
        timed_out.ready() ||
        cito::StaticCodec<
            NetPing>::encodes !=
        before_timeout) {
        return 22;
    }

    bool timeout_done = false;
    bool timeout_ok = false;
    std::uint32_t timeout_value =
        0;
    auto timeout_status =
        cito::RequestStatus::
            Internal;

    await_and_stop(
        requester,
        timed_out,
        timeout_done,
        timeout_ok,
        timeout_value,
        timeout_status);

    requester.run();

    const auto stats =
        driver->control_stats();

    const bool final_ok =
        timeout_done &&
        !timeout_ok &&
        timeout_status ==
            cito::RequestStatus::
                DeadlineExceeded &&
        cito::StaticCodec<
            NetPing>::encodes ==
            before_timeout &&
        stats.datagrams_received >=
            3 &&
        stats.datagrams_sent >= 2 &&
        runtime->request_network()
            .waiting_count() == 0 &&
        runtime->request_network()
            .transport()
            .outbound_count() == 0 &&
        runtime->requests()
            .pending_count() == 0;

    const bool pubsub_mode = std::getenv("CITO_PUBSUB_SMOKE") != nullptr;
    const int expected_data = pubsub_mode ? data_count() : 0;
    const bool c_api_mode = std::getenv("CITO_C_API_DATA") != nullptr;
    bool pubsub_ok = true;
    if (pubsub_mode) {
        const cito::InterestKey data_key{
            cito::stable_name_id("acme"),
            cito::stable_name_id("robot_1"),
            cito::static_type_ref<NetPing>().type_id()};
        cito_context_t* c_publisher = nullptr;
        if (c_api_mode) {
            if (cito_context_create("acme", 4, &c_publisher) != CITO_STATUS_OK)
                return 25;
            if (cito_context_advertise(c_publisher,
                    cito::static_type_ref<NetPing>().type_id(),
                    "robot_1", 7) != CITO_STATUS_OK)
                return 26;
        } else {
            requester.advertise<NetPing>("robot_1");
        }
        bool routed = false;
        for (int retry = 0; retry < 150; ++retry) {
            driver->poll_once();
            if (!runtime->host().remote_destinations(data_key).empty()) {
                routed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        pubsub_ok = routed;
        if (routed) {
            for (int i = 1; i <= expected_data; ++i) {
                if (c_api_mode) {
                    const std::array<std::uint8_t, 4> payload{
                        static_cast<std::uint8_t>(i & 255),
                        static_cast<std::uint8_t>((i >> 8) & 255),
                        static_cast<std::uint8_t>((i >> 16) & 255),
                        static_cast<std::uint8_t>((i >> 24) & 255)};
                    std::size_t sent = 0;
                    if (cito_context_publish(c_publisher,
                            cito::static_type_ref<NetPing>().type_id(),
                            "robot_1", 7, payload.data(), payload.size(),
                            &sent) != CITO_STATUS_OK || sent < 1)
                        pubsub_ok = false;
                } else if (requester.publish("robot_1", NetPing{
                        static_cast<std::uint32_t>(i)}) < 1) {
                    pubsub_ok = false;
                }
                driver->poll_once();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            for (int i = 0; i < 100; ++i) {
                driver->poll_once();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        if (!pubsub_ok)
            std::cerr << "G1 data smoke: routed=" << routed
                      << " remote_destinations="
                      << runtime->host().remote_destinations(data_key).size()
                      << " session_count=" << driver->session_count()
                      << " send/route failed" << std::endl;
        // Advertised intent is owned by the public Context.
        if (c_publisher) cito_context_destroy(c_publisher);
    }

    (void)runtime->clear_driver(
        driver.get());
    driver.reset();

    if (!final_ok || !pubsub_ok) {
        return 23;
    }

    std::cout
        << "quic_public_request_client result=42 lazy=1 deadline=1 discovery=udp run=context"
        << (pubsub_mode ? " pubsub_sent=" + std::to_string(expected_data) : "")
        << (c_api_mode ? " c_api=1" : "")
        << std::endl;

    return 0;
}

} // namespace

int main(
    int argc,
    char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "type-id") == 0) {
        std::cout << cito::static_type_ref<NetPing>().type_id() << std::endl;
        return 0;
    }
    if (
        argc >= 2 &&
        std::strcmp(
            argv[1],
            "server") == 0) {
        if (argc != 8 && argc != 9) {
            return 2;
        }

        return run_server(
            static_cast<
                std::uint16_t>(
                std::stoul(
                    argv[2])),
            static_cast<
                std::uint16_t>(
                std::stoul(
                    argv[3])),
            argv[4],
            static_cast<
                std::uint16_t>(
                std::stoul(
                    argv[5])),
            argv[6],
            argv[7],
            argc == 9 ? argv[8] : nullptr);
    }

    if (
        argc >= 2 &&
        std::strcmp(
            argv[1],
            "client") == 0) {
        if (argc != 4 && argc != 6) {
            return 2;
        }

        return run_client(
            static_cast<
                std::uint16_t>(
                std::stoul(
                    argv[2])),
            argv[3],
            argc == 6 ? argv[4] : nullptr,
            argc == 6 ? argv[5] : nullptr);
    }

    return 2;
}
