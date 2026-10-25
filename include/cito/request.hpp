#pragma once

#include <cito/detail/request_reply_core.hpp>

#include <chrono>
#include <coroutine>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace cito {

using RequestStatus =
    detail::RequestStatus;

struct RequestOptions {
    std::chrono::milliseconds timeout{
        std::chrono::seconds(5)};
};

template <class T>
class Result {
public:
    static Result success(T value) {
        Result result(
            RequestStatus::Ok);
        result.value_.emplace(
            std::move(value));
        return result;
    }

    static Result failure(
        RequestStatus status) {
        if (status ==
            RequestStatus::Ok) {
            throw std::invalid_argument(
                "Cito Result failure status cannot be Ok");
        }

        return Result(status);
    }

    bool ok() const noexcept {
        return
            status_ ==
            RequestStatus::Ok;
    }

    explicit operator bool()
        const noexcept {
        return ok();
    }

    RequestStatus status()
        const noexcept {
        return status_;
    }

    T& value() {
        if (!value_) {
            throw std::logic_error(
                "Cito Result has no value");
        }

        return *value_;
    }

    const T& value() const {
        if (!value_) {
            throw std::logic_error(
                "Cito Result has no value");
        }

        return *value_;
    }

private:
    explicit Result(
        RequestStatus status)
        : status_(status) {}

    RequestStatus status_;
    std::optional<T> value_;
};

namespace detail {

template <class T>
struct RequestState {
    void finish(
        Result<T> result) {
        if (result_) {
            return;
        }

        result_.emplace(
            std::move(result));
        cancel_ = {};

        auto continuation =
            std::exchange(
                continuation_,
                std::coroutine_handle<>{});

        if (continuation) {
            continuation.resume();
        }
    }

    bool ready() const noexcept {
        return result_.has_value();
    }

    std::optional<Result<T>> result_;
    std::coroutine_handle<>
        continuation_{};
    std::function<bool()> cancel_;
};

} // namespace detail

template <class T>
class Request {
public:
    explicit Request(
        std::shared_ptr<
            detail::RequestState<T>>
            state)
        : state_(std::move(state)) {
        if (!state_) {
            throw std::invalid_argument(
                "Cito Request state is null");
        }
    }

    bool ready() const noexcept {
        return state_->ready();
    }

    const Result<T>& result()
        const {
        if (!state_->result_) {
            throw std::logic_error(
                "Cito Request result is not ready");
        }

        return *state_->result_;
    }

    bool cancel() {
        if (
            state_->result_ ||
            !state_->cancel_) {
            return false;
        }

        auto cancel =
            state_->cancel_;

        return cancel();
    }

    bool await_ready()
        const noexcept {
        return ready();
    }

    bool await_suspend(
        std::coroutine_handle<>
            continuation) {
        if (ready()) {
            return false;
        }

        state_->continuation_ =
            continuation;

        return true;
    }

    Result<T> await_resume() {
        if (!state_->result_) {
            throw std::logic_error(
                "Cito Request resumed without a result");
        }

        return std::move(
            *state_->result_);
    }

private:
    std::shared_ptr<
        detail::RequestState<T>>
        state_;
};

class HandlerRegistration {
public:
    HandlerRegistration() = default;

    explicit HandlerRegistration(
        std::function<void()> close)
        : close_(std::move(close)) {}

    HandlerRegistration(
        const HandlerRegistration&) =
        delete;
    HandlerRegistration& operator=(
        const HandlerRegistration&) =
        delete;

    HandlerRegistration(
        HandlerRegistration&&) noexcept =
        default;

    HandlerRegistration& operator=(
        HandlerRegistration&&) noexcept =
        default;

    // handle() registrations belong to Context lifetime. Destruction of this
    // optional handle intentionally does not unregister the handler.
    ~HandlerRegistration() = default;

    void close() {
        if (!close_) {
            return;
        }

        auto close =
            std::move(close_);
        close();
    }

private:
    std::function<void()> close_;
};

} // namespace cito
