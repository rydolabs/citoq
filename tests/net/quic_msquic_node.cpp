#include <cito/data_packet.hpp>
#include <msquic.h>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace {
const QUIC_API_TABLE* Api = nullptr;
HQUIC Registration = nullptr;
HQUIC Configuration = nullptr;
const std::uint8_t AlpnBytes[] = {'c','i','t','o','/','1'};
const QUIC_BUFFER Alpn{sizeof(AlpnBytes), const_cast<std::uint8_t*>(AlpnBytes)};

struct State {
    std::mutex mutex;
    std::condition_variable cv;
    bool connected{false};
    bool datagram_ready{false};
    bool stream_received{false};
    bool datagram_received{false};
    bool stream_send_done{false};
    bool datagram_send_done{false};
    bool failed{false};
    std::vector<std::uint8_t> stream_bytes;
};

struct SendContext {
    explicit SendContext(std::vector<std::uint8_t> data) : bytes(std::move(data)) {
        buffer.Length = static_cast<std::uint32_t>(bytes.size());
        buffer.Buffer = bytes.data();
    }
    std::vector<std::uint8_t> bytes;
    QUIC_BUFFER buffer{};
};

bool verify_packet(std::span<const std::uint8_t> bytes) {
    try {
        const auto packet = cito::decode_data_packet_view(bytes);
        const cito::InterestKey expected{7,17,100};
        static constexpr char text[] = "hello-quic";
        if (!(packet.key == expected) || packet.payload.size() != sizeof(text) - 1) return false;
        return std::memcmp(packet.payload.data(), text, sizeof(text) - 1) == 0;
    } catch (...) { return false; }
}

void fail(State* state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->failed = true;
    state->cv.notify_all();
}

