#pragma once

#include <cito/detail/request_session_lifecycle.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace cito::detail {

struct PosixUdpControlStats {
    std::size_t datagrams_received{0};
    std::size_t datagrams_sent{0};
    std::size_t malformed_datagrams{0};
    std::size_t send_failures{0};
    std::size_t expired_coordinators{0};
};

class PosixUdpControlManager {
public:
    PosixUdpControlManager(
        UdpRuntimeAdapter& udp,
        RequestSessionLifecycleCore*
            lifecycle = nullptr);

    ~PosixUdpControlManager();

    PosixUdpControlManager(
        const PosixUdpControlManager&) =
        delete;
    PosixUdpControlManager& operator=(
        const PosixUdpControlManager&) =
        delete;

    PosixUdpControlManager(
        PosixUdpControlManager&&) noexcept;
    PosixUdpControlManager& operator=(
        PosixUdpControlManager&&) noexcept;

    bool bind(
        std::uint16_t control_port);

    void add_announcement_target(
        UdpEndpoint endpoint);

    void set_announcement_policy(
        std::uint32_t lease_ms,
        std::uint32_t interval_ms);

    // Loop-owner entrypoint. Socket I/O is nonblocking and bounded.
    std::size_t poll(
        std::uint64_t now_ms,
        std::size_t max_datagrams = 64);

    void stop();

    bool open() const noexcept;
    int native_fd() const noexcept;

    std::uint16_t control_port()
        const noexcept;

    std::optional<std::uint64_t>
    next_announcement_ms()
        const noexcept;

    const PosixUdpControlStats&
    stats() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cito::detail
