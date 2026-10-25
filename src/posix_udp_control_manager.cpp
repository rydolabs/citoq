#include <cito/detail/posix_udp_control_manager.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace cito::detail {

class PosixUdpControlManager::Impl {
public:
    Impl(
        UdpRuntimeAdapter& udp,
        RequestSessionLifecycleCore*
            lifecycle)
        : udp_(udp),
          lifecycle_(lifecycle) {}

    ~Impl() {
        stop();
    }

    bool bind(
        std::uint16_t control_port) {
        if (fd_ >= 0) {
            return false;
        }

        const auto fd =
            ::socket(
                AF_INET,
                SOCK_DGRAM,
                0);

        if (fd < 0) {
            return false;
        }

        int yes = 1;
        (void)::setsockopt(
            fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &yes,
            sizeof(yes));
        (void)::setsockopt(
            fd,
            SOL_SOCKET,
            SO_BROADCAST,
            &yes,
            sizeof(yes));

        const auto flags =
            ::fcntl(
                fd,
                F_GETFL,
                0);

        if (
            flags < 0 ||
            ::fcntl(
                fd,
                F_SETFL,
                flags | O_NONBLOCK) !=
                0) {
            ::close(fd);
            return false;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr =
            htonl(INADDR_ANY);
        address.sin_port =
            htons(control_port);

        if (::bind(
                fd,
                reinterpret_cast<
                    sockaddr*>(
                    &address),
                sizeof(address)) != 0) {
            ::close(fd);
            return false;
        }

        sockaddr_in bound{};
        socklen_t bound_size =
            sizeof(bound);

        if (::getsockname(
                fd,
                reinterpret_cast<
                    sockaddr*>(
                    &bound),
                &bound_size) != 0) {
            ::close(fd);
            return false;
        }

        fd_ = fd;
        control_port_ =
            ntohs(bound.sin_port);
        next_announcement_ms_ = 0;
        return control_port_ != 0;
    }

    void add_announcement_target(
        UdpEndpoint endpoint) {
        if (
            endpoint.ipv4 == 0 ||
            endpoint.port == 0) {
            return;
        }

        targets_.push_back(
            endpoint);
    }

    void set_announcement_policy(
        std::uint32_t lease_ms,
        std::uint32_t interval_ms) {
        lease_ms_ =
            lease_ms == 0
                ? 1
                : lease_ms;
        interval_ms_ =
            interval_ms == 0
                ? 1
                : interval_ms;
    }

    std::size_t poll(
        std::uint64_t now_ms,
        std::size_t max_datagrams) {
        if (fd_ < 0) {
            return 0;
        }

        std::size_t work = 0;

        work +=
            flush_lifecycle();

        if (
            !targets_.empty() &&
            now_ms >=
                next_announcement_ms_) {
            const auto bytes =
                udp_.announcement(
                    control_port_,
                    lease_ms_);

            for (const auto& target :
                 targets_) {
                if (send_bytes(
                        target,
                        bytes)) {
                    ++work;
                }
            }

            next_announcement_ms_ =
                now_ms +
                interval_ms_;
        }

        for (std::size_t i = 0;
             i < max_datagrams;
             ++i) {
            std::array<
                std::uint8_t,
                65535> buffer{};

            sockaddr_in source{};
            socklen_t source_size =
                sizeof(source);

            const auto received =
                ::recvfrom(
                    fd_,
                    buffer.data(),
                    buffer.size(),
                    0,
                    reinterpret_cast<
                        sockaddr*>(
                        &source),
                    &source_size);

            if (received < 0) {
                if (
                    errno == EAGAIN ||
                    errno == EWOULDBLOCK) {
                    break;
                }

                if (errno == EINTR) {
                    --i;
                    continue;
                }

                break;
            }

            if (received == 0) {
                continue;
            }

            ++stats_.datagrams_received;
            ++work;

            const UdpEndpoint endpoint{
                ntohl(
                    source.sin_addr.s_addr),
                ntohs(
                    source.sin_port)};

            try {
                const auto sends =
                    udp_.process_control(
                        std::span<
                            const std::uint8_t>(
                            buffer.data(),
                            static_cast<
                                std::size_t>(
                                received)),
                        endpoint,
                        now_ms);

                work +=
                    send_actions(
                        sends);
            } catch (...) {
                ++stats_.
                    malformed_datagrams;
            }
        }

        work +=
            flush_lifecycle();

        const auto expired =
            udp_.expire(now_ms);

        stats_.expired_coordinators +=
            expired;
        work += expired;

        return work;
    }

    void stop() {
        if (fd_ < 0) {
            return;
        }

        ::close(fd_);
        fd_ = -1;
        control_port_ = 0;
    }

    bool open() const noexcept {
        return fd_ >= 0;
    }

    int native_fd() const noexcept {
        return fd_;
    }

    std::uint16_t control_port()
        const noexcept {
        return control_port_;
    }

    std::optional<std::uint64_t>
    next_announcement_ms()
        const noexcept {
        if (
            fd_ < 0 ||
            targets_.empty()) {
            return std::nullopt;
        }

        return next_announcement_ms_;
    }

    const PosixUdpControlStats&
    stats() const noexcept {
        return stats_;
    }

private:
    static sockaddr_in
    to_sockaddr(
        UdpEndpoint endpoint) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr =
            htonl(endpoint.ipv4);
        address.sin_port =
            htons(endpoint.port);
        return address;
    }

