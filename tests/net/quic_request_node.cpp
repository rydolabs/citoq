#include <cito/detail/request_transport.hpp>

#include <msquic.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

const QUIC_API_TABLE* Api = nullptr;
HQUIC Registration = nullptr;
HQUIC Configuration = nullptr;

const std::uint8_t AlpnBytes[] = {
    'c','i','t','o','-','r','q','/','1'};

const QUIC_BUFFER Alpn{
    sizeof(AlpnBytes),
    const_cast<std::uint8_t*>(
        AlpnBytes)};

const cito::detail::RequestKey
    kRequestKey{
        7,
        17,
        100};

struct SendContext {
    explicit SendContext(
        std::vector<std::uint8_t> data)
        : bytes(std::move(data)) {
        buffer.Length =
            static_cast<std::uint32_t>(
                bytes.size());
        buffer.Buffer =
            bytes.data();
    }

    std::vector<std::uint8_t> bytes;
    QUIC_BUFFER buffer{};
};

struct LoopState {
    LoopState()
        : transport(
              host,
              requests) {}

    void post(
        std::function<void()> task) {
        {
            std::lock_guard<std::mutex>
                lock(queue_mutex);
            tasks.push_back(
                std::move(task));
        }

        queue_cv.notify_all();
    }

    void post_failure() {
        post(
            [this] {
                failed = true;
            });
    }

    template <class Predicate>
    bool run_until(
        Predicate predicate,
        std::chrono::milliseconds
            timeout) {
        const auto deadline =
            Clock::now() + timeout;

        while (
            !failed &&
            !predicate()) {
            std::function<void()> task;

            {
                std::unique_lock<std::mutex>
                    lock(queue_mutex);

                if (tasks.empty()) {
                    if (!queue_cv.wait_until(
                            lock,
                            deadline,
                            [this] {
                                return
                                    !tasks.empty();
                            })) {
                        break;
                    }
                }

                if (!tasks.empty()) {
                    task =
                        std::move(
                            tasks.front());
                    tasks.pop_front();
                }
            }

            if (task) {
                try {
                    task();
                } catch (...) {
                    failed = true;
                }
            }

            if (Clock::now() >= deadline) {
                break;
            }
        }

        return
            !failed &&
            predicate();
    }

    cito::detail::HostRuntimeCore host;
    cito::detail::RequestReplyCore
        requests;
    cito::detail::RequestTransportCore
        transport;

    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<
        std::function<void()>> tasks;

    bool failed{false};
    bool connected{false};
    bool stream_send_done{false};
    bool request_dispatched{false};
    bool reply_done{false};
    bool stream_processed{false};

    std::vector<std::uint8_t>
        stream_bytes;
};

bool open_api() {
    if (QUIC_FAILED(
            MsQuicOpen2(&Api))) {
        return false;
    }

    const QUIC_REGISTRATION_CONFIG
        registration{
            "cito-request-quic-smoke",
            QUIC_EXECUTION_PROFILE_LOW_LATENCY};

    return QUIC_SUCCEEDED(
        Api->RegistrationOpen(
            &registration,
            &Registration));
}

bool open_configuration(
    bool server,
    const char* cert = nullptr,
    const char* key = nullptr) {
    QUIC_SETTINGS settings{};
    settings.IdleTimeoutMs = 5000;
    settings.IsSet.IdleTimeoutMs = TRUE;
    settings.PeerBidiStreamCount = 4;
    settings.IsSet.PeerBidiStreamCount =
        TRUE;

    if (QUIC_FAILED(
            Api->ConfigurationOpen(
                Registration,
                &Alpn,
                1,
                &settings,
                sizeof(settings),
                nullptr,
                &Configuration))) {
        return false;
    }

    QUIC_CREDENTIAL_CONFIG credentials{};
    QUIC_CERTIFICATE_FILE files{};

    if (server) {
        files.CertificateFile = cert;
        files.PrivateKeyFile = key;

        credentials.Type =
            QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
        credentials.CertificateFile =
            &files;
        credentials.Flags =
            QUIC_CREDENTIAL_FLAG_NONE;
    } else {
        if (!cert || !*cert) return false;
        credentials.Type =
            QUIC_CREDENTIAL_TYPE_NONE;
        credentials.Flags =
            static_cast<
                QUIC_CREDENTIAL_FLAGS>(
                QUIC_CREDENTIAL_FLAG_CLIENT |
                QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE);
        credentials.CaCertificateFile = cert;
    }

    return QUIC_SUCCEEDED(
        Api->ConfigurationLoadCredential(
            Configuration,
            &credentials));
}