QUIC_STATUS QUIC_API ServerStreamCallback(HQUIC Stream, void* context, QUIC_STREAM_EVENT* event) {
    auto* state = static_cast<State*>(context);
    switch (event->Type) {
        case QUIC_STREAM_EVENT_RECEIVE: {
            std::lock_guard<std::mutex> lock(state->mutex);
            for (std::uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
                const auto& b = event->RECEIVE.Buffers[i];
                state->stream_bytes.insert(state->stream_bytes.end(), b.Buffer, b.Buffer + b.Length);
            }
            if ((event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0) {
                state->stream_received = verify_packet(state->stream_bytes);
                if (!state->stream_received) state->failed = true;
                state->cv.notify_all();
            }
            break;
        }
        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->stream_received) {
                state->stream_received = verify_packet(state->stream_bytes);
                if (!state->stream_received) state->failed = true;
                state->cv.notify_all();
            }
            break;
        }
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            if (!event->SHUTDOWN_COMPLETE.AppCloseInProgress) Api->StreamClose(Stream);
            break;
        default: break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API ServerConnectionCallback(HQUIC Connection, void* context, QUIC_CONNECTION_EVENT* event) {
    auto* state = static_cast<State*>(context);
    switch (event->Type) {
        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
            Api->SetCallbackHandler(event->PEER_STREAM_STARTED.Stream,
                reinterpret_cast<void*>(ServerStreamCallback), state);
            break;
        case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED: {
            const auto* buffer = event->DATAGRAM_RECEIVED.Buffer;
            const bool ok = verify_packet(std::span<const std::uint8_t>(buffer->Buffer, buffer->Length));
            std::lock_guard<std::mutex> lock(state->mutex);
            state->datagram_received = ok;
            if (!ok) state->failed = true;
            state->cv.notify_all();
            break;
        }
        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            if (!event->SHUTDOWN_COMPLETE.AppCloseInProgress) Api->ConnectionClose(Connection);
            break;
        default: break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API ServerListenerCallback(HQUIC, void* context, QUIC_LISTENER_EVENT* event) {
    if (event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION) return QUIC_STATUS_NOT_SUPPORTED;
    Api->SetCallbackHandler(event->NEW_CONNECTION.Connection,
        reinterpret_cast<void*>(ServerConnectionCallback), context);
    return Api->ConnectionSetConfiguration(event->NEW_CONNECTION.Connection, Configuration);
}

QUIC_STATUS QUIC_API ClientStreamCallback(HQUIC Stream, void* context, QUIC_STREAM_EVENT* event) {
    auto* state = static_cast<State*>(context);
    switch (event->Type) {
        case QUIC_STREAM_EVENT_SEND_COMPLETE:
            delete static_cast<SendContext*>(event->SEND_COMPLETE.ClientContext);
            { std::lock_guard<std::mutex> lock(state->mutex); state->stream_send_done = true; state->cv.notify_all(); }
            break;
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            if (!event->SHUTDOWN_COMPLETE.AppCloseInProgress) Api->StreamClose(Stream);
            break;
        default: break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API ClientConnectionCallback(HQUIC Connection, void* context, QUIC_CONNECTION_EVENT* event) {
    auto* state = static_cast<State*>(context);
    switch (event->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            { std::lock_guard<std::mutex> lock(state->mutex); state->connected = true; state->cv.notify_all(); }
            break;
        case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED:
            if (event->DATAGRAM_STATE_CHANGED.SendEnabled) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->datagram_ready = true; state->cv.notify_all();
            }
            break;
        case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
            if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(event->DATAGRAM_SEND_STATE_CHANGED.State)) {
                delete static_cast<SendContext*>(event->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
                std::lock_guard<std::mutex> lock(state->mutex);
                state->datagram_send_done = true; state->cv.notify_all();
            }
            break;
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
            std::cerr << "quic_client_transport_shutdown status="
                      << static_cast<long long>(event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status)
                      << "\n";
            fail(state);
            break;
        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            if (!event->SHUTDOWN_COMPLETE.AppCloseInProgress) Api->ConnectionClose(Connection);
            break;
        default: break;
    }
    return QUIC_STATUS_SUCCESS;
}

bool open_api() {
    if (QUIC_FAILED(MsQuicOpen2(&Api))) return false;
    const QUIC_REGISTRATION_CONFIG registration{"cito-quic-smoke", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
    return QUIC_SUCCEEDED(Api->RegistrationOpen(&registration, &Registration));
}

bool open_configuration(bool server, const char* cert = nullptr, const char* key = nullptr,
                        const char* client_cert = nullptr, const char* client_key = nullptr) {
    QUIC_SETTINGS settings{};
    settings.IdleTimeoutMs = 5000;
    settings.IsSet.IdleTimeoutMs = TRUE;
    settings.PeerBidiStreamCount = 4;
    settings.IsSet.PeerBidiStreamCount = TRUE;
    settings.DatagramReceiveEnabled = TRUE;
    settings.IsSet.DatagramReceiveEnabled = TRUE;
    if (QUIC_FAILED(Api->ConfigurationOpen(Registration, &Alpn, 1, &settings, sizeof(settings), nullptr, &Configuration))) return false;
    QUIC_CREDENTIAL_CONFIG credentials{};
    QUIC_CERTIFICATE_FILE files{};
    if (server) {
        files.CertificateFile = cert;
        files.PrivateKeyFile = key;
        credentials.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
        credentials.CertificateFile = &files;
        credentials.Flags = QUIC_CREDENTIAL_FLAG_NONE;
    } else {
        if (!cert || !*cert) return false;
        if ((client_cert == nullptr) != (client_key == nullptr)) return false;
        if (client_cert && client_key) {
            files.CertificateFile = client_cert;
            files.PrivateKeyFile = client_key;
            credentials.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
            credentials.CertificateFile = &files;
        } else {
            credentials.Type = QUIC_CREDENTIAL_TYPE_NONE;
        }
        credentials.Flags = static_cast<QUIC_CREDENTIAL_FLAGS>(
            QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE);
        credentials.CaCertificateFile = cert;
    }
    return QUIC_SUCCEEDED(Api->ConfigurationLoadCredential(Configuration, &credentials));
}

std::vector<std::uint8_t> make_payload() {
    cito::DataPacket packet{{7,17,100}, {'h','e','l','l','o','-','q','u','i','c'}};
    return cito::encode_data_packet(packet);
}

int run_server(std::uint16_t port, const char* cert, const char* key) {
    if (!open_api() || !open_configuration(true, cert, key)) return 10;
    State state;
    HQUIC listener = nullptr;
    if (QUIC_FAILED(Api->ListenerOpen(Registration, ServerListenerCallback, &state, &listener))) return 11;
    QUIC_ADDR address{};
    QuicAddrSetFamily(&address, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&address, port);
    if (QUIC_FAILED(Api->ListenerStart(listener, &Alpn, 1, &address))) return 12;
    std::unique_lock<std::mutex> lock(state.mutex);
    const bool done = state.cv.wait_for(lock, std::chrono::seconds(8), [&] {
        return state.failed || (state.stream_received && state.datagram_received);
    });
    if (!done || state.failed) return 13;
    std::cout << "quic_server stream=1 datagram=1" << std::endl;
    std::_Exit(0);
}

int run_client(const char* host, std::uint16_t port, const char* ca,
               const char* client_cert, const char* client_key) {
    if (!open_api() || !open_configuration(false, ca, nullptr, client_cert, client_key)) return 20;
    State state;
    HQUIC connection = nullptr;
    if (QUIC_FAILED(Api->ConnectionOpen(Registration, ClientConnectionCallback, &state, &connection))) return 21;
    if (QUIC_FAILED(Api->ConnectionStart(connection, Configuration, QUIC_ADDRESS_FAMILY_UNSPEC, host, port))) return 22;
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        if (!state.cv.wait_for(lock, std::chrono::seconds(6), [&] {
                return state.failed || (state.connected && state.datagram_ready);
            }) || state.failed) return 23;
    }
    const auto payload = make_payload();
    HQUIC stream = nullptr;
    if (QUIC_FAILED(Api->StreamOpen(connection, QUIC_STREAM_OPEN_FLAG_NONE, ClientStreamCallback, &state, &stream))) return 24;
    if (QUIC_FAILED(Api->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE))) return 25;
    auto* stream_send = new SendContext(payload);
    if (QUIC_FAILED(Api->StreamSend(stream, &stream_send->buffer, 1, QUIC_SEND_FLAG_FIN, stream_send))) { delete stream_send; return 26; }
    auto* datagram_send = new SendContext(payload);
    if (QUIC_FAILED(Api->DatagramSend(connection, &datagram_send->buffer, 1, QUIC_SEND_FLAG_NONE, datagram_send))) { delete datagram_send; return 27; }
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        if (!state.cv.wait_for(lock, std::chrono::seconds(6), [&] {
                return state.failed || (state.stream_send_done && state.datagram_send_done);
            }) || state.failed) return 28;
    }
    std::cout << "quic_client stream=1 datagram=1" << std::endl;
    std::_Exit(0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "server") == 0) {
        if (argc != 5) return 2;
        return run_server(static_cast<std::uint16_t>(std::stoul(argv[2])), argv[3], argv[4]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "client") == 0) {
        if (argc != 5 && argc != 7) return 2;
        return run_client(argv[2], static_cast<std::uint16_t>(std::stoul(argv[3])), argv[4],
                          argc == 7 ? argv[5] : nullptr, argc == 7 ? argv[6] : nullptr);
    }
    return 2;
}
