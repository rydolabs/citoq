#include <cito/cito_c.h>
#include <cito/detail/embedded_host_runtime.hpp>
#include <cito/detail/request_transport.hpp>
#include <cito/identity.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace {

struct ProviderState {
    cito_context_t* context{};
    int calls{0};
    std::uint64_t local_request_id{};
};

void provider_callback(
    void* user,
    std::uint64_t request_id,
    std::uint64_t request_type_id,
    const char* resource,
    std::size_t resource_size,
    const std::uint8_t* payload,
    std::size_t payload_size) {
    auto* state =
        static_cast<ProviderState*>(
            user);

    ++state->calls;
    state->local_request_id =
        request_id;

    assert(request_type_id == 500);
    assert(
        std::string_view(
            resource,
            resource_size) ==
        "remote_in");
    assert(payload_size == 1);
    assert(payload[0] == 7);

    const std::array<
        std::uint8_t,
        1> response{8};

    int accepted = 0;

    assert(
        cito_context_reply(
            state->context,
            request_id,
            CITO_REQUEST_OK,
            response.data(),
            response.size(),
            &accepted) ==
        CITO_STATUS_OK);

    assert(accepted == 1);
}

struct CompletionState {
    int calls{0};
    cito_request_status_t status{
        CITO_REQUEST_INTERNAL};
    std::vector<std::uint8_t>
        payload;
};

void completion_callback(
    void* user,
    cito_request_status_t status,
    const std::uint8_t* payload,
    std::size_t payload_size) {
    auto* state =
        static_cast<CompletionState*>(
            user);

    ++state->calls;
    state->status = status;
    state->payload.assign(
        payload,
        payload + payload_size);
}

cito::detail::RequestKey make_key(
    std::uint64_t type,
    std::string_view resource) {
    return {
        cito::stable_name_id(
            "acme"),
        cito::stable_name_id(
            resource),
        type};
}

void seed_remote_routes(
    cito::detail::EmbeddedHostRuntime&
        runtime,
    std::span<
        const cito::detail::RequestKey>
        keys) {
    assert(
        runtime.host()
            .observe_coordinator(
                90,
                9001,
                10000,
                0) ==
        cito::
            CoordinatorLeaseObservation::
                Added);

    std::vector<cito::InterestKey>
        capabilities;

    for (const auto& key : keys) {
        capabilities.push_back(
            cito::detail::
                request_handler_interest_key(
                    key));
    }

    assert(
        runtime.host()
            .apply_remote_snapshot(
                90,
                {9001,
                 1,
                 static_cast<
                     std::uint32_t>(
                     capabilities.size())},
                capabilities));

    const std::array<
        cito::DestinationId,
        1> destination{77};

    for (const auto& capability :
         capabilities) {
        assert(
            runtime.host()
                .replace_remote_routes(
                    90,
                    9001,
                    capability,
                    destination));
    }
}

} // namespace