    bool send_bytes(
        UdpEndpoint endpoint,
        std::span<
            const std::uint8_t>
            bytes) {
        if (
            fd_ < 0 ||
            endpoint.ipv4 == 0 ||
            endpoint.port == 0 ||
            bytes.empty()) {
            ++stats_.send_failures;
            return false;
        }

        const auto address =
            to_sockaddr(
                endpoint);

        const auto sent =
            ::sendto(
                fd_,
                bytes.data(),
                bytes.size(),
                0,
                reinterpret_cast<
                    const sockaddr*>(
                    &address),
                sizeof(address));

        if (
            sent !=
            static_cast<ssize_t>(
                bytes.size())) {
            ++stats_.send_failures;
            return false;
        }

        ++stats_.datagrams_sent;
        return true;
    }

    std::size_t send_actions(
        std::span<const UdpSend>
            sends) {
        std::size_t sent = 0;

        for (const auto& action :
             sends) {
            if (send_bytes(
                    action.endpoint,
                    action.bytes)) {
                ++sent;
            }
        }

        return sent;
    }

    std::size_t flush_lifecycle() {
        if (!lifecycle_) {
            return 0;
        }

        const auto sends =
            lifecycle_->
                take_control_sends();

        return send_actions(
            sends);
    }

    UdpRuntimeAdapter& udp_;
    RequestSessionLifecycleCore*
        lifecycle_{};

    int fd_{-1};
    std::uint16_t control_port_{};
    std::uint32_t lease_ms_{1000};
    std::uint32_t interval_ms_{150};
    std::uint64_t
        next_announcement_ms_{};

    std::vector<UdpEndpoint>
        targets_;

    PosixUdpControlStats stats_;
};

PosixUdpControlManager::
PosixUdpControlManager(
    UdpRuntimeAdapter& udp,
    RequestSessionLifecycleCore*
        lifecycle)
    : impl_(
          std::make_unique<Impl>(
              udp,
              lifecycle)) {}

PosixUdpControlManager::
~PosixUdpControlManager() = default;

PosixUdpControlManager::
PosixUdpControlManager(
    PosixUdpControlManager&&)
    noexcept = default;

PosixUdpControlManager&
PosixUdpControlManager::operator=(
    PosixUdpControlManager&&)
    noexcept = default;

bool
PosixUdpControlManager::bind(
    std::uint16_t control_port) {
    return
        impl_ &&
        impl_->bind(
            control_port);
}

void
PosixUdpControlManager::
add_announcement_target(
    UdpEndpoint endpoint) {
    if (impl_) {
        impl_->
            add_announcement_target(
                endpoint);
    }
}

void
PosixUdpControlManager::
set_announcement_policy(
    std::uint32_t lease_ms,
    std::uint32_t interval_ms) {
    if (impl_) {
        impl_->
            set_announcement_policy(
                lease_ms,
                interval_ms);
    }
}

std::size_t
PosixUdpControlManager::poll(
    std::uint64_t now_ms,
    std::size_t max_datagrams) {
    return
        impl_
            ? impl_->poll(
                  now_ms,
                  max_datagrams)
            : 0;
}

void
PosixUdpControlManager::stop() {
    if (impl_) {
        impl_->stop();
    }
}

bool
PosixUdpControlManager::open()
    const noexcept {
    return
        impl_ &&
        impl_->open();
}

int
PosixUdpControlManager::native_fd()
    const noexcept {
    return
        impl_
            ? impl_->native_fd()
            : -1;
}

std::uint16_t
PosixUdpControlManager::
control_port()
    const noexcept {
    return
        impl_
            ? impl_->control_port()
            : 0;
}

std::optional<std::uint64_t>
PosixUdpControlManager::
next_announcement_ms()
    const noexcept {
    return
        impl_
            ? impl_->
                  next_announcement_ms()
            : std::nullopt;
}

const PosixUdpControlStats&
PosixUdpControlManager::stats()
    const noexcept {
    static const
        PosixUdpControlStats empty{};

    return
        impl_
            ? impl_->stats()
            : empty;
}

} // namespace cito::detail
