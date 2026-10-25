#pragma once

#include <msquic.h>

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace cito::detail {

// Owns a native MsQuic listener, not its accepted connections. In particular,
// each successful Accept MUST install a connection callback before returning,
// arrange ConnectionSetConfiguration for the server TLS configuration, and
// eventually close the accepted connection after SHUTDOWN_COMPLETE. MsQuic
// retains ownership of rejected connections (failed Accept result).
//
// start/stop/close run on the Cito owner thread. Accept runs on a MsQuic worker
// and must be thread-safe: it must not directly mutate the Context or
// RequestSessionManager. ListenerClose is blocking and must never be called
// from a MsQuic listener callback.
class RequestMsQuicListener {
public:
    using Accept = std::function<QUIC_STATUS(HQUIC)>;

    RequestMsQuicListener(const QUIC_API_TABLE* api, HQUIC registration,
                          Accept accept)
        : api_(api), registration_(registration), accept_(std::move(accept)) {}

    RequestMsQuicListener(const RequestMsQuicListener&) = delete;
    RequestMsQuicListener& operator=(const RequestMsQuicListener&) = delete;
    ~RequestMsQuicListener() { close(); }

    // ALPN bytes are kept alive for the entire listener lifetime. A caller
    // can pass a stack-allocated ALPN and release it immediately afterwards.
    bool start(std::span<const std::uint8_t> alpn, const QUIC_ADDR* bind) {
        if (listener_ || !api_ || !registration_ || !accept_ || alpn.empty() ||
            alpn.size() >= 255) return false;
        try { alpn_.assign(alpn.begin(), alpn.end()); }
        catch (...) { return false; }

        HQUIC opened = nullptr;
        if (QUIC_FAILED(api_->ListenerOpen(registration_, &listener_callback, this, &opened)) ||
            !opened) {
            // An anomalous non-null handle returned on error is still ours.
            if (opened) api_->ListenerClose(opened);
            alpn_.clear();
            return false;
        }
        listener_ = opened;
        QUIC_BUFFER buffer{static_cast<std::uint32_t>(alpn_.size()), alpn_.data()};
        if (QUIC_FAILED(api_->ListenerStart(listener_, &buffer, 1, bind))) {
            // MsQuic ListenerClose waits for any in-flight callbacks. Never
            // close from the callback itself, including synchronous Start.
            close();
            return false;
        }
        running_ = true;
        return true;
    }

    void stop() {
        if (listener_ && running_) {
            running_ = false;
            api_->ListenerStop(listener_);
        }
    }

    void close() {
        if (!listener_) return;
        stop();
        const auto handle = listener_;
        listener_ = nullptr;
        api_->ListenerClose(handle);
        alpn_.clear();
    }

    bool running() const noexcept { return running_; }
    bool opened() const noexcept { return listener_ != nullptr; }

private:
    static QUIC_STATUS QUIC_API listener_callback(HQUIC, void* context,
                                                   QUIC_LISTENER_EVENT* event) {
        if (!context || !event) return QUIC_STATUS_INVALID_PARAMETER;
        if (event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION)
            return QUIC_STATUS_SUCCESS;
        auto* self = static_cast<RequestMsQuicListener*>(context);
        if (!self->accept_ || !event->NEW_CONNECTION.Connection)
            return QUIC_STATUS_INVALID_PARAMETER;
        try { return self->accept_(event->NEW_CONNECTION.Connection); }
        catch (...) {
            // Never unwind C++ exceptions through a C ABI callback. MsQuic
            // rejects the connection for a failed NEW_CONNECTION result.
            return QUIC_STATUS_INVALID_PARAMETER;
        }
    }

    const QUIC_API_TABLE* api_{};
    HQUIC registration_{};
    Accept accept_;
    HQUIC listener_{};
    std::vector<std::uint8_t> alpn_;
    bool running_{false};
};

} // namespace cito::detail