int main() {
    using namespace cito::detail;

    auto shared =
        acquire_embedded_host_runtime();

    // Remote CTX1 request -> C ABI handler -> cito_context_reply().
    cito_context_t* provider =
        nullptr;

    assert(
        cito_context_create(
            "acme",
            4,
            &provider) ==
        CITO_STATUS_OK);

    ProviderState provider_state{
        provider};

    cito_handler_t* provider_handler =
        nullptr;

    assert(
        cito_context_handle(
            provider,
            500,
            "remote_in",
            9,
            provider_callback,
            &provider_state,
            &provider_handler) ==
        CITO_STATUS_OK);

    const auto inbound_key =
        make_key(
            500,
            "remote_in");

    const std::array<
        std::uint8_t,
        1> inbound_payload{7};

    const auto inbound_frame =
        encode_request_transport_request(
            900,
            inbound_key,
            inbound_payload);

    std::vector<std::uint8_t>
        inbound_reply;

    assert(
        shared->request_network()
            .receive(
                66,
                inbound_frame,
                [&](std::span<
                        const std::uint8_t>
                        reply) {
                    inbound_reply.assign(
                        reply.begin(),
                        reply.end());
                    return true;
                }));

    assert(provider_state.calls == 1);
    assert(
        provider_state.local_request_id !=
        0);
    assert(
        provider_state.local_request_id !=
        900);

    const auto decoded_inbound_reply =
        decode_request_transport_frame(
            inbound_reply);

    assert(
        decoded_inbound_reply.kind ==
        RequestTransportKind::Reply);
    assert(
        decoded_inbound_reply.request_id ==
        900);
    assert(
        decoded_inbound_reply.status ==
        RequestStatus::Ok);
    assert(
        decoded_inbound_reply.payload.size() ==
        1);
    assert(
        decoded_inbound_reply.payload[0] ==
        8);
    assert(
        shared->requests()
            .external_pending_count() ==
        0);
    assert(
        shared->request_network()
            .transport()
            .inbound_count() ==
        0);

    // C ABI requester -> separate remote runtime through the same network
    // adapter used by public C++ Context.
    EmbeddedHostRuntime server;

    const std::array<
        RequestKey,
        3> remote_keys{
            make_key(
                501,
                "remote_out"),
            make_key(
                502,
                "slow"),
            make_key(
                503,
                "destroy")};

    seed_remote_routes(
        *shared,
        remote_keys);

    RequestReplyCore::Reply slow_reply;
    RequestReplyCore::Reply destroy_reply;

    server.requests()
        .register_handler(
            201,
            remote_keys[0],
            [](RequestId,
               AppId requester_app,
               std::span<
                   const std::uint8_t>
                   payload,
               RequestReplyCore::Reply
                   reply) {
                assert(requester_app == 0);
                assert(payload.size() == 1);

                const std::array<
                    std::uint8_t,
                    1> response{
                        static_cast<
                            std::uint8_t>(
                            payload[0] + 1)};

                assert(
                    reply(
                        RequestStatus::Ok,
                        response));
            });

    server.requests()
        .register_handler(
            202,
            remote_keys[1],
            [&](RequestId,
                AppId,
                std::span<
                    const std::uint8_t>,
                RequestReplyCore::Reply
                    reply) {
                slow_reply =
                    std::move(reply);
            });

    server.requests()
        .register_handler(
            203,
            remote_keys[2],
            [&](RequestId,
                AppId,
                std::span<
                    const std::uint8_t>,
                RequestReplyCore::Reply
                    reply) {
                destroy_reply =
                    std::move(reply);
            });

    assert(
        shared->request_network()
            .attach_session(
                77,
                [&](std::span<
                        const std::uint8_t>
                        bytes) {
                    return server
                        .request_network()
                        .receive(
                            66,
                            bytes,
                            [&](std::span<
                                    const std::uint8_t>
                                    reply) {
                                return shared->
                                    request_network()
                                    .receive(
                                        77,
                                        reply);
                            });
                }));

    cito_context_t* requester =
        nullptr;

    assert(
        cito_context_create(
            "acme",
            4,
            &requester) ==
        CITO_STATUS_OK);

    int has_handler = 0;

    assert(
        cito_context_has_handler(
            requester,
            501,
            "remote_out",
            10,
            &has_handler) ==
        CITO_STATUS_OK);
    assert(has_handler == 1);

    const std::array<
        std::uint8_t,
        1> request_payload{9};

    CompletionState immediate;
    cito_request_t* request =
        nullptr;

    assert(
        cito_context_request(
            requester,
            501,
            "remote_out",
            10,
            request_payload.data(),
            request_payload.size(),
            completion_callback,
            &immediate,
            &request) ==
        CITO_STATUS_OK);

    assert(request != nullptr);
    assert(immediate.calls == 1);
    assert(
        immediate.status ==
        CITO_REQUEST_OK);
    assert(
        immediate.payload ==
        std::vector<std::uint8_t>(
            {10}));

    int cancelled = 99;

    assert(
        cito_request_cancel(
            request,
            &cancelled) ==
        CITO_STATUS_OK);
    assert(cancelled == 0);
    cito_request_destroy(request);

    // Explicit C cancellation traverses CTX1 Cancel and invalidates the
    // remote handler's delayed reply closure.
    CompletionState slow;
    request = nullptr;

    assert(
        cito_context_request(
            requester,
            502,
            "slow",
            4,
            request_payload.data(),
            request_payload.size(),
            completion_callback,
            &slow,
            &request) ==
        CITO_STATUS_OK);

    assert(slow.calls == 0);
    assert(
        shared->request_network()
            .transport()
            .outbound_count() ==
        1);
    assert(
        server.request_network()
            .transport()
            .inbound_count() ==
        1);

    cancelled = 0;

    assert(
        cito_request_cancel(
            request,
            &cancelled) ==
        CITO_STATUS_OK);

    assert(cancelled == 1);
    assert(slow.calls == 1);
    assert(
        slow.status ==
        CITO_REQUEST_CANCELLED);
    assert(
        server.request_network()
            .transport()
            .inbound_count() ==
        0);

    const std::array<
        std::uint8_t,
        1> late{42};

    assert(
        !slow_reply(
            RequestStatus::Ok,
            late));

    cito_request_destroy(request);

    // Raw C Context destruction suppresses the requester callback while
    // still sending remote cancellation and clearing both runtimes.
    cito_context_t* temporary =
        nullptr;

    assert(
        cito_context_create(
            "acme",
            4,
            &temporary) ==
        CITO_STATUS_OK);

    CompletionState destroyed;
    cito_request_t* destroyed_request =
        nullptr;

    assert(
        cito_context_request(
            temporary,
            503,
            "destroy",
            7,
            request_payload.data(),
            request_payload.size(),
            completion_callback,
            &destroyed,
            &destroyed_request) ==
        CITO_STATUS_OK);

    assert(destroyed.calls == 0);
    assert(
        shared->request_network()
            .transport()
            .outbound_count() ==
        1);
    assert(
        server.request_network()
            .transport()
            .inbound_count() ==
        1);

    cito_context_destroy(
        temporary);

    assert(destroyed.calls == 0);
    assert(
        shared->request_network()
            .transport()
            .outbound_count() ==
        0);
    assert(
        server.request_network()
            .transport()
            .inbound_count() ==
        0);
    assert(
        !destroy_reply(
            RequestStatus::Ok,
            late));

    cito_request_destroy(
        destroyed_request);

    cito_context_destroy(
        requester);

    assert(
        shared->request_network()
            .detach_session(
                77));

    cito_handler_destroy(
        provider_handler);
    cito_context_destroy(
        provider);

    return 0;
}
