#include <cito/detail/msquic_request_session_manager.hpp>

#include <msquic.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cito::detail {

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

        SendContext(
            Impl* owner,
            std::span<
                const std::uint8_t>
                source)
            : impl(owner),
              bytes(
                  source.begin(),
                  source.end()) {
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
                    owned);
            });

        return true;
    }

    void send_packet(
        DestinationId destination,
        std::span<const std::uint8_t>
            bytes) {
        const auto it =
            outbound_.find(
                destination);

        if (
            it == outbound_.end() ||
            !it->second->connected.load(
                    std::memory_order_acquire) ||
            !it->second->handle) {
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
            transport_failed(
                destination);
            return;
        }

        if (!send_on_stream(
                stream,
                bytes)) {
            if (
                stream->handle &&
                !stream->closed.load(
                    std::memory_order_acquire)) {
                api_->StreamShutdown(
                    stream->handle,
                    QUIC_STREAM_SHUTDOWN_FLAG_ABORT,
                    0);
            }

            transport_failed(
                destination);
        }
    }

    bool send_on_stream(
        const std::shared_ptr<
            StreamState>& stream,
        std::span<const std::uint8_t>
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
                bytes);

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

        stream->received.insert(
            stream->received.end(),
            bytes.begin(),
            bytes.end());

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
                                            owned)) {
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

} // namespace cito::detail
