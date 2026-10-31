#include <citoq/detail/msquic_request_session_manager.hpp>
#include <citoq/detail/quic_reliable_delivery.hpp>

#include <msquic.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <random>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace citoq::detail {

namespace {

constexpr std::uint8_t kAlpnBytes[] = {
    'c', 'i', 't', 'o', '-', 'r', 'q', '/', '1'};

const QUIC_BUFFER kAlpn{
    static_cast<std::uint32_t>(
        sizeof(kAlpnBytes)),
    const_cast<std::uint8_t*>(
        kAlpnBytes)};

std::string ipv4_text(
    std::uint32_t address) {
    return
        std::to_string(
            (address >> 24) &
            0xffu) +
        "." +
        std::to_string(
            (address >> 16) &
            0xffu) +
        "." +
        std::to_string(
            (address >> 8) &
            0xffu) +
        "." +
        std::to_string(
            address & 0xffu);
}

} // namespace

class MsQuicRequestSessionManager::Impl {
private:
    struct ConnectionState;
    struct StreamState;

    struct ConnectionContext {
        Impl* impl{};
        std::shared_ptr<
            ConnectionState> state;
    };

    struct StreamContext {
        Impl* impl{};
        std::shared_ptr<
            StreamState> state;
    };

    struct SendContext {
        Impl* impl{};
        std::vector<
            std::uint8_t> bytes;
        QUIC_BUFFER buffer{};

        // The event queue already owns a copy of the packet. Move it into
        // MsQuic's SEND_COMPLETE-lifetime buffer instead of copying again.
        SendContext(
            Impl* owner,
            std::vector<std::uint8_t>
                source)
            : impl(owner),
              bytes(
                  std::move(source)) {
            buffer.Length =
                static_cast<
                    std::uint32_t>(
                    bytes.size());
            buffer.Buffer =
                bytes.data();
        }
    };

    struct ConnectionState {
        HQUIC handle{};
        ConnectionContext*
            callback_context{};
        DestinationId destination{};
        RequestPeerId peer{};
        UdpEndpoint endpoint{};
        bool outbound{false};
        std::atomic<bool> connected{
            false};
        std::atomic<bool> closing{
            false};
    };

    struct StreamState {
        HQUIC handle{};
        std::shared_ptr<
            ConnectionState> connection;
        std::vector<std::uint8_t>
            received;
        bool server_side{false};
        bool finalized{false};
        std::atomic<bool> closed{
            false};
    };

public:
    Impl(
        EmbeddedHostRuntime& runtime,
        RequestSessionLifecycleCore*
            lifecycle,
        MsQuicRequestSessionOptions
            options)
        : runtime_(runtime),
          lifecycle_(lifecycle),
          options_(std::move(options)) {
        if (QUIC_FAILED(
                MsQuicOpen2(
                    &api_))) {
            return;
        }

        QUIC_REGISTRATION_CONFIG
            registration{};
        registration.AppName =
            options_.registration_name
                .c_str();
        registration.ExecutionProfile =
            QUIC_EXECUTION_PROFILE_LOW_LATENCY;

        if (QUIC_FAILED(
                api_->RegistrationOpen(
                    &registration,
                    &registration_))) {
            MsQuicClose(api_);
            api_ = nullptr;
            return;
        }

        ready_ =
            open_client_configuration();

        if (
            ready_ &&
            lifecycle_) {
            runtime_.request_network()
                .set_wait_hook(
                    [lifecycle =
                         lifecycle_](
                        const RequestKey&
                            key) {
                        lifecycle->ensure(
                            key);
                    });
            runtime_.request_network().set_data_interest_hooks(
                [lifecycle = lifecycle_](const InterestKey& key) {
                    (void)lifecycle->ensure_data(key);
                },
                [lifecycle = lifecycle_](const InterestKey& key) {
                    (void)lifecycle->release_data(key);
                });
            owns_wait_hook_ = true;
        }
    }

    ~Impl() {
        shutdown();

        if (server_configuration_) {
            api_->ConfigurationClose(
                server_configuration_);
            server_configuration_ =
                nullptr;
        }

        if (client_configuration_) {
            api_->ConfigurationClose(
                client_configuration_);
            client_configuration_ =
                nullptr;
        }

        if (registration_) {
            api_->RegistrationClose(
                registration_);
            registration_ = nullptr;
        }

        if (api_) {
            MsQuicClose(api_);
            api_ = nullptr;
        }
    }

    bool ready() const noexcept {
        return ready_;
    }

    bool start_listener(
        std::uint16_t port,
        const std::string&
            certificate_file,
        const std::string&
            private_key_file) {
        if (
            !ready_ ||
            listener_ ||
            certificate_file.empty() ||
            private_key_file.empty()) {
            return false;
        }

        if (!open_server_configuration(
                certificate_file,
                private_key_file)) {
            return false;
        }

        HQUIC listener = nullptr;

        if (QUIC_FAILED(
                api_->ListenerOpen(
                    registration_,
                    &listener_callback,
                    this,
                    &listener))) {
            return false;
        }

        QUIC_ADDR address{};
        QuicAddrSetFamily(
            &address,
            QUIC_ADDRESS_FAMILY_UNSPEC);
        QuicAddrSetPort(
            &address,
            port);

        if (QUIC_FAILED(
                api_->ListenerStart(
                    listener,
                    &kAlpn,
                    1,
                    &address))) {
            api_->ListenerClose(
                listener);
            return false;
        }

        listener_ = listener;
        listen_port_ = port;
        return true;
    }

    void stop_listener() {
        if (!listener_) {
            return;
        }

        api_->ListenerStop(
            listener_);
        api_->ListenerClose(
            listener_);
        listener_ = nullptr;
        listen_port_ = 0;
    }