bool send_stream(
    HQUIC stream,
    std::span<const std::uint8_t>
        bytes) {
    auto* send =
        new SendContext(
            std::vector<std::uint8_t>(
                bytes.begin(),
                bytes.end()));

    if (QUIC_FAILED(
            Api->StreamSend(
                stream,
                &send->buffer,
                1,
                QUIC_SEND_FLAG_FIN,
                send))) {
        delete send;
        return false;
    }

    return true;
}

void append_receive(
    LoopState* state,
    HQUIC stream,
    std::vector<std::uint8_t> chunk,
    bool peer_done,
    bool server_side) {
    state->post(
        [state,
         stream,
         chunk =
             std::move(chunk),
         peer_done,
         server_side]() mutable {
            state->stream_bytes.insert(
                state->stream_bytes.end(),
                chunk.begin(),
                chunk.end());

            if (
                !peer_done ||
                state->stream_processed) {
                return;
            }

            state->stream_processed = true;

            if (server_side) {
                const bool dispatched =
                    state->transport.on_request(
                        1,
                        state->stream_bytes,
                        [stream](
                            std::span<
                                const std::uint8_t>
                                reply) {
                            return send_stream(
                                stream,
                                reply);
                        });

                if (!dispatched) {
                    state->failed = true;
                    return;
                }

                state->request_dispatched =
                    true;
            } else {
                if (!state->transport.on_reply(
                        77,
                        state->stream_bytes)) {
                    state->failed = true;
                }
            }
        });
}

