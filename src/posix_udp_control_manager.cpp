#include <citoq/detail/posix_udp_control_manager.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <utility>
#include <vector>

namespace citoq::detail {

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
#if defined(CITOQ_PLUS_DISCOVERY) && CITOQ_PLUS_DISCOVERY
        next_gossip_ms_ = 0;
#endif
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

#if defined(CITOQ_PLUS_DISCOVERY) && CITOQ_PLUS_DISCOVERY
        // Bounded fanout of *our own* authoritative summary plus hints.
        // A hint never installs a coordinator, DemandKey or endpoint route:
        // it only initiates a direct CTS2 handshake with the hinted peer.
        if (now_ms >= next_gossip_ms_) {
            next_gossip_ms_ = now_ms + 900;
            if (!verified_peers_.empty()) {
                const auto summary = udp_.announcement(control_port_, lease_ms_);
                const auto count = std::min<std::size_t>(3, verified_peers_.size());
                for (std::size_t j = 0; j < count; ++j) {
                    const auto target = verified_peers_[(gossip_cursor_ + j) % verified_peers_.size()];
                    if (send_bytes(target, summary)) ++stats_.gossip_announcements_sent;
                    if (verified_peers_.size() > 1) {
                        const auto other = verified_peers_[(gossip_cursor_ + j + 1) % verified_peers_.size()];
                        if (other != target) {
                            if (send_bytes(target, encode_gossip_hint(other))) ++stats_.gossip_hints_sent;
                        }
                    }
                }
                gossip_cursor_ = (gossip_cursor_ + count) % verified_peers_.size();
            }
        }
#endif

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
#if defined(CITOQ_PLUS_DISCOVERY) && CITOQ_PLUS_DISCOVERY
                const auto incoming = std::span<const std::uint8_t>(
                    buffer.data(), static_cast<std::size_t>(received));
                if (incoming.size() == 10 && incoming[0] == 'C' &&
                    incoming[1] == 'T' && incoming[2] == 'G' && incoming[3] == '1') {
                    if (known_peer(endpoint) && now_ms >= next_invite_ms_) {
                        const auto suggested = decode_gossip_hint(incoming);
                        if (suggested.ipv4 != 0 && suggested.port != 0 &&
                            suggested != endpoint && !known_peer(suggested)) {
                            // A peer hint is untrusted metadata. Validate the
                            // hinted endpoint by requesting a direct summary.
                            const auto own = udp_.announcement(control_port_, lease_ms_);
                            if (send_bytes(suggested, own)) {
                                ++stats_.gossip_hints_received;
                                next_invite_ms_ = now_ms + 300;
                            }
                        }
                    }
                    continue;
                }
#endif
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
#if defined(CITOQ_PLUS_DISCOVERY) && CITOQ_PLUS_DISCOVERY
                if (incoming.size() == 40 && incoming[0] == 'C' &&
                    incoming[1] == 'T' && incoming[2] == 'S' && incoming[3] == '2') {
                    const auto summary = citoq::decode_summary_announcement(incoming);
                    remember_peer(UdpEndpoint{source_ip(endpoint), summary.control_port});
                }
#endif
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
        if (expired != 0 && std::getenv("CITOQ_NATIVE_DIAGNOSTICS")) {
            const auto* enabled = std::getenv("CITOQ_NATIVE_DIAGNOSTICS");
            if (enabled[0] == '1' && enabled[1] == '\0') {
                std::fprintf(stderr,
                    "CITOQ_DIAG_LEASE_EXPIRED count=%zu now_ms=%llu\n",
                    expired, static_cast<unsigned long long>(now_ms));
                std::fflush(stderr);
            }
        }
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
#if defined(CITOQ_PLUS_DISCOVERY) && CITOQ_PLUS_DISCOVERY
    static std::uint32_t source_ip(UdpEndpoint endpoint) noexcept { return endpoint.ipv4; }

    bool known_peer(UdpEndpoint e) const {
        return std::find(verified_peers_.begin(), verified_peers_.end(), e)
            != verified_peers_.end();
    }

    void remember_peer(UdpEndpoint e) {
        if (e.ipv4 == 0 || e.port == 0 || known_peer(e)) return;
        constexpr std::size_t cap = 64;
        if (verified_peers_.size() < cap) {
            verified_peers_.push_back(e);
        } else {
            verified_peers_[peer_replace_cursor_++ % cap] = e;
        }
        ++stats_.gossip_peers_learned;
    }

    static std::array<std::uint8_t, 10> encode_gossip_hint(UdpEndpoint e) {
        return {'C', 'T', 'G', '1',
            static_cast<std::uint8_t>(e.ipv4),
            static_cast<std::uint8_t>(e.ipv4 >> 8),
            static_cast<std::uint8_t>(e.ipv4 >> 16),
            static_cast<std::uint8_t>(e.ipv4 >> 24),
            static_cast<std::uint8_t>(e.port),
            static_cast<std::uint8_t>(e.port >> 8)};
    }

    static UdpEndpoint decode_gossip_hint(std::span<const std::uint8_t> b) {
        return UdpEndpoint{
            std::uint32_t(b[4]) | (std::uint32_t(b[5]) << 8) |
            (std::uint32_t(b[6]) << 16) | (std::uint32_t(b[7]) << 24),
            std::uint16_t(std::uint16_t(b[8]) | (std::uint16_t(b[9]) << 8))};
    }
#endif
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
#if defined(CITOQ_PLUS_DISCOVERY) && CITOQ_PLUS_DISCOVERY
    std::uint32_t lease_ms_{3000};
#else
    std::uint32_t lease_ms_{1000};
#endif
    std::uint32_t interval_ms_{150};
    std::uint64_t
        next_announcement_ms_{};

    std::vector<UdpEndpoint>
        targets_;
#if defined(CITOQ_PLUS_DISCOVERY) && CITOQ_PLUS_DISCOVERY
    std::vector<UdpEndpoint> verified_peers_;
    std::uint64_t next_gossip_ms_{};
    std::uint64_t next_invite_ms_{};
    std::size_t gossip_cursor_{};
    std::size_t peer_replace_cursor_{};
#endif

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

} // namespace citoq::detail