    std::size_t poll(
        std::size_t max_events) {
        std::size_t count = 0;

        while (
            max_events == 0 ||
            count < max_events) {
            std::function<void()>
                event;

            {
                std::lock_guard<
                    std::mutex>
                    lock(queue_mutex_);

                if (events_.empty()) {
                    break;
                }

                event =
                    std::move(
                        events_.front());
                events_.pop_front();
            }

            event();
            ++count;
        }

        // Lazy typed requests are encoded only after an attached session is
        // ready, and deadlines advance from the same loop-owner poll path.
#if defined(CITOQ_STABILIZE_RECONNECT) && CITOQ_STABILIZE_RECONNECT
        drop_expired_deferred();
#endif
#if defined(CITOQ_RELIABLE_QUIC) && CITOQ_RELIABLE_QUIC
        retry_unacked();
#endif
        runtime_.request_network()
            .progress();

        return count;
    }

    void set_wake_callback(
        std::function<void()> wake) {
        std::lock_guard<std::mutex>
            lock(queue_mutex_);
        wake_ =
            std::move(wake);
    }

    std::size_t reconcile() {
        if (
            !ready_ ||
            !lifecycle_) {
            return 0;
        }

        const auto actions =
            lifecycle_->reconcile();

        for (const auto& action :
             actions) {
            if (
                action.kind ==
                RequestSessionActionKind::
                    Connect) {
                if (!connect(
                        action.destination,
                        action.endpoint)) {
                    lifecycle_->
                        mark_connect_failed(
                            action.destination,
                            action.endpoint);
                }
            } else {
                disconnect(
                    action.destination);
            }
        }

        return actions.size();
    }

    bool has_session(
        DestinationId destination)
        const noexcept {
        return
            runtime_.request_network()
                .has_session(
                    destination);
    }

    std::size_t session_count()
        const noexcept {
        return
            runtime_.request_network()
                .session_count();
    }

    std::size_t peer_count()
        const noexcept {
        return inbound_.size();
    }

    std::size_t
    outstanding_send_count()
        const noexcept {
        return
            outstanding_sends_
                .load(
                    std::memory_order_relaxed);
    }

    std::size_t
    completed_send_count()
        const noexcept {
        return
            completed_sends_
                .load(
                    std::memory_order_relaxed);
    }

    std::uint16_t listen_port()
        const noexcept {
        return listen_port_;
    }

    void shutdown() {
        if (!api_) {
            return;
        }

        if (stopping_.exchange(
                true,
                std::memory_order_acq_rel)) {
            return;
        }

        if (owns_wait_hook_) {
            runtime_.request_network().clear_data_interest_hooks();
            runtime_.request_network()
                .clear_wait_hook();
            runtime_.request_network()
                .fail_waiting(
                    RequestStatus::
                        Unavailable);
            owns_wait_hook_ = false;
        }

        stop_listener();

        std::vector<
            std::shared_ptr<
                ConnectionState>>
            connections;

        connections.reserve(
            outbound_.size() +
            inbound_.size());

        for (const auto& [_, state] :
             outbound_) {
            connections.push_back(
                state);
        }

        for (const auto& [_, state] :
             inbound_) {
            connections.push_back(
                state);
        }

        for (const auto& state :
             connections) {
            if (
                state &&
                state->handle &&
                !state->closing.load(
                    std::memory_order_acquire)) {
                state->closing.store(
                    true,
                    std::memory_order_release);
                api_->ConnectionShutdown(
                    state->handle,
                    QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                    0);
            }
        }

        const auto deadline =
            std::chrono::
                steady_clock::now() +
            std::chrono::seconds(2);

        while (
            (!outbound_.empty() ||
             !inbound_.empty()) &&
            std::chrono::
                steady_clock::now() <
                deadline) {
            poll(0);
            std::this_thread::sleep_for(
                std::chrono::
                    milliseconds(1));
        }

        for (auto& [destination, _] :
             outbound_) {
            runtime_.request_network()
                .detach_session(
                    destination,
                    RequestStatus::
                        Unavailable);
        }

        outbound_.clear();

        for (const auto& [peer, _] :
             inbound_) {
            runtime_.request_network()
                .drop_peer(peer);
        }

        inbound_.clear();
        inbound_streams_.clear();

        poll(0);
    }

private:
    bool open_client_configuration() {
        QUIC_SETTINGS settings{};
        settings.IdleTimeoutMs =
            10000;
        settings.IsSet.IdleTimeoutMs =
            TRUE;
        settings.PeerBidiStreamCount =
            64;
        settings.IsSet
            .PeerBidiStreamCount =
            TRUE;

        if (QUIC_FAILED(
                api_->ConfigurationOpen(
                    registration_,
                    &kAlpn,
                    1,
                    &settings,
                    sizeof(settings),
                    nullptr,
                    &client_configuration_))) {
            return false;
        }

        QUIC_CREDENTIAL_CONFIG credentials{};
        QUIC_CERTIFICATE_FILE client_files{};
        const bool has_cert = !options_.client_certificate_file.empty();
        const bool has_key = !options_.client_private_key_file.empty();
        if (has_cert != has_key) return false;

        credentials.Type = has_cert
            ? QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE
            : QUIC_CREDENTIAL_TYPE_NONE;
        if (has_cert) {
            client_files.CertificateFile = options_.client_certificate_file.c_str();
            client_files.PrivateKeyFile = options_.client_private_key_file.c_str();
            credentials.CertificateFile = &client_files;
        }
        credentials.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;

        if (options_.skip_server_certificate_validation) {
            // Fail closed on conflicting security options rather than silently
            // accepting an unverified peer despite a configured CA.
            if (!options_.ca_certificate_file.empty()) return false;
            credentials.Flags = static_cast<QUIC_CREDENTIAL_FLAGS>(
                credentials.Flags | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION);
        } else if (!options_.ca_certificate_file.empty()) {
            credentials.Flags = static_cast<QUIC_CREDENTIAL_FLAGS>(
                credentials.Flags | QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE);
            credentials.CaCertificateFile = options_.ca_certificate_file.c_str();
        }

        return QUIC_SUCCEEDED(
            api_->
                ConfigurationLoadCredential(
                    client_configuration_,
                    &credentials));
    }

