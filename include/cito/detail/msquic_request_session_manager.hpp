#pragma once

#include <cito/detail/embedded_host_runtime.hpp>
#include <cito/detail/request_session_lifecycle.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace cito::detail {

struct MsQuicRequestSessionOptions {
    std::string registration_name{
        "cito-request"};
    // An explicit trust anchor for self-managed LAN certificates. By default
    // MsQuic uses the platform trust store and validates the peer's name.
    std::string ca_certificate_file;
    // Optional client identity when this node dials an mTLS server.
    // Both files are required; a half-specified key pair fails closed.
    std::string client_certificate_file;
    std::string client_private_key_file;
    // Optional server-side mTLS trust anchor. When require_client_certificate
    // is true, this CA is mandatory; a handshake without a trusted client
    // certificate is rejected before application authorization.
    std::string trusted_client_ca_file;
    bool require_client_certificate{false};
    // Insecure test-only escape hatch. Never enable in shipped applications.
    bool skip_server_certificate_validation{
        false};
};

class MsQuicRequestSessionManager {
public:
    MsQuicRequestSessionManager(
        EmbeddedHostRuntime& runtime,
        RequestSessionLifecycleCore* lifecycle,
        MsQuicRequestSessionOptions options = {});

    ~MsQuicRequestSessionManager();

    MsQuicRequestSessionManager(
        const MsQuicRequestSessionManager&) =
        delete;
    MsQuicRequestSessionManager& operator=(
        const MsQuicRequestSessionManager&) =
        delete;

    MsQuicRequestSessionManager(
        MsQuicRequestSessionManager&&) noexcept;
    MsQuicRequestSessionManager& operator=(
        MsQuicRequestSessionManager&&) noexcept;

    bool start_listener(
        std::uint16_t port,
        const std::string& certificate_file,
        const std::string& private_key_file);

    void stop_listener();

    // Loop-owner entrypoint. Executes queued MsQuic completions and mutates
    // Cito request/discovery state only on the caller's thread.
    std::size_t poll(
        std::size_t max_events = 0);

    // Convert RequestSessionLifecycleCore actions into concrete MsQuic
    // connect/disconnect operations. Call from the same loop owner as poll().
    std::size_t reconcile();

    void set_wake_callback(
        std::function<void()> wake);

    bool has_session(
        DestinationId destination)
        const noexcept;

    std::size_t session_count()
        const noexcept;

    std::size_t peer_count()
        const noexcept;

    std::size_t outstanding_send_count()
        const noexcept;

    std::size_t completed_send_count()
        const noexcept;

    std::uint16_t listen_port()
        const noexcept;

    void shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cito::detail
