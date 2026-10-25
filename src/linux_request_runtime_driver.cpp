#include <cito/detail/linux_request_runtime_driver.hpp>

#include <cerrno>
#include <chrono>
#include <climits>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

namespace cito::detail {

namespace {

bool set_nonblocking(
    int fd) noexcept {
    const auto flags =
        ::fcntl(
            fd,
            F_GETFL,
            0);

    return
        flags >= 0 &&
        ::fcntl(
            fd,
            F_SETFL,
            flags | O_NONBLOCK) ==
            0;
}

int duration_timeout_ms(
    RequestNetworkAdapter::Clock::
        duration duration) noexcept {
    if (
        duration <=
        RequestNetworkAdapter::Clock::
            duration::zero()) {
        return 0;
    }

    auto millis =
        std::chrono::
            duration_cast<
                std::chrono::
                    milliseconds>(
                duration);

    if (
        millis <
        std::chrono::
            milliseconds(1)) {
        millis =
            std::chrono::
                milliseconds(1);
    }

    if (
        millis.count() >
        INT_MAX) {
        return INT_MAX;
    }

    return static_cast<int>(
        millis.count());
}

} // namespace

LinuxRequestRuntimeDriver::
LinuxRequestRuntimeDriver(
    EmbeddedHostRuntime& runtime,
    LinuxRequestRuntimeOptions options)
    : runtime_(runtime),
      options_(std::move(options)),
      routes_(
          runtime_.host(),
          options_.coordinator,
          options_.incarnation),
      lifecycle_(
          runtime_.host(),
          routes_),
      control_(
          routes_,
          &lifecycle_),
      quic_(
          runtime_,
          &lifecycle_,
          options_.quic) {
    if (
        options_.coordinator == 0 ||
        options_.incarnation == 0) {
        throw std::invalid_argument(
            "Cito Linux request runtime requires coordinator/incarnation");
    }

    int wake_fds[2]{
        -1,
        -1};

    if (
        ::pipe(wake_fds) != 0 ||
        !set_nonblocking(
            wake_fds[0]) ||
        !set_nonblocking(
            wake_fds[1])) {
        if (wake_fds[0] >= 0) {
            ::close(wake_fds[0]);
        }
        if (wake_fds[1] >= 0) {
            ::close(wake_fds[1]);
        }

        throw std::runtime_error(
            "Cito Linux request runtime wake pipe failed");
    }

    wake_read_ =
        wake_fds[0];
    wake_write_ =
        wake_fds[1];

    if (!control_.bind(
            options_.control_port)) {
        throw std::runtime_error(
            "Cito Linux request runtime control bind failed");
    }

    control_.set_announcement_policy(
        options_.lease_ms,
        options_.announce_interval_ms);

    for (const auto target :
         options_.announcement_targets) {
        control_.add_announcement_target(
            target);
    }

    if (
        options_.destination != 0 &&
        options_.quic_port != 0) {
        const std::array<
            cito::RouteEndpoint,
            1> endpoints{{
                {
                    options_.destination,
                    0,
                    options_.quic_port}}};

        routes_.set_local_route_endpoints(
            endpoints);
    }

    if (options_.quic_port != 0) {
        if (
            options_.certificate_file.empty() ||
            options_.private_key_file.empty() ||
            !quic_.start_listener(
                options_.quic_port,
                options_.certificate_file,
                options_.private_key_file)) {
            throw std::runtime_error(
                "Cito Linux request runtime QUIC listener failed");
        }
    }

    quic_.set_wake_callback(
        [this] {
            wake();
        });
}

LinuxRequestRuntimeDriver::
~LinuxRequestRuntimeDriver() {
    stop();

    quic_.set_wake_callback({});

    if (running_.load(
            std::memory_order_acquire)) {
        // Destruction while another thread owns run() is an integration
        // error. The normal EmbeddedHostRuntime owns the driver until run()
        // has returned, so only make a best effort here.
        wake();
    }

    quic_.shutdown();
    control_.stop();

    if (wake_read_ >= 0) {
        ::close(wake_read_);
        wake_read_ = -1;
    }

    if (wake_write_ >= 0) {
        ::close(wake_write_);
        wake_write_ = -1;
    }
}

bool LinuxRequestRuntimeDriver::ensure_data(const InterestKey& key) {
    return lifecycle_.ensure_data(key);
}

bool LinuxRequestRuntimeDriver::release_data(const InterestKey& key) {
    return lifecycle_.release_data(key);
}

void
LinuxRequestRuntimeDriver::run() {
    bool expected = false;

    if (!running_.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel)) {
        throw std::logic_error(
            "Cito runtime loop is already running");
    }

    stop_requested_.store(
        false,
        std::memory_order_release);

