#pragma once

#include <msquic.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cito::detail {

// Owner-thread-pumped server connection admission. MsQuic callbacks only set
// atomic state or install a transport-level stream callback. The owner loop
// MUST keep this object and its Configuration alive until shutdown_all(),
// followed by poll(), has reaped all connections. Destroying it while MsQuic
// can still call into it is an unrecoverable lifetime contract violation.
//
// Security: CONNECTED means TLS handshake complete, NOT that a Cito logical
// destination has been authorized. The owner's Authorize predicate MUST bind
// the connection to a verified peer identity (e.g. mTLS plus an allowlist).
// Until that predicate AND OnAuthorized both succeed, all peer streams are
// rejected. Never treat the remote IP address as proof of identity.
class RequestMsQuicServerAdmission {
public:
    using Authorize = std::function<bool(HQUIC)>; // owner thread only
    using OnAuthorized = std::function<bool(HQUIC)>; // owner thread only
    using AcceptStream = std::function<bool(HQUIC, HQUIC)>; // MsQuic worker only
    using OnClosed = std::function<void(HQUIC)>; // owner thread, before ConnectionClose

    // Optional certificate-bound node identity. Requires the MsQuic server
    // credential to set REQUIRE_CLIENT_AUTHENTICATION,
    // INDICATE_CERTIFICATE_RECEIVED and USE_PORTABLE_CERTIFICATES, with
    // actual CA validation enabled. The callback sees a borrowed DER buffer
    // on the Cito owner thread, never on an MsQuic worker.
    using AuthorizeIdentity = std::function<bool(HQUIC, std::span<const std::uint8_t>)>;
    static constexpr std::size_t kMaxPeerCertificateBytes = 16 * 1024;

    RequestMsQuicServerAdmission(const QUIC_API_TABLE* api, HQUIC configuration,
                                 std::size_t max_connections, Authorize authorize,
                                 OnAuthorized ready, AcceptStream stream, OnClosed on_closed = {},
                                 AuthorizeIdentity authorize_identity = {})
        : api_(api), configuration_(configuration), limit_(max_connections),
          authorize_(std::move(authorize)), ready_(std::move(ready)),
          accept_stream_(std::move(stream)), on_closed_(std::move(on_closed)),
          authorize_identity_(std::move(authorize_identity)) {}
    RequestMsQuicServerAdmission(const RequestMsQuicServerAdmission&) = delete;
    RequestMsQuicServerAdmission& operator=(const RequestMsQuicServerAdmission&) = delete;

    ~RequestMsQuicServerAdmission() {
        // Silent UAF is worse than failing fast. There is no safe general
        // way to free callback contexts while a native callback is in flight.
        std::lock_guard lock(mutex_);
        if (!connections_.empty()) std::terminate();
    }

    // Called from RequestMsQuicListener's NEW_CONNECTION callback.
    // Failure means MsQuic owns/rejects HQUIC: never ConnectionClose it.
    // After SetCallbackHandler succeeds, ALWAYS return success, including if
    // Configuration fails; we then shutdown and reap our accepted connection.
    QUIC_STATUS accept(HQUIC connection) noexcept {
        if (!connection || !api_ || !configuration_ || !authorize_ || !ready_ ||
            !accept_stream_ || !api_->SetCallbackHandler ||
            !api_->ConnectionSetConfiguration || !api_->ConnectionShutdown ||
            !api_->ConnectionClose || !api_->StreamShutdown || !api_->StreamClose)
            return QUIC_STATUS_INVALID_PARAMETER;
        std::shared_ptr<Entry> entry;
        try {
            entry = std::make_shared<Entry>();
            entry->connection = connection;
            entry->parent = this;
            {
                std::lock_guard lock(mutex_);
                if (stopping_ || connections_.size() >= limit_ ||
                    connections_.contains(connection))
                    return QUIC_STATUS_INVALID_PARAMETER;
                connections_.emplace(connection, entry);
            }
        } catch (...) { return QUIC_STATUS_INVALID_PARAMETER; }

        api_->SetCallbackHandler(connection,
                                 reinterpret_cast<void*>(&connection_callback), entry.get());
        const QUIC_STATUS configured = api_->ConnectionSetConfiguration(connection, configuration_);
        if (QUIC_FAILED(configured)) request_shutdown(*entry);
        entry->admission_done.store(true, std::memory_order_release);
        return QUIC_STATUS_SUCCESS;
    }

