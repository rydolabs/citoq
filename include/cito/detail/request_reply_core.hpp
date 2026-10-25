#pragma once

#include <cito/detail/host_runtime_core.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cito::detail {

using RequestId = std::uint64_t;
using HandlerToken = std::uint64_t;

struct RequestKey {
    ScopeId scope{};
    ResourceId resource{};
    TypeId request_type{};

    bool operator==(const RequestKey&) const noexcept = default;
};

struct RequestKeyHash {
    std::size_t operator()(const RequestKey& key) const noexcept {
        return InterestKeyHash{}(
            InterestKey{
                key.scope,
                key.resource,
                key.request_type});
    }
};

inline TypeId request_handler_capability_type(
    TypeId request_type) noexcept {
    // Internal demand identity for "a handler exists for RequestT".
    // Keep this distinct from ordinary Pub/Sub demand for RequestT while
    // preserving the canonical 3-part InterestKey shape.
    constexpr std::uint64_t basis =
        14695981039346656037ull;
    constexpr std::uint64_t prime =
        1099511628211ull;
    constexpr char tag[] =
        "cito.request.handler.v1";

    auto hash = basis;

    for (std::size_t i = 0;
         i + 1 < sizeof(tag);
         ++i) {
        hash ^=
            static_cast<
                unsigned char>(
                tag[i]);
        hash *= prime;
    }

    for (int i = 0; i < 8; ++i) {
        hash ^=
            static_cast<std::uint8_t>(
                (request_type >>
                 (i * 8)) &
                0xffu);
        hash *= prime;
    }

    return hash;
}

inline InterestKey request_handler_interest_key(
    const RequestKey& key) noexcept {
    return {
        key.scope,
        key.resource,
        request_handler_capability_type(
            key.request_type)};
}

enum class RequestStatus : std::uint8_t {
    Ok = 0,
    Cancelled = 1,
    Unavailable = 2,
    InvalidArgument = 3,
    DeadlineExceeded = 4,
    Internal = 5
};

struct RequestStart {
    RequestId id{};
    AppId handler_app{};
    bool dispatched{false};
};

struct ExternalDispatch {
    RequestId handler_request_id{};
    AppId handler_app{};
    bool dispatched{false};
};

class RequestReplyCore {
public:
    using Completion =
        std::function<void(
            RequestStatus,
            std::span<const std::uint8_t>)>;

    using Reply =
        std::function<bool(
            RequestStatus,
            std::span<const std::uint8_t>)>;

    using Handler =
        std::function<void(
            RequestId,
            AppId,
            std::span<const std::uint8_t>,
            Reply)>;

private:
    struct HandlerEntry {
        HandlerToken token{};
        AppId app{};
        Handler handler;
    };

    struct HandlerLocator {
        RequestKey key{};
        AppId app{};
    };

    struct Pending {
        AppId requester_app{};
        AppId handler_app{};
        Completion completion;
    };

    struct ExternalPending {
        AppId handler_app{};
        Reply reply;
    };

public:
    HandlerToken register_handler(
        AppId app,
        const RequestKey& key,
        Handler handler) {
        if (app == 0) {
            throw std::invalid_argument(
                "Cito request handler app id is zero");
        }

        if (!handler) {
            throw std::invalid_argument(
                "Cito request handler is empty");
        }

        const auto token =
            next_handler_token_++;

        handlers_[key].push_back(
            HandlerEntry{
                token,
                app,
                std::move(handler)});

        handler_locators_.emplace(
            token,
            HandlerLocator{
                key,
                app});

        return token;
    }

    bool unregister_handler(
        HandlerToken token) {
        const auto locator_it =
            handler_locators_.find(token);

        if (locator_it ==
            handler_locators_.end()) {
            return false;
        }

        const auto locator =
            locator_it->second;

        auto bucket_it =
            handlers_.find(
                locator.key);

        if (bucket_it != handlers_.end()) {
            auto& bucket =
                bucket_it->second;

            bucket.erase(
                std::remove_if(
                    bucket.begin(),
                    bucket.end(),
                    [token](
                        const HandlerEntry&
                            entry) {
                        return
                            entry.token ==
                            token;
                    }),
                bucket.end());

            if (bucket.empty()) {
                handlers_.erase(
                    bucket_it);
            }
        }

        handler_locators_.erase(
            locator_it);

        return true;
    }