QUIC_STATUS QUIC_API
ServerStreamCallback(
    HQUIC stream,
    void* context,
    QUIC_STREAM_EVENT* event) {
    auto* state =
        static_cast<LoopState*>(
            context);

    switch (event->Type) {
        case QUIC_STREAM_EVENT_RECEIVE: {
            std::vector<std::uint8_t>
                chunk;

            std::size_t total = 0;

            for (
                std::uint32_t i = 0;
                i <
                event->RECEIVE.BufferCount;
                ++i) {
                total +=
                    event->RECEIVE
                        .Buffers[i]
                        .Length;
            }

            chunk.reserve(total);

            for (
                std::uint32_t i = 0;
                i <
                event->RECEIVE.BufferCount;
                ++i) {
                const auto& buffer =
                    event->RECEIVE
                        .Buffers[i];

                chunk.insert(
                    chunk.end(),
                    buffer.Buffer,
                    buffer.Buffer +
                        buffer.Length);
            }

            const bool peer_done =
                (event->RECEIVE.Flags &
                 QUIC_RECEIVE_FLAG_FIN) !=
                0;

            append_receive(
                state,
                stream,
                std::move(chunk),
                peer_done,
                true);
            break;
        }

        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
            append_receive(
                state,
                stream,
                {},
                true,
                true);
            break;

        case QUIC_STREAM_EVENT_SEND_COMPLETE:
            delete static_cast<
                SendContext*>(
                event->SEND_COMPLETE
                    .ClientContext);

            state->post(
                [state] {
                    state->stream_send_done =
                        true;
                });
            break;

        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            if (!event->
                    SHUTDOWN_COMPLETE
                    .AppCloseInProgress) {
                Api->StreamClose(
                    stream);
            }
            break;

        default:
            break;
    }

    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API
ServerConnectionCallback(
    HQUIC connection,
    void* context,
    QUIC_CONNECTION_EVENT* event) {
    auto* state =
        static_cast<LoopState*>(
            context);

    switch (event->Type) {
        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
            Api->SetCallbackHandler(
                event->
                    PEER_STREAM_STARTED
                    .Stream,
                reinterpret_cast<void*>(
                    ServerStreamCallback),
                state);
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
            state->post_failure();
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            if (!event->
                    SHUTDOWN_COMPLETE
                    .AppCloseInProgress) {
                Api->ConnectionClose(
                    connection);
            }
            break;

        default:
            break;
    }

    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API
ServerListenerCallback(
    HQUIC,
    void* context,
    QUIC_LISTENER_EVENT* event) {
    if (
        event->Type !=
        QUIC_LISTENER_EVENT_NEW_CONNECTION) {
        return
            QUIC_STATUS_NOT_SUPPORTED;
    }

    Api->SetCallbackHandler(
        event->NEW_CONNECTION
            .Connection,
        reinterpret_cast<void*>(
            ServerConnectionCallback),
        context);

    return Api->ConnectionSetConfiguration(
        event->NEW_CONNECTION
            .Connection,
        Configuration);
}

QUIC_STATUS QUIC_API
ClientStreamCallback(
    HQUIC stream,
    void* context,
    QUIC_STREAM_EVENT* event) {
    auto* state =
        static_cast<LoopState*>(
            context);

    switch (event->Type) {
        case QUIC_STREAM_EVENT_SEND_COMPLETE:
            delete static_cast<
                SendContext*>(
                event->SEND_COMPLETE
                    .ClientContext);

            state->post(
                [state] {
                    state->stream_send_done =
                        true;
                });
            break;

        case QUIC_STREAM_EVENT_RECEIVE: {
            std::vector<std::uint8_t>
                chunk;
            std::size_t total = 0;

            for (
                std::uint32_t i = 0;
                i <
                event->RECEIVE.BufferCount;
                ++i) {
                total +=
                    event->RECEIVE
                        .Buffers[i]
                        .Length;
            }

            chunk.reserve(total);

            for (
                std::uint32_t i = 0;
                i <
                event->RECEIVE.BufferCount;
                ++i) {
                const auto& buffer =
                    event->RECEIVE
                        .Buffers[i];

                chunk.insert(
                    chunk.end(),
                    buffer.Buffer,
                    buffer.Buffer +
                        buffer.Length);
            }

            const bool peer_done =
                (event->RECEIVE.Flags &
                 QUIC_RECEIVE_FLAG_FIN) !=
                0;

            append_receive(
                state,
                stream,
                std::move(chunk),
                peer_done,
                false);
            break;
        }

        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
            append_receive(
                state,
                stream,
                {},
                true,
                false);
            break;

        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            if (!event->
                    SHUTDOWN_COMPLETE
                    .AppCloseInProgress) {
                Api->StreamClose(
                    stream);
            }
            break;

        default:
            break;
    }

    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API
ClientConnectionCallback(
    HQUIC connection,
    void* context,
    QUIC_CONNECTION_EVENT* event) {
    auto* state =
        static_cast<LoopState*>(
            context);

    switch (event->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            state->post(
                [state] {
                    state->connected =
                        true;
                });
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
            state->post_failure();
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            if (!event->
                    SHUTDOWN_COMPLETE
                    .AppCloseInProgress) {
                Api->ConnectionClose(
                    connection);
            }
            break;

        default:
            break;
    }

    return QUIC_STATUS_SUCCESS;
}

void seed_remote_route(
    LoopState& state) {
    const auto capability =
        cito::detail::
            request_handler_interest_key(
                kRequestKey);

    if (
        state.host.observe_coordinator(
            9,
            9001,
            10000,
            0) !=
        cito::
            CoordinatorLeaseObservation::
                Added) {
        throw std::runtime_error(
            "coordinator seed failed");
    }

    const std::array<
        cito::InterestKey,
        1> keys{capability};

    if (!state.host.apply_remote_snapshot(
            9,
            {9001, 1, 1},
            keys)) {
        throw std::runtime_error(
            "snapshot seed failed");
    }

    const std::array<
        cito::DestinationId,
        1> destinations{77};

    if (!state.host.replace_remote_routes(
            9,
            9001,
            capability,
            destinations)) {
        throw std::runtime_error(
            "route seed failed");
    }
}

int run_server(
    std::uint16_t port,
    const char* cert,
    const char* key) {
    if (
        !open_api() ||
        !open_configuration(
            true,
            cert,
            key)) {
        return 10;
    }

    LoopState state;

    state.requests.register_handler(
        2,
        kRequestKey,
        [](cito::detail::RequestId,
           cito::detail::AppId requester,
           std::span<
               const std::uint8_t>
               payload,
           cito::detail::
               RequestReplyCore::Reply
               reply) {
            static constexpr
                std::array<
                    std::uint8_t,
                    4> expected{
                        'p','i','n','g'};

            if (
                requester != 0 ||
                payload.size() !=
                    expected.size() ||
                !std::equal(
                    payload.begin(),
                    payload.end(),
                    expected.begin())) {
                reply(
                    cito::detail::
                        RequestStatus::
                            InvalidArgument,
                    {});
                return;
            }

            static constexpr
                std::array<
                    std::uint8_t,
                    4> response{
                        'p','o','n','g'};

            reply(
                cito::detail::
                    RequestStatus::Ok,
                response);
        });

    HQUIC listener = nullptr;

    if (QUIC_FAILED(
            Api->ListenerOpen(
                Registration,
                ServerListenerCallback,
                &state,
                &listener))) {
        return 11;
    }

    QUIC_ADDR address{};
    QuicAddrSetFamily(
        &address,
        QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(
        &address,
        port);

    if (QUIC_FAILED(
            Api->ListenerStart(
                listener,
                &Alpn,
                1,
                &address))) {
        return 12;
    }

    const bool done =
        state.run_until(
            [&] {
                return
                    state.request_dispatched &&
                    state.stream_send_done;
            },
            std::chrono::seconds(8));

    if (!done) {
        return 13;
    }

    std::cout
        << "quic_request_server request=1 reply=1 loop=1"
        << std::endl;

    std::_Exit(0);
}

int run_client(
    const char* host,
    std::uint16_t port,
    const char* ca) {
    if (
        !open_api() ||
        !open_configuration(false, ca)) {
        return 20;
    }

    LoopState state;

    try {
        seed_remote_route(state);
    } catch (...) {
        return 21;
    }

    HQUIC connection = nullptr;

    if (QUIC_FAILED(
            Api->ConnectionOpen(
                Registration,
                ClientConnectionCallback,
                &state,
                &connection))) {
        return 22;
    }

    if (QUIC_FAILED(
            Api->ConnectionStart(
                connection,
                Configuration,
                QUIC_ADDRESS_FAMILY_UNSPEC,
                host,
                port))) {
        return 23;
    }

    if (!state.run_until(
            [&] {
                return state.connected;
            },
            std::chrono::seconds(6))) {
        return 24;
    }

    static constexpr
        std::array<
            std::uint8_t,
            4> ping{
                'p','i','n','g'};

    auto send =
        state.transport.start(
            1,
            kRequestKey,
            ping,
            [&](cito::detail::
                    RequestStatus status,
                std::span<
                    const std::uint8_t>
                    payload) {
                static constexpr
                    std::array<
                        std::uint8_t,
                        4> expected{
                            'p','o','n','g'};

                if (
                    status !=
                        cito::detail::
                            RequestStatus::Ok ||
                    payload.size() !=
                        expected.size() ||
                    !std::equal(
                        payload.begin(),
                        payload.end(),
                        expected.begin())) {
                    state.failed = true;
                    return;
                }

                state.reply_done = true;
            });

    if (
        !send ||
        send->destination != 77) {
        return 25;
    }

    HQUIC stream = nullptr;

    if (QUIC_FAILED(
            Api->StreamOpen(
                connection,
                QUIC_STREAM_OPEN_FLAG_NONE,
                ClientStreamCallback,
                &state,
                &stream))) {
        return 26;
    }

    if (QUIC_FAILED(
            Api->StreamStart(
                stream,
                QUIC_STREAM_START_FLAG_NONE))) {
        return 27;
    }

    if (!send_stream(
            stream,
            send->bytes)) {
        return 28;
    }

    const bool done =
        state.run_until(
            [&] {
                return
                    state.reply_done &&
                    state.stream_send_done;
            },
            std::chrono::seconds(8));

    if (!done) {
        return 29;
    }

    if (
        state.transport
            .outbound_count() != 0 ||
        state.requests
            .pending_count() != 0) {
        return 30;
    }

    std::cout
        << "quic_request_client request=1 reply=1 loop=1"
        << std::endl;

    std::_Exit(0);
}

} // namespace

int main(
    int argc,
    char** argv) {
    if (
        argc >= 2 &&
        std::strcmp(
            argv[1],
            "server") == 0) {
        if (argc != 5) {
            return 2;
        }

        return run_server(
            static_cast<
                std::uint16_t>(
                std::stoul(
                    argv[2])),
            argv[3],
            argv[4]);
    }

    if (
        argc >= 2 &&
        std::strcmp(
            argv[1],
            "client") == 0) {
        if (argc != 5) {
            return 2;
        }

        return run_client(
            argv[2],
            static_cast<
                std::uint16_t>(
                std::stoul(
                    argv[3])),
            argv[4]);
    }

    return 2;
}