    // Must only be called from the Cito owner loop. Never call from MsQuic.
    void poll() {
        const auto entries = snapshot();
        bool stopping = false;
        { std::lock_guard lock(mutex_); stopping = stopping_; }
        for (const auto& entry : entries) {
            if (!entry->admission_done.load(std::memory_order_acquire) ||
                entry->shutdown_seen.load(std::memory_order_acquire)) continue;
            // shutdown_all() may race admission on a MsQuic worker. Do not
            // shutdown until the native callback has been installed.
            if (stopping) { request_shutdown(*entry); continue; }
            if (entry->connected.load(std::memory_order_acquire) &&
                !entry->shutdown_requested.load(std::memory_order_acquire) &&
                !entry->decided.exchange(true, std::memory_order_acq_rel)) {
                bool allowed = false;
                try {
                    bool identity_ok = true;
                    if (authorize_identity_) {
                        identity_ok = false; // deny missing, oversized or malformed certificates
                        if (entry->certificate_observed.load(std::memory_order_acquire) &&
                            !entry->certificate_invalid.load(std::memory_order_acquire)) {
                            std::vector<std::uint8_t> der;
                            {
                                std::lock_guard lock(entry->certificate_mutex);
                                der = entry->certificate_der;
                            }
                            if (!der.empty())
                                identity_ok = authorize_identity_(entry->connection, der);
                        }
                    }
                    if (identity_ok && authorize_(entry->connection)) {
                        allowed = ready_(entry->connection);
                        if (allowed) entry->prepared.store(true, std::memory_order_release);
                    }
                } catch (...) { allowed = false; }
                if (allowed && !entry->shutdown_seen.load(std::memory_order_acquire) &&
                    !entry->shutdown_requested.load(std::memory_order_acquire)) {
                    entry->authorized.store(true, std::memory_order_release);
                } else request_shutdown(*entry);
            }
        }
        for (const auto& entry : entries) {
            if (!entry->admission_done.load(std::memory_order_acquire) ||
                !entry->shutdown_seen.load(std::memory_order_acquire) ||
                entry->callbacks.load(std::memory_order_acquire) != 0) continue;
            {
                std::lock_guard lock(mutex_);
                auto it = connections_.find(entry->connection);
                if (it == connections_.end() || it->second != entry) continue;
                connections_.erase(it);
            }
            // Revoke any session published by OnAuthorized before freeing the
            // HQUIC. OnClosed MUST NOT retain or use the handle afterwards.
            if (entry->prepared.load(std::memory_order_acquire) && on_closed_) {
                try { on_closed_(entry->connection); } catch (...) {}
            }
            // SHUTDOWN_COMPLETE is MsQuic's final callback. Close only on
            // this owner thread, after it has actually returned.
            api_->ConnectionClose(entry->connection);
        }
    }

    // Initiates shutdown, but DOES NOT free handles or callback contexts.
    // Continue poll() until size() == 0, then stop/close Configuration etc.
    void shutdown_all() {
        std::vector<std::shared_ptr<Entry>> entries;
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            entries.reserve(connections_.size());
            for (auto& item : connections_) entries.push_back(item.second);
        }
        for (auto& entry : entries) {
            if (entry->admission_done.load(std::memory_order_acquire))
                request_shutdown(*entry);
        }
    }

    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return connections_.size();
    }
    bool empty() const { return size() == 0; }