    std::size_t unregister_app(
        AppId app) {
        std::vector<HandlerToken>
            tokens;

        for (const auto& [token, locator] :
             handler_locators_) {
            if (locator.app == app) {
                tokens.push_back(token);
            }
        }

        for (const auto token : tokens) {
            unregister_handler(token);
        }

        std::vector<RequestId>
            requester_pending;
        std::vector<RequestId>
            handler_pending;

        for (const auto& [id, pending] :
             pending_) {
            if (pending.requester_app == app) {
                requester_pending.push_back(
                    id);
            } else if (
                pending.handler_app == app) {
                handler_pending.push_back(
                    id);
            }
        }

        for (const auto id :
             requester_pending) {
            pending_.erase(id);
        }

        for (const auto id :
             handler_pending) {
            complete(
                id,
                RequestStatus::Unavailable,
                {});
        }

        std::vector<RequestId>
            external_pending;

        for (const auto& [id, pending] :
             external_pending_) {
            if (pending.handler_app == app) {
                external_pending.push_back(
                    id);
            }
        }

        for (const auto id :
             external_pending) {
            complete_external(
                id,
                RequestStatus::Unavailable,
                {});
        }

        return tokens.size();
    }

    bool has_handler(
        const RequestKey& key) const noexcept {
        const auto it =
            handlers_.find(key);

        return
            it != handlers_.end() &&
            !it->second.empty();
    }

    std::size_t handler_count(
        const RequestKey& key) const noexcept {
        const auto it =
            handlers_.find(key);

        return
            it == handlers_.end()
                ? 0
                : it->second.size();
    }

    RequestStart start_remote(
        AppId requester_app,
        Completion completion) {
        if (requester_app == 0) {
            throw std::invalid_argument(
                "Cito remote requester app id is zero");
        }

        if (!completion) {
            throw std::invalid_argument(
                "Cito remote request completion is empty");
        }

        const auto id =
            next_request_id_++;

        pending_.emplace(
            id,
            Pending{
                requester_app,
                0,
                std::move(completion)});

        return {
            id,
            0,
            true};
    }

    ExternalDispatch
    dispatch_external(
        const RequestKey& key,
        RequestId wire_request_id,
        std::span<const std::uint8_t>
            payload,
        Reply reply) {
        if (wire_request_id == 0) {
            throw std::invalid_argument(
                "Cito external wire request id is zero");
        }

        if (!reply) {
            throw std::invalid_argument(
                "Cito external reply callback is empty");
        }

        const auto bucket_it =
            handlers_.find(key);

        if (
            bucket_it ==
                handlers_.end() ||
            bucket_it->second.empty()) {
            reply(
                RequestStatus::Unavailable,
                {});
            return {};
        }

        const auto selected =
            bucket_it->second.front();

        const auto handler_request_id =
            next_request_id_++;

        external_pending_.emplace(
            handler_request_id,
            ExternalPending{
                selected.app,
                std::move(reply)});

        auto handler =
            selected.handler;

        Reply handler_reply =
            [this,
             handler_request_id](
                RequestStatus status,
                std::span<
                    const std::uint8_t>
                    response) {
                return complete_external(
                    handler_request_id,
                    status,
                    response);
            };

        try {
            handler(
                handler_request_id,
                0,
                payload,
                std::move(
                    handler_reply));
        } catch (...) {
            complete_external(
                handler_request_id,
                RequestStatus::Internal,
                {});
        }

        return {
            handler_request_id,
            selected.app,
            true};
    }