    try {
        while (!stop_requested_.load(
                    std::memory_order_acquire)) {
            (void)poll_once();

            if (stop_requested_.load(
                    std::memory_order_acquire)) {
                break;
            }

            wait_for_event();
        }

        (void)poll_once();
    } catch (...) {
        running_.store(
            false,
            std::memory_order_release);
        throw;
    }

    running_.store(
        false,
        std::memory_order_release);
}

void
LinuxRequestRuntimeDriver::stop()
    noexcept {
    stop_requested_.store(
        true,
        std::memory_order_release);
    wake();
}

std::size_t
LinuxRequestRuntimeDriver::
poll_once() {
    std::size_t work = 0;

    work +=
        control_.poll(
            now_ms());

    work +=
        quic_.reconcile();

    work +=
        quic_.poll();

    // A QUIC completion can change connection state and make another
    // lifecycle action immediately eligible.
    work +=
        quic_.reconcile();

    return work;
}

bool
LinuxRequestRuntimeDriver::running()
    const noexcept {
    return running_.load(
        std::memory_order_acquire);
}

std::uint16_t
LinuxRequestRuntimeDriver::
control_port()
    const noexcept {
    return control_.control_port();
}

std::size_t
LinuxRequestRuntimeDriver::
session_count()
    const noexcept {
    return quic_.session_count();
}

std::size_t
LinuxRequestRuntimeDriver::
peer_count()
    const noexcept {
    return quic_.peer_count();
}

std::size_t
LinuxRequestRuntimeDriver::
completed_send_count()
    const noexcept {
    return
        quic_.completed_send_count();
}

const PosixUdpControlStats&
LinuxRequestRuntimeDriver::
control_stats()
    const noexcept {
    return control_.stats();
}

std::uint64_t
LinuxRequestRuntimeDriver::now_ms()
    const noexcept {
    return static_cast<
        std::uint64_t>(
        std::chrono::
            duration_cast<
                std::chrono::
                    milliseconds>(
                RequestNetworkAdapter::
                    Clock::now()
                    .time_since_epoch())
            .count());
}

int
LinuxRequestRuntimeDriver::
wait_timeout_ms()
    const noexcept {
    // Lease cleanup is allowed to lag by at most 50 ms while socket and QUIC
    // events wake the loop immediately.
    int timeout = 50;

    const auto now =
        RequestNetworkAdapter::
            Clock::now();

    if (const auto deadline =
            runtime_
                .request_network()
                .next_deadline()) {
        timeout =
            std::min(
                timeout,
                duration_timeout_ms(
                    *deadline -
                    now));
    }

    if (const auto announcement =
            control_
                .next_announcement_ms()) {
        const auto current =
            now_ms();

        if (*announcement <= current) {
            timeout = 0;
        } else {
            const auto delta =
                *announcement -
                current;

            timeout =
                std::min(
                    timeout,
                    static_cast<int>(
                        std::min<
                            std::uint64_t>(
                            delta,
                            INT_MAX)));
        }
    }

    return timeout;
}

void
LinuxRequestRuntimeDriver::
wait_for_event() {
    pollfd fds[2]{
        {
            control_.native_fd(),
            POLLIN,
            0},
        {
            wake_read_,
            POLLIN,
            0}};

    while (true) {
        const auto result =
            ::poll(
                fds,
                2,
                wait_timeout_ms());

        if (result >= 0) {
            break;
        }

        if (errno != EINTR) {
            throw std::runtime_error(
                "Cito Linux request runtime poll failed");
        }

        if (stop_requested_.load(
                std::memory_order_acquire)) {
            return;
        }
    }

    if (
        fds[1].revents &
        POLLIN) {
        drain_wake();
    }
}

void
LinuxRequestRuntimeDriver::wake()
    noexcept {
    if (wake_write_ < 0) {
        return;
    }

    const std::uint8_t byte = 1;

    const auto result =
        ::write(
            wake_write_,
            &byte,
            sizeof(byte));

    (void)result;
}

void
LinuxRequestRuntimeDriver::
drain_wake()
    noexcept {
    if (wake_read_ < 0) {
        return;
    }

    std::array<
        std::uint8_t,
        128> buffer{};

    while (true) {
        const auto received =
            ::read(
                wake_read_,
                buffer.data(),
                buffer.size());

        if (received > 0) {
            continue;
        }

        if (
            received < 0 &&
            errno == EINTR) {
            continue;
        }

        break;
    }
}

std::shared_ptr<
    LinuxRequestRuntimeDriver>
install_linux_request_runtime(
    EmbeddedHostRuntime& runtime,
    LinuxRequestRuntimeOptions options) {
    auto driver =
        std::make_shared<
            LinuxRequestRuntimeDriver>(
                runtime,
                std::move(options));

    if (!runtime.install_driver(
            driver)) {
        throw std::logic_error(
            "Cito runtime already has a driver");
    }

    return driver;
}

} // namespace cito::detail