    bool open_server_configuration(
        const std::string&
            certificate_file,
        const std::string&
            private_key_file) {
        if (server_configuration_) {
            api_->ConfigurationClose(
                server_configuration_);
            server_configuration_ =
                nullptr;
        }

        QUIC_SETTINGS settings{};
        settings.IdleTimeoutMs =
            10000;
        settings.IsSet.IdleTimeoutMs =
            TRUE;
        settings.PeerBidiStreamCount =
            64;
        settings.IsSet
            .PeerBidiStreamCount =
            TRUE;

        if (QUIC_FAILED(
                api_->ConfigurationOpen(
                    registration_,
                    &kAlpn,
                    1,
                    &settings,
                    sizeof(settings),
                    nullptr,
                    &server_configuration_))) {
            return false;
        }

        QUIC_CERTIFICATE_FILE files{};
        files.CertificateFile =
            certificate_file.c_str();
        files.PrivateKeyFile =
            private_key_file.c_str();

        QUIC_CREDENTIAL_CONFIG
            credentials{};
        credentials.Type =
            QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
        credentials.CertificateFile =
            &files;
        credentials.Flags = QUIC_CREDENTIAL_FLAG_NONE;
        if (options_.require_client_certificate) {
            if (options_.trusted_client_ca_file.empty()) {
                api_->ConfigurationClose(server_configuration_);
                server_configuration_ = nullptr;
                return false; // never require mTLS without a verifiable CA
            }
            credentials.Flags = static_cast<QUIC_CREDENTIAL_FLAGS>(
                QUIC_CREDENTIAL_FLAG_REQUIRE_CLIENT_AUTHENTICATION |
                QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE);
            credentials.CaCertificateFile = options_.trusted_client_ca_file.c_str();
        }

        if (QUIC_FAILED(
                api_->
                    ConfigurationLoadCredential(
                        server_configuration_,
                        &credentials))) {
            api_->ConfigurationClose(
                server_configuration_);
            server_configuration_ =
                nullptr;
            return false;
        }

        return true;
    }

    bool connect(
        DestinationId destination,
        UdpEndpoint endpoint) {
        if (
            destination == 0 ||
            endpoint.ipv4 == 0 ||
            endpoint.port == 0 ||
            outbound_.contains(
                destination)) {
            return false;
        }

        trace("CONNECT_BEGIN", destination);
        auto state =
            std::make_shared<
                ConnectionState>();

        state->destination =
            destination;
        state->peer =
            static_cast<
                RequestPeerId>(
                destination);
        state->endpoint =
            endpoint;
        state->outbound = true;

        auto* context =
            new ConnectionContext{
                this,
                state};

        HQUIC connection = nullptr;

        if (QUIC_FAILED(
                api_->ConnectionOpen(
                    registration_,
                    &connection_callback,
                    context,
                    &connection))) {
            delete context;
            return false;
        }

        state->handle =
            connection;
        state->callback_context =
            context;

        outbound_.emplace(
            destination,
            state);

        const auto host =
            ipv4_text(
                endpoint.ipv4);

        if (QUIC_FAILED(
                api_->ConnectionStart(
                    connection,
                    client_configuration_,
                    QUIC_ADDRESS_FAMILY_UNSPEC,
                    host.c_str(),
                    endpoint.port))) {
            outbound_.erase(
                destination);
            api_->ConnectionClose(
                connection);
            delete context;
            state->handle = nullptr;
            state->callback_context =
                nullptr;
            return false;
        }

        trace("CONNECT_START_OK", destination);
        return true;
    }

    void disconnect(
        DestinationId destination) {
        const auto it =
            outbound_.find(
                destination);

        runtime_.request_network()
            .detach_session(
                destination,
                RequestStatus::
                    Unavailable);
        trace("DISCONNECT", destination);

        if (it == outbound_.end()) {
            return;
        }

        const auto state =
            it->second;

        if (
            state->handle &&
            !state->closing.load(
                    std::memory_order_acquire)) {
            state->closing.store(
                    true,
                    std::memory_order_release);
            api_->ConnectionShutdown(
                state->handle,
                QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                0);
        }
    }

    bool queue_packet(
        DestinationId destination,
        std::span<const std::uint8_t>
            bytes) {
        if (
            stopping_.load(
                std::memory_order_acquire) ||
            !runtime_.request_network()
                 .has_session(
                     destination)) {
            return false;
        }

        std::vector<std::uint8_t>
            owned(
                bytes.begin(),
                bytes.end());

        post(
            [this,
             destination,
             owned =
                 std::move(owned)] {
                send_packet(
                    destination,
                    std::move(owned));
            });

        return true;
    }

#if defined(CITOQ_STABILIZE_RECONNECT) && CITOQ_STABILIZE_RECONNECT
    // Only data that never reached StreamOpen may be replayed automatically.
    // A successful StreamSend can still fail at the application layer and
    // requires a different, explicitly acknowledged Reliable delivery mode.
    struct DeferredData {
        std::vector<std::uint8_t> bytes;
        std::chrono::steady_clock::time_point expires_at;
    };
    void defer_unsent(DestinationId destination,
                      std::vector<std::uint8_t> bytes) {
        constexpr std::size_t kMaxDeferredFrames = 1024;
        constexpr std::size_t kMaxDeferredBytes = 4u * 1024u * 1024u;
        if (deferred_frames_ >= kMaxDeferredFrames ||
            bytes.size() > kMaxDeferredBytes - deferred_bytes_) {
            trace("DEFERRED_REJECTED_LIMIT", destination, bytes.size());
            return;
        }
        deferred_bytes_ += bytes.size();
        ++deferred_frames_;
        deferred_[destination].push_back(DeferredData{
            std::move(bytes),
            std::chrono::steady_clock::now() + std::chrono::seconds(8)});
        trace("DEFERRED_QUEUED", destination);
    }
    void drop_expired_deferred() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = deferred_.begin(); it != deferred_.end();) {
            auto& pending = it->second;
            while (!pending.empty() && pending.front().expires_at <= now) {
                deferred_bytes_ -= pending.front().bytes.size();
                --deferred_frames_;
                trace("DEFERRED_EXPIRED", it->first);
                pending.pop_front();
            }
            if (pending.empty()) it = deferred_.erase(it);
            else ++it;
        }
    }
    void flush_deferred(DestinationId destination) {
        const auto it = deferred_.find(destination);
        if (it == deferred_.end()) return;
        auto pending = std::move(it->second);
        deferred_.erase(it);
        const auto now = std::chrono::steady_clock::now();
        for (auto& item : pending) {
            deferred_bytes_ -= item.bytes.size();
            --deferred_frames_;
            if (item.expires_at <= now) {
                trace("DEFERRED_EXPIRED", destination);
                continue;
            }
            trace("DEFERRED_REPLAY", destination, item.bytes.size());
            send_packet(destination, std::move(item.bytes));
        }
    }