    RequestStart start(
        AppId requester_app,
        const RequestKey& key,
        std::span<const std::uint8_t>
            payload,
        Completion completion) {
        if (requester_app == 0) {
            throw std::invalid_argument(
                "Cito requester app id is zero");
        }

        if (!completion) {
            throw std::invalid_argument(
                "Cito request completion is empty");
        }

        const auto bucket_it =
            handlers_.find(key);

        if (
            bucket_it ==
                handlers_.end() ||
            bucket_it->second.empty()) {
            completion(
                RequestStatus::Unavailable,
                {});

            return {};
        }

        // v1 deterministic rule: first registered active handler wins.
        // Later load-balancing policy can replace selection without changing
        // Request/Reply correlation semantics.
        const auto selected =
            bucket_it->second.front();

        const auto id =
            next_request_id_++;

        pending_.emplace(
            id,
            Pending{
                requester_app,
                selected.app,
                std::move(completion)});

        auto handler =
            selected.handler;

        Reply reply =
            [this, id](
                RequestStatus status,
                std::span<
                    const std::uint8_t>
                    response) {
                return complete(
                    id,
                    status,
                    response);
            };

        try {
            handler(
                id,
                requester_app,
                payload,
                std::move(reply));
        } catch (...) {
            complete(
                id,
                RequestStatus::Internal,
                {});
        }

        return {
            id,
            selected.app,
            true};
    }

    bool complete(
        RequestId id,
        RequestStatus status,
        std::span<const std::uint8_t>
            payload) {
        const auto it =
            pending_.find(id);

        if (it == pending_.end()) {
            return false;
        }

        auto completion =
            std::move(
                it->second.completion);

        pending_.erase(it);

        completion(
            status,
            payload);

        return true;
    }

    bool complete_remote(
        RequestId id,
        RequestStatus status,
        std::span<const std::uint8_t>
            payload) {
        const auto it =
            pending_.find(id);

        if (
            it == pending_.end() ||
            it->second.handler_app != 0) {
            return false;
        }

        return complete(
            id,
            status,
            payload);
    }

    bool complete_external(
        RequestId id,
        RequestStatus status,
        std::span<const std::uint8_t>
            payload) {
        const auto it =
            external_pending_.find(
                id);

        if (it ==
            external_pending_.end()) {
            return false;
        }

        auto reply =
            std::move(
                it->second.reply);

        external_pending_.erase(it);

        return reply(
            status,
            payload);
    }

    bool abandon_external(
        RequestId id) {
        return
            external_pending_.erase(
                id) != 0;
    }

    bool abandon(
        RequestId id) {
        return
            pending_.erase(id) != 0;
    }

    bool complete_from(
        AppId handler_app,
        RequestId id,
        RequestStatus status,
        std::span<const std::uint8_t>
            payload) {
        const auto it =
            pending_.find(id);

        if (
            it == pending_.end() ||
            it->second.handler_app !=
                handler_app) {
            return false;
        }

        return complete(
            id,
            status,
            payload);
    }

    bool complete_handler_reply(
        AppId handler_app,
        RequestId id,
        RequestStatus status,
        std::span<const std::uint8_t>
            payload) {
        const auto local =
            pending_.find(id);

        if (
            local != pending_.end() &&
            local->second.handler_app ==
                handler_app) {
            return complete(
                id,
                status,
                payload);
        }

        const auto external =
            external_pending_.find(
                id);

        if (
            external ==
                external_pending_.end() ||
            external->second.handler_app !=
                handler_app) {
            return false;
        }

        return complete_external(
            id,
            status,
            payload);
    }

    bool cancel(
        RequestId id) {
        return complete(
            id,
            RequestStatus::Cancelled,
            {});
    }

    std::size_t cancel_requests(
        AppId requester_app,
        RequestStatus status =
            RequestStatus::Cancelled) {
        std::vector<RequestId> ids;

        for (const auto& [id, pending] :
             pending_) {
            if (
                pending.requester_app ==
                requester_app) {
                ids.push_back(id);
            }
        }

        for (const auto id : ids) {
            complete(
                id,
                status,
                {});
        }

        return ids.size();
    }

    std::size_t pending_count()
        const noexcept {
        return pending_.size();
    }

    std::size_t handler_total()
        const noexcept {
        return handler_locators_.size();
    }

    std::size_t external_pending_count()
        const noexcept {
        return external_pending_.size();
    }

private:
    HandlerToken next_handler_token_{1};
    RequestId next_request_id_{1};

    std::unordered_map<
        RequestKey,
        std::vector<HandlerEntry>,
        RequestKeyHash> handlers_;

    std::unordered_map<
        HandlerToken,
        HandlerLocator>
        handler_locators_;

    std::unordered_map<
        RequestId,
        Pending> pending_;

    std::unordered_map<
        RequestId,
        ExternalPending>
        external_pending_;
};

} // namespace cito::detail
