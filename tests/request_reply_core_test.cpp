#include <cito/detail/request_reply_core.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

cito::detail::RequestKey key(
    std::uint64_t scope,
    std::uint64_t resource,
    std::uint64_t type) {
    return {
        scope,
        resource,
        type};
}

} // namespace

int main() {
    using namespace cito::detail;

    RequestReplyCore core;

    const auto get_status =
        key(1, 10, 100);
    const auto other_resource =
        key(1, 11, 100);
    const auto other_scope =
        key(2, 10, 100);

    assert(!core.has_handler(get_status));
    assert(core.handler_count(get_status) == 0);

    int unavailable = 0;
    const auto missing =
        core.start(
            1,
            get_status,
            {},
            [&](RequestStatus status,
                std::span<
                    const std::uint8_t>) {
                assert(
                    status ==
                    RequestStatus::
                        Unavailable);
                ++unavailable;
            });

    assert(!missing.dispatched);
    assert(missing.id == 0);
    assert(unavailable == 1);
    assert(core.pending_count() == 0);

    int first_calls = 0;
    const auto first =
        core.register_handler(
            7,
            get_status,
            [&](RequestId id,
                AppId requester,
                std::span<
                    const std::uint8_t>
                    payload,
                RequestReplyCore::Reply
                    reply) {
                ++first_calls;
                assert(id != 0);
                assert(requester == 3);
                assert(payload.size() == 1);
                assert(payload[0] == 9);

                const std::array<
                    std::uint8_t,
                    2> response{8, 4};

                assert(
                    reply(
                        RequestStatus::Ok,
                        response));
            });

    assert(first != 0);
    assert(core.has_handler(get_status));
    assert(core.handler_count(get_status) == 1);
    assert(!core.has_handler(other_resource));
    assert(!core.has_handler(other_scope));

    int completions = 0;
    std::vector<std::uint8_t>
        response_copy;

    const std::array<std::uint8_t, 1>
        request_payload{9};

    const auto started =
        core.start(
            3,
            get_status,
            request_payload,
            [&](RequestStatus status,
                std::span<
                    const std::uint8_t>
                    response) {
                ++completions;
                assert(
                    status ==
                    RequestStatus::Ok);
                response_copy.assign(
                    response.begin(),
                    response.end());
            });

    assert(started.dispatched);
    assert(started.handler_app == 7);
    assert(first_calls == 1);
    assert(completions == 1);
    assert(
        response_copy ==
        std::vector<std::uint8_t>(
            {8, 4}));
    assert(core.pending_count() == 0);

    // Multiple handlers have a deterministic v1 rule: first registered wins.
    int second_calls = 0;
    const auto second =
        core.register_handler(
            8,
            get_status,
            [&](RequestId,
                AppId,
                std::span<
                    const std::uint8_t>,
                RequestReplyCore::Reply
                    reply) {
                ++second_calls;
                assert(
                    reply(
                        RequestStatus::Ok,
                        {}));
            });

    assert(core.handler_count(get_status) == 2);

    core.start(
        3,
        get_status,
        request_payload,
        [](RequestStatus,
           std::span<
               const std::uint8_t>) {});

    assert(first_calls == 2);
    assert(second_calls == 0);

    assert(core.unregister_handler(first));

    core.start(
        3,
        get_status,
        request_payload,
        [](RequestStatus,
           std::span<
               const std::uint8_t>) {});

    assert(second_calls == 1);

    // Cancellation completes once and a late reply is ignored.
    RequestReplyCore::Reply late_reply;
    const auto slow =
        core.register_handler(
            9,
            other_resource,
            [&](RequestId,
                AppId,
                std::span<
                    const std::uint8_t>,
                RequestReplyCore::Reply
                    reply) {
                late_reply =
                    std::move(reply);
            });

    int cancelled = 0;
    const auto pending =
        core.start(
            4,
            other_resource,
            {},
            [&](RequestStatus status,
                std::span<
                    const std::uint8_t>) {
                assert(
                    status ==
                    RequestStatus::
                        Cancelled);
                ++cancelled;
            });

    assert(pending.dispatched);
    assert(core.pending_count() == 1);
    assert(core.cancel(pending.id));
    assert(cancelled == 1);
    assert(core.pending_count() == 0);
    assert(!late_reply(RequestStatus::Ok, {}));
    assert(!core.cancel(pending.id));

    // Context-style requester teardown can explicitly cancel its pending
    // correlations before unregistering the app.
    RequestReplyCore::Reply teardown_reply;
    const auto teardown_key =
        key(1, 12, 101);
    core.register_handler(
        11,
        teardown_key,
        [&](RequestId,
            AppId,
            std::span<
                const std::uint8_t>,
            RequestReplyCore::Reply reply) {
            teardown_reply =
                std::move(reply);
        });

    int teardown_cancelled = 0;
    core.start(
        12,
        teardown_key,
        {},
        [&](RequestStatus status,
            std::span<
                const std::uint8_t>) {
            assert(
                status ==
                RequestStatus::Cancelled);
            ++teardown_cancelled;
        });

    assert(core.pending_count() == 1);
    assert(core.cancel_requests(12) == 1);
    assert(teardown_cancelled == 1);
    assert(core.pending_count() == 0);
    assert(!teardown_reply(RequestStatus::Ok, {}));
    assert(core.unregister_app(11) == 1);

    // Handler exceptions become Internal and cannot leak correlation state.
    const auto throwing =
        core.register_handler(
            10,
            other_scope,
            [](RequestId,
               AppId,
               std::span<
                   const std::uint8_t>,
               RequestReplyCore::Reply) {
                throw std::runtime_error(
                    "boom");
            });

    int internal = 0;
    core.start(
        5,
        other_scope,
        {},
        [&](RequestStatus status,
            std::span<
                const std::uint8_t>) {
            assert(
                status ==
                RequestStatus::Internal);
            ++internal;
        });

    assert(internal == 1);
    assert(core.pending_count() == 0);

    // Removing a handler app fails its in-flight work as Unavailable.
    RequestReplyCore::Reply abandoned;
    const auto app_key =
        key(1, 20, 200);
    core.register_handler(
        20,
        app_key,
        [&](RequestId,
            AppId,
            std::span<
                const std::uint8_t>,
            RequestReplyCore::Reply reply) {
            abandoned =
                std::move(reply);
        });

    int app_unavailable = 0;
    core.start(
        21,
        app_key,
        {},
        [&](RequestStatus status,
            std::span<
                const std::uint8_t>) {
            assert(
                status ==
                RequestStatus::
                    Unavailable);
            ++app_unavailable;
        });

    assert(core.pending_count() == 1);
    assert(core.unregister_app(20) == 1);
    assert(app_unavailable == 1);
    assert(core.pending_count() == 0);
    assert(!abandoned(RequestStatus::Ok, {}));

    // Removing a requester drops its pending completion without calling it.
    const auto requester_key =
        key(1, 21, 201);
    RequestReplyCore::Reply requester_late;
    core.register_handler(
        30,
        requester_key,
        [&](RequestId,
            AppId,
            std::span<
                const std::uint8_t>,
            RequestReplyCore::Reply reply) {
            requester_late =
                std::move(reply);
        });

    int requester_completion = 0;
    core.start(
        31,
        requester_key,
        {},
        [&](RequestStatus,
            std::span<
                const std::uint8_t>) {
            ++requester_completion;
        });

    assert(core.pending_count() == 1);
    assert(core.unregister_app(31) == 0);
    assert(core.pending_count() == 0);
    assert(requester_completion == 0);
    assert(!requester_late(RequestStatus::Ok, {}));

    // External transports receive a process-local handler correlation id.
    // It must be distinct from the peer's wire RequestId so C/Python can
    // safely reply even when different peers reuse the same wire id.
    const auto external_key =
        key(3, 30, 300);

    RequestId external_handler_id = 0;
    RequestReplyCore::Reply
        unused_external_reply;

    const auto external_handler =
        core.register_handler(
            40,
            external_key,
            [&](RequestId id,
                AppId requester,
                std::span<
                    const std::uint8_t>
                    payload,
                RequestReplyCore::Reply
                    reply) {
                external_handler_id = id;
                unused_external_reply =
                    std::move(reply);
                assert(requester == 0);
                assert(payload.size() == 1);
                assert(payload[0] == 7);
            });

    int external_replies = 0;
    RequestStatus external_status =
        RequestStatus::Internal;
    std::vector<std::uint8_t>
        external_payload;

    const std::array<
        std::uint8_t,
        1> external_request{7};

    const auto external =
        core.dispatch_external(
            external_key,
            777,
            external_request,
            [&](RequestStatus status,
                std::span<
                    const std::uint8_t>
                    payload) {
                ++external_replies;
                external_status = status;
                external_payload.assign(
                    payload.begin(),
                    payload.end());
                return true;
            });

    assert(external.dispatched);
    assert(external.handler_app == 40);
    assert(external.handler_request_id != 0);
    assert(external.handler_request_id != 777);
    assert(
        external.handler_request_id ==
        external_handler_id);
    assert(core.external_pending_count() == 1);

    const std::array<
        std::uint8_t,
        2> external_response{8, 9};

    assert(
        core.complete_handler_reply(
            40,
            external_handler_id,
            RequestStatus::Ok,
            external_response));
    assert(external_replies == 1);
    assert(external_status == RequestStatus::Ok);
    assert(
        external_payload ==
        std::vector<std::uint8_t>(
            {8, 9}));
    assert(core.external_pending_count() == 0);
    assert(
        !core.complete_handler_reply(
            40,
            external_handler_id,
            RequestStatus::Ok,
            {}));
    assert(
        !unused_external_reply(
            RequestStatus::Ok,
            {}));

    // Remote cancel/peer loss abandons external handler correlation without
    // sending a second reply.
    RequestId abandoned_external_id = 0;

    const auto abandoned_external =
        core.dispatch_external(
            external_key,
            778,
            external_request,
            [&](RequestStatus,
                std::span<
                    const std::uint8_t>) {
                ++external_replies;
                return true;
            });

    abandoned_external_id =
        abandoned_external
            .handler_request_id;

    assert(core.external_pending_count() == 1);
    assert(
        core.abandon_external(
            abandoned_external_id));
    assert(core.external_pending_count() == 0);
    assert(
        !core.complete_handler_reply(
            40,
            abandoned_external_id,
            RequestStatus::Ok,
            {}));

    // Destroying the handler app fails its delayed external work remotely.
    RequestId teardown_external_id = 0;
    int teardown_external_replies = 0;

    core.dispatch_external(
        external_key,
        779,
        external_request,
        [&](RequestStatus status,
            std::span<
                const std::uint8_t>) {
            ++teardown_external_replies;
            assert(
                status ==
                RequestStatus::
                    Unavailable);
            return true;
        });

    // The handler callback above stores the newest local correlation id.
    teardown_external_id =
        external_handler_id;
    assert(teardown_external_id != 0);
    assert(core.external_pending_count() == 1);
    assert(core.unregister_app(40) == 1);
    assert(teardown_external_replies == 1);
    assert(core.external_pending_count() == 0);

    assert(core.unregister_handler(second));
    assert(core.unregister_handler(slow));
    assert(core.unregister_handler(throwing));
    assert(core.handler_total() == 1); // requester_key handler remains
    assert(core.unregister_app(30) == 1);
    assert(core.handler_total() == 0);

    return 0;
}
