#pragma once

#include <citoq/detail/embedded_host_runtime.hpp>
#include <citoq/detail/msquic_request_session_manager.hpp>
#include <citoq/detail/posix_udp_control_manager.hpp>
#include <citoq/detail/request_session_lifecycle.hpp>
#include <citoq/detail/udp_runtime_adapter.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace citoq::detail {

struct LinuxRequestRuntimeOptions {
    CoordinatorId coordinator{};
    std::uint64_t incarnation{};
    DestinationId destination{};

    std::uint16_t control_port{};
    std::uint16_t quic_port{};

    std::uint32_t lease_ms{1000};
    std::uint32_t announce_interval_ms{150};

    std::vector<UdpEndpoint>
        announcement_targets;

    std::string certificate_file;
    std::string private_key_file;

    MsQuicRequestSessionOptions
        quic;
};

class LinuxRequestRuntimeDriver final
    : public RuntimeDriver {
public:
    LinuxRequestRuntimeDriver(
        EmbeddedHostRuntime& runtime,
        LinuxRequestRuntimeOptions options);

    ~LinuxRequestRuntimeDriver()
        override;

    LinuxRequestRuntimeDriver(
        const LinuxRequestRuntimeDriver&) =
        delete;
    LinuxRequestRuntimeDriver&
    operator=(
        const LinuxRequestRuntimeDriver&) =
        delete;

    void run() override;
    void stop() noexcept override;

    // One nonblocking owner-thread progression step, useful for integration
    // tests and future composition with a larger CitoQ loop.
    std::size_t poll_once();

    // Single-owner blocking progression: process ready work, wait on control
    // socket / QUIC wake fd (bounded by max_wait_ms), then process completions.
    // No worker thread and no application callback on a foreign thread.
    std::size_t poll_wait(std::uint32_t max_wait_ms);

    // Owner-thread explicit publisher intent for Discovery-driven data routes.
    bool ensure_data(const InterestKey& key);
    bool release_data(const InterestKey& key);

    bool running() const noexcept;

    std::uint16_t control_port()
        const noexcept;

    std::size_t session_count()
        const noexcept;

    std::size_t peer_count()
        const noexcept;

    std::size_t completed_send_count()
        const noexcept;

    std::size_t reliable_acked_count() const noexcept;
    std::size_t reliable_pending_count() const noexcept;

    const PosixUdpControlStats&
    control_stats()
        const noexcept;

private:
    std::uint64_t now_ms()
        const noexcept;

    int wait_timeout_ms()
        const noexcept;

    void wait_for_event(int max_wait_ms = 50);
    void wake() noexcept;
    void drain_wake() noexcept;

    EmbeddedHostRuntime& runtime_;
    LinuxRequestRuntimeOptions options_;

    UdpRuntimeAdapter routes_;
    RequestSessionLifecycleCore
        lifecycle_;
    PosixUdpControlManager control_;
    MsQuicRequestSessionManager quic_;

    int wake_read_{-1};
    int wake_write_{-1};

    std::atomic<bool> running_{
        false};
    std::atomic<bool> stop_requested_{
        false};
};

std::shared_ptr<
    LinuxRequestRuntimeDriver>
install_linux_request_runtime(
    EmbeddedHostRuntime& runtime,
    LinuxRequestRuntimeOptions options);

} // namespace citoq::detail