#endif

#if defined(CITOQ_RELIABLE_QUIC) && CITOQ_RELIABLE_QUIC
    struct AwaitingAck {
        DestinationId destination{};
        std::vector<std::uint8_t> frame;
        std::chrono::steady_clock::time_point next_retry{};
        std::chrono::steady_clock::time_point expires{};
        unsigned attempts{0};
    };
    bool prepare_reliable(DestinationId destination,
                          std::vector<std::uint8_t>& bytes) {
        if (!quic_reliable::magic(bytes,'D')) return true;
        constexpr std::size_t kMaxFrames=4096;
        constexpr std::size_t kMaxBytes=16u*1024u*1024u;
        if (pending_ack_.size()>=kMaxFrames ||
            bytes.size()>kMaxBytes-pending_ack_bytes_) {
            ++reliable_rejected_;
            return false;
        }
        if (next_reliable_sequence_==0) {
            ++reliable_rejected_;
            return false;
        }
        const auto sequence=next_reliable_sequence_++;
        auto wire=quic_reliable::encode_data(
            sender_epoch_, sequence, bytes);
        const auto now=std::chrono::steady_clock::now();
        AwaitingAck record{
            destination, wire,
            now+std::chrono::milliseconds(650),
            now+std::chrono::seconds(14), 0};
        pending_ack_bytes_+=wire.size();
        pending_ack_.emplace(sequence,std::move(record));
        bytes=std::move(wire);
        return true;
    }
    void accept_ack(DestinationId destination,
                    const quic_reliable::Ack& ack) {
        if (ack.epoch!=sender_epoch_) return;
        const auto it=pending_ack_.find(ack.sequence);
        if (it==pending_ack_.end() || it->second.destination!=destination)
            return;
        pending_ack_bytes_-=it->second.frame.size();
        pending_ack_.erase(it);
        ++reliable_acked_;
    }
    void retry_unacked() {
        const auto now=std::chrono::steady_clock::now();
        std::size_t budget=64;
        for (auto it=pending_ack_.begin();it!=pending_ack_.end();) {
            auto& record=it->second;
            if (now>=record.expires || record.attempts>=6) {
                pending_ack_bytes_-=record.frame.size();
                ++reliable_expired_;
                it=pending_ack_.erase(it);
                continue;
            }
            if (budget && now>=record.next_retry) {
                auto out=outbound_.find(record.destination);
                if (out!=outbound_.end() && out->second->handle &&
                    out->second->connected.load(std::memory_order_acquire)) {
                    --budget;
                    send_packet(record.destination,record.frame,true);
                    ++record.attempts;
                    ++reliable_retries_;
                    const auto ms=std::min<unsigned>(3200,650u << 
                        std::min<unsigned>(record.attempts,2));
                    record.next_retry=now+std::chrono::milliseconds(ms);
                } else {
                    record.next_retry=now+std::chrono::milliseconds(250);
                }
            }
            ++it;
        }
        if (diagnostics_ && now>=next_reliable_stats_) {
            next_reliable_stats_=now+std::chrono::seconds(2);
            std::fprintf(stderr,
                "CITOQ_RELIABLE_METRICS pending=%zu acked=%zu retries=%zu expired=%zu rejected=%zu duplicates=%zu\n",
                pending_ack_.size(),reliable_acked_,reliable_retries_,
                reliable_expired_,reliable_rejected_,reliable_duplicates_);
            std::fflush(stderr);
        }
    }
