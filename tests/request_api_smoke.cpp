#include "request_reply.hpp"

#include <cito/cito.hpp>

#include <cassert>
#include <coroutine>
#include <exception>
#include <type_traits>

namespace {

struct FireAndForget {
    struct promise_type {
        FireAndForget get_return_object() {
            return {};
        }

        std::suspend_never initial_suspend()
            const noexcept {
            return {};
        }

        std::suspend_never final_suspend()
            const noexcept {
            return {};
        }

        void return_void() const noexcept {}

        void unhandled_exception() {
            std::terminate();
        }
    };
};

FireAndForget await_status(
    cito::Context& requester,
    bool& completed) {
    auto result =
        co_await requester.request(
            "drone_17",
            acme::GetStatus{
                .drone_id =
                    "drone_17"});

    assert(result);
    assert(
        result.status() ==
        cito::RequestStatus::Ok);
    assert(
        result.value().battery ==
        84);
    assert(
        result.value().armed);

    completed = true;
}

} // namespace

int main() {
    static_assert(
        std::is_same_v<
            cito::ReplyTypeT<
                acme::GetStatus>,
            acme::Status>);

    cito::Context provider("acme");
    cito::Context requester("acme");
    cito::Context isolated("other");

    int handler_calls = 0;

    // handle() belongs to Context lifetime. The return value is optional.
    provider.handle<
        acme::GetStatus>(
        "drone_17",
        [&](const acme::GetStatus&
                request) {
            ++handler_calls;
            assert(
                request.drone_id ==
                "drone_17");

            return acme::Status{
                .battery = 84,
                .armed = true};
        });

    auto immediate =
        requester.request(
            "drone_17",
            acme::GetStatus{
                .drone_id =
                    "drone_17"});

    assert(immediate.ready());
    assert(immediate.result());
    assert(
        immediate.result()
            .value()
            .battery == 84);
    assert(handler_calls == 1);

    bool coroutine_completed =
        false;
    await_status(
        requester,
        coroutine_completed);
    assert(coroutine_completed);
    assert(handler_calls == 2);

    // Exact resource and scope matching are part of handler demand.
    auto wrong_resource =
        requester.request(
            "drone_18",
            acme::GetStatus{
                .drone_id =
                    "drone_18"});

    assert(wrong_resource.ready());
    assert(!wrong_resource.result());
    assert(
        wrong_resource.result()
            .status() ==
        cito::RequestStatus::
            Unavailable);

    auto wrong_scope =
        isolated.request(
            "drone_17",
            acme::GetStatus{
                .drone_id =
                    "drone_17"});

    assert(wrong_scope.ready());
    assert(!wrong_scope.result());
    assert(
        wrong_scope.result()
            .status() ==
        cito::RequestStatus::
            Unavailable);

    // The optional registration handle is only for explicit early removal.
    auto temporary =
        provider.handle<
            acme::GetStatus>(
            "temporary",
            [](const acme::GetStatus&) {
                return acme::Status{
                    .battery = 1,
                    .armed = false};
            });

    auto before_close =
        requester.request(
            "temporary",
            acme::GetStatus{});
    assert(before_close.result());

    temporary.close();

    auto after_close =
        requester.request(
            "temporary",
            acme::GetStatus{});
    assert(!after_close.result());
    assert(
        after_close.result()
            .status() ==
        cito::RequestStatus::
            Unavailable);

    return 0;
}