private:
    struct Entry {
        RequestMsQuicServerAdmission* parent{};
        HQUIC connection{};
        std::atomic<unsigned> callbacks{0};
        std::atomic<bool> admission_done{false};
        std::atomic<bool> connected{false};
        std::atomic<bool> decided{false};
        std::atomic<bool> authorized{false};
        std::atomic<bool> prepared{false};
        std::atomic<bool> shutdown_requested{false};
        std::atomic<bool> shutdown_seen{false};
        std::mutex certificate_mutex;
        std::vector<std::uint8_t> certificate_der;
        std::atomic<bool> certificate_observed{false};
        std::atomic<bool> certificate_invalid{false};
    };

    static QUIC_STATUS QUIC_API connection_callback(HQUIC connection, void* context,
                                                      QUIC_CONNECTION_EVENT* event) noexcept {
        if (!context || !event) return QUIC_STATUS_INVALID_PARAMETER;
        auto* entry = static_cast<Entry*>(context);
        entry->callbacks.fetch_add(1, std::memory_order_acq_rel);
        struct Guard {
            Entry* entry;
            ~Guard() { entry->callbacks.fetch_sub(1, std::memory_order_acq_rel); }
        } guard{entry};
        try {
            switch (event->Type) {
                case QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED: {
                    // The MsQuic credential MUST enable portable certificates.
                    // Its QUIC_BUFFER exists only during this callback, so
                    // copy bounded bytes before returning to MsQuic.
                    entry->certificate_observed.store(true, std::memory_order_release);
                    if (entry->parent->authorize_identity_) {
                        auto* buffer = reinterpret_cast<const QUIC_BUFFER*>(
                            event->PEER_CERTIFICATE_RECEIVED.Certificate);
                        if (!buffer || !buffer->Buffer || buffer->Length == 0 ||
                            buffer->Length > kMaxPeerCertificateBytes) {
                            entry->certificate_invalid.store(true, std::memory_order_release);
                            break;
                        }
                        std::lock_guard lock(entry->certificate_mutex);
                        entry->certificate_der.assign(buffer->Buffer,
                                                       buffer->Buffer + buffer->Length);
                    }
                    break;
                }
                case QUIC_CONNECTION_EVENT_CONNECTED:
                    entry->connected.store(true, std::memory_order_release);
                    break;
                case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
                    HQUIC stream = event->PEER_STREAM_STARTED.Stream;
                    if (stream) {
                        bool adopted = false;
                        if (entry->authorized.load(std::memory_order_acquire) &&
                            !entry->shutdown_requested.load(std::memory_order_acquire) &&
                            !entry->shutdown_seen.load(std::memory_order_acquire)) {
                            try { adopted = entry->parent->accept_stream_(connection, stream); }
                            catch (...) { adopted = false; }
                        }
                        if (!adopted) {
                            entry->parent->api_->StreamShutdown(
                                stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE, 1);
                            entry->parent->api_->StreamClose(stream);
                        }
                    }
                    break;
                }
                case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
                case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
                    entry->shutdown_requested.store(true, std::memory_order_release);
                    break;
                case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
                    entry->authorized.store(false, std::memory_order_release);
                    entry->shutdown_seen.store(true, std::memory_order_release);
                    break;
                default: break;
            }
        } catch (...) { return QUIC_STATUS_INVALID_PARAMETER; }
        return QUIC_STATUS_SUCCESS;
    }

    void request_shutdown(Entry& entry) {
        if (!entry.shutdown_seen.load(std::memory_order_acquire) &&
            !entry.shutdown_requested.exchange(true, std::memory_order_acq_rel)) {
            entry.authorized.store(false, std::memory_order_release);
            api_->ConnectionShutdown(entry.connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
        }
    }

    std::vector<std::shared_ptr<Entry>> snapshot() const {
        std::lock_guard lock(mutex_);
        std::vector<std::shared_ptr<Entry>> entries;
        entries.reserve(connections_.size());
        for (const auto& item : connections_) entries.push_back(item.second);
        return entries;
    }

    const QUIC_API_TABLE* api_{};
    HQUIC configuration_{};
    std::size_t limit_{};
    Authorize authorize_;
    OnAuthorized ready_;
    AcceptStream accept_stream_;
    OnClosed on_closed_;
    AuthorizeIdentity authorize_identity_;
    mutable std::mutex mutex_;
    std::unordered_map<HQUIC, std::shared_ptr<Entry>> connections_;
    bool stopping_{false};
};

} // namespace cito::detail