#endif
    void send_packet(
        DestinationId destination,
        std::vector<std::uint8_t>
            bytes
#if defined(CITOQ_RELIABLE_QUIC) && CITOQ_RELIABLE_QUIC
        , bool already_wrapped=false
#endif
    ) {
#if defined(CITOQ_RELIABLE_QUIC) && CITOQ_RELIABLE_QUIC
        if (!already_wrapped && !prepare_reliable(destination,bytes)) return;
#endif
        const auto it =
            outbound_.find(
                destination);

        if (
            it == outbound_.end() ||
            !it->second->connected.load(
                    std::memory_order_acquire) ||
            !it->second->handle) {
#if defined(CITOQ_STABILIZE_RECONNECT) && CITOQ_STABILIZE_RECONNECT
            defer_unsent(destination, std::move(bytes));
#else
            trace("ASYNC_SEND_DROPPED_NOT_CONNECTED", destination, bytes.size());
#endif
            runtime_.request_network()
                .detach_session(
                    destination,
                    RequestStatus::
                        Unavailable);
            return;
        }

        auto stream =
            std::make_shared<
                StreamState>();

        stream->connection =
            it->second;
        stream->server_side =
            false;

        auto* context =
            new StreamContext{
                this,
                stream};

        HQUIC handle = nullptr;

        if (QUIC_FAILED(
                api_->StreamOpen(
                    it->second->handle,
                    QUIC_STREAM_OPEN_FLAG_NONE,
                    &stream_callback,
                    context,
                    &handle))) {
            delete context;
            trace("STREAM_OPEN_FAILED", destination, bytes.size());
            transport_failed(
                destination);
            return;
        }

        stream->handle =
            handle;

        if (QUIC_FAILED(
                api_->StreamStart(
                    handle,
                    QUIC_STREAM_START_FLAG_NONE))) {
            api_->StreamClose(
                handle);
            stream->handle =
                nullptr;
            delete context;
            trace("STREAM_START_FAILED", destination, bytes.size());
            transport_failed(
                destination);
            return;
        }

        if (!send_on_stream(
                stream,
                std::move(bytes))) {
            if (
                stream->handle &&
                !stream->closed.load(
                    std::memory_order_acquire)) {
                api_->StreamShutdown(
                    stream->handle,
                    QUIC_STREAM_SHUTDOWN_FLAG_ABORT,
                    0);
            }

            trace("STREAM_SEND_FAILED", destination);
            transport_failed(
                destination);
        }
    }

    bool send_on_stream(
        const std::shared_ptr<
            StreamState>& stream,
        std::vector<std::uint8_t>
            bytes) {
        if (
            !stream ||
            !stream->handle ||
            stream->closed.load(
                std::memory_order_acquire)) {
            return false;
        }

        auto* send =
            new SendContext(
                this,
                std::move(bytes));

        outstanding_sends_
            .fetch_add(
                1,
                std::memory_order_relaxed);

        if (QUIC_FAILED(
                api_->StreamSend(
                    stream->handle,
                    &send->buffer,
                    1,
                    QUIC_SEND_FLAG_FIN,
                    send))) {
            outstanding_sends_
                .fetch_sub(
                    1,
                    std::memory_order_relaxed);
            delete send;
            return false;
        }

        return true;
    }

    void transport_failed(
        DestinationId destination) {
        trace("TRANSPORT_FAILED", destination);
        runtime_.request_network()
            .detach_session(
                destination,
                RequestStatus::
                    Unavailable);

        if (
            lifecycle_ &&
            outbound_.contains(
                destination)) {
            lifecycle_->
                mark_disconnected(
                    destination);
        }

        disconnect(
            destination);
    }

    void on_connected(
        const std::shared_ptr<
            ConnectionState>& state) {
        if (
            stopping_.load(
                std::memory_order_acquire) ||
            !state ||
            !state->outbound ||
            !outbound_.contains(
                state->destination) ||
            outbound_.at(
                state->destination)
                .get() != state.get()) {
            if (
                state &&
                state->handle) {
                api_->ConnectionShutdown(
                    state->handle,
                    QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                    0);
            }
            return;
        }

        if (
            lifecycle_ &&
            !lifecycle_->
                 mark_connected(
                     state->destination,
                     state->endpoint)) {
            api_->ConnectionShutdown(
                state->handle,
                QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                0);
            return;
        }

        state->connected.store(
            true,
            std::memory_order_release);
        trace("CONNECTED", state->destination);

        runtime_.request_network()
            .attach_session(
                state->destination,
                [this,
                 destination =
                     state->destination](
                    std::span<
                        const std::uint8_t>
                        bytes) {
                    return queue_packet(
                        destination,
                        bytes);
                });
#if defined(CITOQ_STABILIZE_RECONNECT) && CITOQ_STABILIZE_RECONNECT
        flush_deferred(state->destination);
#endif
    }

    void on_connection_closed(
        const std::shared_ptr<
            ConnectionState>& state) {
        if (!state) {
            return;
        }

        state->connected.store(
            false,
            std::memory_order_release);
        if (state->outbound)
            trace("CONNECTION_CLOSED", state->destination);

        if (state->outbound) {
            const auto it =
                outbound_.find(
                    state->destination);

            if (
                it != outbound_.end() &&
                it->second.get() ==
                    state.get()) {
                runtime_.request_network()
                    .detach_session(
                        state->destination,
                        RequestStatus::
                            Unavailable);

                outbound_.erase(it);

                if (
                    lifecycle_ &&
                    lifecycle_->
                        is_connecting(
                            state->destination)) {
                    lifecycle_->
                        mark_connect_failed(
                            state->destination,
                            state->endpoint);
                } else if (lifecycle_) {
                    lifecycle_->
                        mark_disconnected(
                            state->destination);
                }
            }
        } else {
            runtime_.request_network()
                .drop_peer(
                    state->peer);
            inbound_.erase(
                state->peer);

            for (auto it =
                     inbound_streams_
                         .begin();
                 it !=
                 inbound_streams_
                     .end();) {
                if (
                    it->first.peer ==
                    state->peer) {
                    it =
                        inbound_streams_
                            .erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    void on_stream_bytes(
        const std::shared_ptr<
            StreamState>& stream,
        std::vector<std::uint8_t>
            bytes,
        bool peer_done) {
        if (
            !stream ||
            stream->finalized) {
            return;
        }

        // The MsQuic RECEIVE callback aggregated these bytes into a
        // uniquely-owned vector. Most messages arrive as one receive event:
        // steal its allocation instead of allocating and copying again.
        if (stream->received.empty()) {
            stream->received = std::move(bytes);
        } else {
            stream->received.insert(
                stream->received.end(),
                bytes.begin(),
                bytes.end());
        }

        if (!peer_done) {
            return;
        }

        stream->finalized = true;

        if (
            stream->received.empty()) {
            graceful_stream_finish(
                stream);
            return;
        }

#if defined(CITOQ_RELIABLE_QUIC) && CITOQ_RELIABLE_QUIC
        if (stream->server_side &&
            quic_reliable::magic(stream->received,'Y')) {
            if (!options_.require_client_certificate) {
                graceful_stream_finish(stream);
                return;
            }
            try {
                const auto data=quic_reliable::decode_data(stream->received);
                if (dedup_.contains(data.epoch,data.sequence)) {
                    ++reliable_duplicates_;
                } else {
                    // ACK is generated only after synchronous application
                    // callback dispatch accepted the CTD1 payload.
                    if (runtime_.request_network().receive_data(
                            data.original_ctd1)==0) {
                        graceful_stream_finish(stream);
                        return;
                    }
                    dedup_.commit(data.epoch,data.sequence);
                }
                if (!send_on_stream(stream,quic_reliable::encode_ack(
                        {data.epoch,data.sequence})))
                    graceful_stream_finish(stream);
            } catch (...) {
                graceful_stream_finish(stream);
            }
            return;
        }
        if (!stream->server_side &&
            quic_reliable::magic(stream->received,'K')) {
            try {
                const auto ack=quic_reliable::decode_ack(stream->received);
                accept_ack(stream->connection->destination,ack);
            } catch (...) {}
            graceful_stream_finish(stream);
            return;
        }
#endif
        // Data is admitted only when the listener required a validated
        // client certificate. Legacy unauthenticated RPC remains separate.
        if (stream->server_side &&
            stream->received.size() >= 4 &&
            stream->received[0] == 'C' &&
            stream->received[1] == 'T' &&
            stream->received[2] == 'D' &&
            stream->received[3] == '1') {
            if (options_.require_client_certificate) {
                (void)runtime_.request_network().receive_data(
                    stream->received);
            }
            graceful_stream_finish(stream);
            return;
        }

        RequestTransportFrameView
            frame;

        try {
            frame =
                decode_request_transport_frame(
                    stream->received);
        } catch (...) {
            graceful_stream_finish(
                stream);
            return;
        }

        if (stream->server_side) {
            process_server_frame(
                stream,
                frame);
        } else {
            process_client_frame(
                stream,
                frame);
        }
    }

    void process_client_frame(
        const std::shared_ptr<
            StreamState>& stream,
        const RequestTransportFrameView&
            frame) {
        if (
            frame.kind !=
            RequestTransportKind::
                Reply) {
            graceful_stream_finish(
                stream);
            return;
        }

        runtime_.request_network()
            .receive(
                static_cast<
                    RequestPeerId>(
                    stream->connection
                        ->destination),
                stream->received);

        graceful_stream_finish(
            stream);
    }

    void process_server_frame(
        const std::shared_ptr<
            StreamState>& stream,
        const RequestTransportFrameView&
            frame) {
        const auto peer =
            stream->connection->peer;

        if (
            frame.kind ==
            RequestTransportKind::
                Request) {
            const InboundRequestKey
                key{
                    peer,
                    frame.request_id};

            inbound_streams_
                .insert_or_assign(
                    key,
                    stream);

            const auto accepted =
                runtime_.request_network()
                    .receive(
                        peer,
                        stream->received,
                        [this,
                         key,
                         weak =
                             std::weak_ptr<
                                 StreamState>(
                                 stream)](
                            std::span<
                                const std::uint8_t>
                                reply) {
                            if (
                                stopping_.load(
                                    std::memory_order_acquire)) {
                                return false;
                            }

                            std::vector<
                                std::uint8_t>
                                owned(
                                    reply.begin(),
                                    reply.end());

                            post(
                                [this,
                                 key,
                                 weak,
                                 owned =
                                     std::move(
                                         owned)] {
                                    inbound_streams_
                                        .erase(
                                            key);

                                    const auto target =
                                        weak.lock();

                                    if (!target) {
                                        return;
                                    }

                                    if (!send_on_stream(
                                            target,
                                            std::move(owned))) {
                                        graceful_stream_finish(
                                            target);
                                    }
                                });

                            return true;
                        });

            if (!accepted) {
                // Unavailable still queues one reply through SendReply.
            }

            return;
        }

        if (
            frame.kind ==
            RequestTransportKind::
                Cancel) {
            runtime_.request_network()
                .receive(
                    peer,
                    stream->received);

            const InboundRequestKey
                key{
                    peer,
                    frame.request_id};

            const auto original =
                inbound_streams_
                    .find(key);

            if (
                original !=
                inbound_streams_
                    .end()) {
                if (const auto target =
                        original->second
                            .lock()) {
                    graceful_stream_finish(
                        target);
                }

                inbound_streams_.erase(
                    original);
            }

            graceful_stream_finish(
                stream);
            return;
        }

        graceful_stream_finish(
            stream);
    }

    void graceful_stream_finish(
        const std::shared_ptr<
            StreamState>& stream) {
        if (
            !stream ||
            !stream->handle ||
            stream->closed.load(
                std::memory_order_acquire)) {
            return;
        }

        api_->StreamShutdown(
            stream->handle,
            QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL,
            0);
    }

    void post(
        std::function<void()>
            event) {
        std::function<void()> wake;

        {
            std::lock_guard<std::mutex>
                lock(queue_mutex_);
            events_.push_back(
                std::move(event));
            if (diagnostics_ && events_.size() >= queue_report_threshold_) {
                std::fprintf(stderr, "CITOQ_DIAG_EVENT_QUEUE depth=%zu\n",
                             events_.size());
                std::fflush(stderr);
                queue_report_threshold_ *= 4; // high-water signal at most logarithmic
            }
            wake = wake_;
        }

        if (wake) {
            try {
                wake();
            } catch (...) {
                // Wake notification is advisory; queued work remains intact.
            }
        }
    }

    static QUIC_STATUS QUIC_API
    listener_callback(
        HQUIC,
        void* context,
        QUIC_LISTENER_EVENT* event) {
        auto* self =
            static_cast<Impl*>(
                context);

        if (
            !self ||
            event->Type !=
                QUIC_LISTENER_EVENT_NEW_CONNECTION ||
            !self->server_configuration_ ||
            self->stopping_.load(
                std::memory_order_acquire)) {
            return
                QUIC_STATUS_NOT_SUPPORTED;
        }

        auto state =
            std::make_shared<
                ConnectionState>();

        state->handle =
            event->
                NEW_CONNECTION
                .Connection;
        state->peer =
            self->next_peer_
                .fetch_add(
                    1,
                    std::memory_order_relaxed);
        state->outbound = false;

        auto* callback_context =
            new ConnectionContext{
                self,
                state};

        state->callback_context =
            callback_context;

        self->api_->
            SetCallbackHandler(
                state->handle,
                reinterpret_cast<void*>(
                    &connection_callback),
                callback_context);

        const auto status =
            self->api_->
                ConnectionSetConfiguration(
                    state->handle,
                    self->
                        server_configuration_);

        if (QUIC_FAILED(status)) {
            self->api_->
                ConnectionClose(
                    state->handle);
            state->handle = nullptr;
            state->callback_context =
                nullptr;
            delete callback_context;
            return status;
        }

        self->post(
            [self,
             state] {
                if (
                    state->handle &&
                    !state->closing.load(
                    std::memory_order_acquire)) {
                    self->inbound_
                        .insert_or_assign(
                            state->peer,
                            state);
                }
            });

        return status;
    }

    static QUIC_STATUS QUIC_API
    connection_callback(
        HQUIC connection,
        void* context,
        QUIC_CONNECTION_EVENT* event) {
        auto* callback_context =
            static_cast<
                ConnectionContext*>(
                context);

        if (
            !callback_context ||
            !callback_context->impl) {
            return
                QUIC_STATUS_SUCCESS;
        }

        auto* self =
            callback_context->impl;
        auto state =
            callback_context->state;

        switch (event->Type) {
            case QUIC_CONNECTION_EVENT_CONNECTED:
                if (state->outbound) {
                    self->post(
                        [self,
                         state] {
                            self->
                                on_connected(
                                    state);
                        });
                }
                break;

            case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
                auto stream =
                    std::make_shared<
                        StreamState>();

                stream->handle =
                    event->
                        PEER_STREAM_STARTED
                        .Stream;
                stream->connection =
                    state;
                stream->server_side =
                    true;

                auto* stream_context =
                    new StreamContext{
                        self,
                        stream};

                self->api_->
                    SetCallbackHandler(
                        stream->handle,
                        reinterpret_cast<void*>(
                            &stream_callback),
                        stream_context);
                break;
            }

            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
                if (state->outbound && self->diagnostics_) {
                    std::fprintf(stderr,
                        "CITOQ_DIAG_QUIC stage=SHUTDOWN_TRANSPORT destination=%llu status=%lld error_code=%llu\n",
                        static_cast<unsigned long long>(state->destination),
                        static_cast<long long>(
                            event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status),
                        static_cast<unsigned long long>(
                            event->SHUTDOWN_INITIATED_BY_TRANSPORT.ErrorCode));
                    std::fflush(stderr);
                }
                break;
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
                if (state->outbound && self->diagnostics_) {
                    std::fprintf(stderr,
                        "CITOQ_DIAG_QUIC stage=SHUTDOWN_PEER destination=%llu error_code=%llu\n",
                        static_cast<unsigned long long>(state->destination),
                        static_cast<unsigned long long>(
                            event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode));
                    std::fflush(stderr);
                }
                break;
            case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
                self->post(
                    [self,
                     state] {
                        self->
                            on_connection_closed(
                                state);
                    });

                state->closing.store(
                    true,
                    std::memory_order_release);
                state->connected.store(
                    false,
                    std::memory_order_release);
                state->callback_context =
                    nullptr;

                self->api_->
                    ConnectionClose(
                        connection);

                delete callback_context;
                break;

            default:
                break;
        }

        return QUIC_STATUS_SUCCESS;
    }

    static QUIC_STATUS QUIC_API
    stream_callback(
        HQUIC stream_handle,
        void* context,
        QUIC_STREAM_EVENT* event) {
        auto* stream_context =
            static_cast<
                StreamContext*>(
                context);

        if (
            !stream_context ||
            !stream_context->impl) {
            return
                QUIC_STATUS_SUCCESS;
        }

        auto* self =
            stream_context->impl;
        auto stream =
            stream_context->state;

        switch (event->Type) {
            case QUIC_STREAM_EVENT_RECEIVE: {
                std::size_t total = 0;

                for (
                    std::uint32_t i = 0;
                    i <
                    event->RECEIVE
                        .BufferCount;
                    ++i) {
                    total +=
                        event->RECEIVE
                            .Buffers[i]
                            .Length;
                }

                std::vector<
                    std::uint8_t>
                    bytes;
                bytes.reserve(total);

                for (
                    std::uint32_t i = 0;
                    i <
                    event->RECEIVE
                        .BufferCount;
                    ++i) {
                    const auto& buffer =
                        event->RECEIVE
                            .Buffers[i];

                    bytes.insert(
                        bytes.end(),
                        buffer.Buffer,
                        buffer.Buffer +
                            buffer.Length);
                }

                const bool peer_done =
                    (event->RECEIVE.Flags &
                     QUIC_RECEIVE_FLAG_FIN) !=
                    0;

                self->post(
                    [self,
                     stream,
                     bytes =
                         std::move(bytes),
                     peer_done]() mutable {
                        self->
                            on_stream_bytes(
                                stream,
                                std::move(
                                    bytes),
                                peer_done);
                    });
                break;
            }

            case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
                self->post(
                    [self,
                     stream] {
                        self->
                            on_stream_bytes(
                                stream,
                                {},
                                true);
                    });
                break;

            case QUIC_STREAM_EVENT_SEND_COMPLETE: {
                auto* send =
                    static_cast<
                        SendContext*>(
                        event->
                            SEND_COMPLETE
                            .ClientContext);

                if (send) {
                    send->impl->
                        outstanding_sends_
                        .fetch_sub(
                            1,
                            std::memory_order_relaxed);
                    send->impl->
                        completed_sends_
                        .fetch_add(
                            1,
                            std::memory_order_relaxed);
                    delete send;
                }
                break;
            }

            case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
                stream->closed.store(
                    true,
                    std::memory_order_release);

                self->api_->
                    StreamClose(
                        stream_handle);

                delete stream_context;
                break;

            default:
                break;
        }

        return QUIC_STATUS_SUCCESS;
    }

private:
    void trace(const char* stage, DestinationId destination,
               std::size_t detail = 0) const {
        if (!diagnostics_) return;
        std::fprintf(stderr,
            "CITOQ_DIAG_QUIC stage=%s destination=%llu detail=%zu\n",
            stage, static_cast<unsigned long long>(destination), detail);
        std::fflush(stderr);
    }

    EmbeddedHostRuntime& runtime_;
    RequestSessionLifecycleCore*
        lifecycle_{};
    MsQuicRequestSessionOptions
        options_;

    const QUIC_API_TABLE* api_{};
    HQUIC registration_{};
    HQUIC client_configuration_{};
    HQUIC server_configuration_{};
    HQUIC listener_{};
    std::uint16_t listen_port_{};
    bool ready_{false};
    bool owns_wait_hook_{false};
    const bool diagnostics_ = [] {
        const char* value = std::getenv("CITOQ_NATIVE_DIAGNOSTICS");
        return value && std::string_view(value) == "1";
    }();

    std::atomic<bool> stopping_{
        false};
    std::atomic<
        std::uint64_t> next_peer_{
            1};
    std::atomic<
        std::size_t> outstanding_sends_{
            0};
    std::atomic<
        std::size_t> completed_sends_{
            0};

#if defined(CITOQ_RELIABLE_QUIC) && CITOQ_RELIABLE_QUIC
    std::uint64_t sender_epoch_{[] {
        std::random_device rand;
        std::uint64_t epoch=(std::uint64_t(rand())<<32) ^ rand();
        return epoch ? epoch : std::uint64_t{1};
    }()};
    std::uint64_t next_reliable_sequence_{1};
    std::unordered_map<std::uint64_t,AwaitingAck> pending_ack_;
    quic_reliable::Dedup dedup_;
    std::size_t pending_ack_bytes_{};
    std::size_t reliable_acked_{};
    std::size_t reliable_retries_{};
    std::size_t reliable_expired_{};
    std::size_t reliable_rejected_{};
    std::size_t reliable_duplicates_{};
    std::chrono::steady_clock::time_point next_reliable_stats_{};
#endif
    std::size_t queue_report_threshold_{64};
#if defined(CITOQ_STABILIZE_RECONNECT) && CITOQ_STABILIZE_RECONNECT
    std::unordered_map<DestinationId, std::deque<DeferredData>> deferred_;
    std::size_t deferred_frames_{};
    std::size_t deferred_bytes_{};
#endif

    std::mutex queue_mutex_;
    std::deque<
        std::function<void()>>
        events_;
    std::function<void()>
        wake_;

    std::unordered_map<
        DestinationId,
        std::shared_ptr<
            ConnectionState>>
        outbound_;

    std::unordered_map<
        RequestPeerId,
        std::shared_ptr<
            ConnectionState>>
        inbound_;

    std::unordered_map<
        InboundRequestKey,
        std::weak_ptr<
            StreamState>,
        InboundRequestKeyHash>
        inbound_streams_;
};

MsQuicRequestSessionManager::
MsQuicRequestSessionManager(
    EmbeddedHostRuntime& runtime,
    RequestSessionLifecycleCore* lifecycle,
    MsQuicRequestSessionOptions options)
    : impl_(
          std::make_unique<Impl>(
              runtime,
              lifecycle,
              std::move(options))) {}

MsQuicRequestSessionManager::
~MsQuicRequestSessionManager() = default;

MsQuicRequestSessionManager::
MsQuicRequestSessionManager(
    MsQuicRequestSessionManager&&)
    noexcept = default;

MsQuicRequestSessionManager&
MsQuicRequestSessionManager::operator=(
    MsQuicRequestSessionManager&&)
    noexcept = default;

bool
MsQuicRequestSessionManager::
start_listener(
    std::uint16_t port,
    const std::string&
        certificate_file,
    const std::string&
        private_key_file) {
    return
        impl_ &&
        impl_->start_listener(
            port,
            certificate_file,
            private_key_file);
}

void
MsQuicRequestSessionManager::
stop_listener() {
    if (impl_) {
        impl_->stop_listener();
    }
}

std::size_t
MsQuicRequestSessionManager::poll(
    std::size_t max_events) {
    return
        impl_
            ? impl_->poll(
                  max_events)
            : 0;
}

std::size_t
MsQuicRequestSessionManager::
reconcile() {
    return
        impl_
            ? impl_->reconcile()
            : 0;
}

void
MsQuicRequestSessionManager::
set_wake_callback(
    std::function<void()> wake) {
    if (impl_) {
        impl_->set_wake_callback(
            std::move(wake));
    }
}

bool
MsQuicRequestSessionManager::
has_session(
    DestinationId destination)
    const noexcept {
    return
        impl_ &&
        impl_->has_session(
            destination);
}

std::size_t
MsQuicRequestSessionManager::
session_count()
    const noexcept {
    return
        impl_
            ? impl_->session_count()
            : 0;
}

std::size_t
MsQuicRequestSessionManager::
peer_count()
    const noexcept {
    return
        impl_
            ? impl_->peer_count()
            : 0;
}

std::size_t
MsQuicRequestSessionManager::
outstanding_send_count()
    const noexcept {
    return
        impl_
            ? impl_->
                  outstanding_send_count()
            : 0;
}

std::size_t
MsQuicRequestSessionManager::
completed_send_count()
    const noexcept {
    return
        impl_
            ? impl_->
                  completed_send_count()
            : 0;
}

std::uint16_t
MsQuicRequestSessionManager::
listen_port()
    const noexcept {
    return
        impl_
            ? impl_->listen_port()
            : 0;
}

void
MsQuicRequestSessionManager::
shutdown() {
    if (impl_) {
        impl_->shutdown();
    }
}

} // namespace citoq::detail
