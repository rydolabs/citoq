#pragma once

#include <cito/detail/embedded_host_runtime.hpp>
#include <cito/identity.hpp>
#include <cito/request.hpp>
#include <cito/static.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <deque>
#include <vector>

namespace cito {

class Subscription {
public:
    Subscription() = default;

    explicit Subscription(
        std::function<void()> close,
        bool close_on_destroy = true)
        : close_(std::move(close)),
          close_on_destroy_(close_on_destroy) {}

    Subscription(const Subscription&) = delete;
    Subscription& operator=(
        const Subscription&) = delete;

    Subscription(
        Subscription&& other) noexcept
        : close_(std::move(other.close_)),
          close_on_destroy_(
              other.close_on_destroy_) {
        other.close_on_destroy_ = false;
    }

    Subscription& operator=(
        Subscription&& other) noexcept {
        if (this != &other) {
            if (close_on_destroy_) {
                close();
            } else {
                close_ = {};
            }

            close_ =
                std::move(other.close_);
            close_on_destroy_ =
                other.close_on_destroy_;
            other.close_on_destroy_ =
                false;
        }
        return *this;
    }

    ~Subscription() {
        if (close_on_destroy_) {
            close();
        }
    }

    void close() {
        if (!close_) return;
        auto fn = std::move(close_);
        fn();
    }

private:
    std::function<void()> close_;
    bool close_on_destroy_{true};
};

class Context {
private:
    struct TransparentStringHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
        std::size_t operator()(const std::string& value) const noexcept {
            return (*this)(std::string_view(value));
        }
    };

    struct Handler {
        std::size_t id{};
        bool active{true};
        std::function<void(const void*)> invoke;
    };

    // Keep executing callbacks stable when subscriptions are added reentrantly.
    using Bucket = std::deque<Handler>;
    using ResourceMap = std::unordered_map<
        std::string, Bucket, TransparentStringHash, std::equal_to<>>;

    struct WireRoute {
        std::size_t count{0};
        std::function<std::size_t(std::span<const std::uint8_t>)> deliver;
    };

    using WireResourceMap = std::unordered_map<
        std::string, WireRoute, TransparentStringHash, std::equal_to<>>;

    struct Locator {
        std::type_index type{typeid(void)};
        std::string resource;
        bool wire_enabled{false};
        TypeId wire_type{};
        InterestKey wire_key{};
    };

    struct State {
        static std::shared_ptr<State> create(std::string_view scope) {
            auto state = std::shared_ptr<State>(new State(scope));
            std::weak_ptr<State> weak = state;

            const auto registered = state->runtime_->host().register_app(
                state->app_id_,
                [weak](
                    TypeId type_id,
                    std::string_view resource,
                    std::span<const std::uint8_t> payload) {
                    if (const auto locked = weak.lock()) {
                        return locked->deliver_wire(type_id, resource, payload);
                    }
                    return std::size_t{0};
                });

            if (!registered) {
                throw std::logic_error("Cito Context app registration failed");
            }

            state->registered_ = true;
            return state;
        }

        ~State() {
            for (const auto& key : published_keys_)
                runtime_->request_network().release_data_interest(key);
            runtime_->request_network()
                .cancel_requests(
                    app_id_);
            runtime_->requests()
                .cancel_requests(
                    app_id_);
            runtime_->requests()
                .unregister_app(
                    app_id_);

            for (const auto& [id, locator] : locators) {
                (void)id;
                if (locator.wire_enabled)
                    runtime_->host().unregister_resource_name(locator.wire_key);
            }
            if (registered_) {
                runtime_->host()
                    .unregister_app(
                        app_id_);
            }
        }

        template <class T, class F>
        std::size_t add(std::string resource, F&& handler) {
            const auto id = next_id++;
            const auto type = std::type_index(typeid(T));
            auto& bucket = handlers[type][resource];

            bucket.push_back(Handler{
                id,
                true,
                [fn = std::function<void(const T&)>(
                    std::forward<F>(handler))](const void* value) {
                    fn(*static_cast<const T*>(value));
                }});

            locators.emplace(id, Locator{
                type, std::move(resource), false, 0, {}});

            if constexpr (StaticCodec<T>::direct) {
                try {
                    enable_wire<T>(id);
                } catch (...) {
                    remove_now(id);
                    throw;
                }
            }

            return id;
        }

        void remove(std::size_t id) {
            const auto locator = locators.find(id);
            if (locator == locators.end()) return;

            disable_wire(locator->second);

            if (dispatch_depth == 0) {
                remove_now(id);
                return;
            }

            auto* handler = find_handler(locator->second, id);
            if (handler && handler->active) {
                handler->active = false;
                pending_removals.push_back(id);
            }
        }

        template <class T>
        void advertise(std::string_view resource) {
            if constexpr (StaticCodec<T>::direct) {
                auto key = make_key(static_type_ref<T>().type_id(), resource);
                auto& network = runtime_->request_network();
                if (published_keys_.contains(key) || !network.data_interest_enabled())
                    return;
                if (published_keys_.size() >= 256)
                    throw std::length_error("Cito: maximum published topics exceeded");
                if (!network.ensure_data_interest(key)) return;
                try {
                    published_keys_.insert(key);
                } catch (...) {
                    network.release_data_interest(key);
                    throw;
                }
            }
        }

        template <class T>
        std::size_t publish(std::string_view resource, const T& value) {
            auto delivered = publish_local(resource, value);

            if constexpr (StaticCodec<T>::direct) {
                advertise<T>(resource);
                const auto key = make_key(
                    static_type_ref<T>().type_id(), resource);
                auto& host = runtime_->host();

                if (!host.has_demand_except(app_id_, key)) {
                    return delivered;
                }

                const auto payload = StaticCodec<T>::encode(value);
                const auto result = host.publish_from(
                    app_id_,
                    key,
                    resource,
                    std::span<const std::uint8_t>(
                        payload.data(), payload.size()),
                    [this](DestinationId destination,
                           std::span<const std::uint8_t> packet) {
                        return runtime_->request_network().send_data(
                            destination, packet);
                    });

                delivered += result.delivered();
            }

            return delivered;
        }

        template <class RequestT, class F>
        HandlerRegistration handle(
            std::string resource,
            F&& handler) {
            using ReplyT =
                ReplyTypeT<RequestT>;

            const auto key =
                make_request_key<RequestT>(
                    resource);

            const auto capability =
                detail::
                    request_handler_interest_key(
                        key);

            auto token =
                runtime_->requests()
                    .register_handler(
                        app_id_,
                        key,
                        [fn =
                             std::function<
                                 ReplyT(
                                     const RequestT&)>(
                                 std::forward<F>(
                                     handler))](
                            detail::RequestId,
                            detail::AppId,
                            std::span<
                                const std::uint8_t>
                                payload,
                            detail::
                                RequestReplyCore::
                                    Reply reply) mutable {
                            try {
                                const auto request =
                                    StaticCodec<
                                        RequestT>::
                                        decode(
                                            payload);

                                auto response =
                                    fn(request);

                                const auto bytes =
                                    StaticCodec<
                                        ReplyT>::
                                        encode(
                                            response);

                                reply(
                                    detail::
                                        RequestStatus::
                                            Ok,
                                    std::span<
                                        const std::uint8_t>(
                                        bytes.data(),
                                        bytes.size()));
                            } catch (...) {
                                reply(
                                    detail::
                                        RequestStatus::
                                            Internal,
                                    {});
                            }
                        });

            try {
                const auto interest =
                    runtime_->host()
                        .add_interest(
                            app_id_,
                            capability);

                if (!interest.changed) {
                    throw std::logic_error(
                        "Cito request handler demand registration failed");
                }
            } catch (...) {
                runtime_->requests()
                    .unregister_handler(
                        token);
                throw;
            }

            std::weak_ptr<
                detail::EmbeddedHostRuntime>
                weak_runtime =
                    runtime_;

            return HandlerRegistration(
                [weak_runtime =
                     std::move(
                         weak_runtime),
                 app_id = app_id_,
                 capability,
                 token] {
                    if (const auto runtime =
                            weak_runtime
                                .lock()) {
                        runtime->requests()
                            .unregister_handler(
                                token);
                        runtime->host()
                            .remove_interest(
                                app_id,
                                capability);
                    }
                });
        }

        template <class RequestT>
        Request<
            ReplyTypeT<RequestT>>
        request(
            std::string_view resource,
            const RequestT& value,
            RequestOptions options) {
            using ReplyT =
                ReplyTypeT<RequestT>;

            auto result_state =
                std::make_shared<
                    detail::
                        RequestState<
                            ReplyT>>();

            Request<ReplyT> request(
                result_state);

            const auto key =
                make_request_key<RequestT>(
                    resource);

            auto& requests =
                runtime_->requests();
            auto& network =
                runtime_->
                    request_network();

            if (
                options.timeout.count() <
                0) {
                throw std::invalid_argument(
                    "Cito request timeout cannot be negative");
            }

            auto completion =
                [result_state](
                    detail::RequestStatus
                        status,
                    std::span<
                        const std::uint8_t>
                        response) {
                    if (
                        status !=
                        detail::
                            RequestStatus::
                                Ok) {
                        result_state->
                            finish(
                                Result<
                                    ReplyT>::
                                    failure(
                                        status));
                        return;
                    }

                    try {
                        auto value =
                            StaticCodec<
                                ReplyT>::
                                decode(
                                    response);

                        result_state->
                            finish(
                                Result<
                                    ReplyT>::
                                    success(
                                        std::move(
                                            value)));
                    } catch (...) {
                        result_state->
                            finish(
                                Result<
                                    ReplyT>::
                                    failure(
                                        RequestStatus::
                                            Internal));
                    }
                };

            // Prefer an exact same-process handler. This preserves the
            // lowest-latency path and the existing deterministic local
            // handler selection rule.
            if (requests.has_handler(
                    key)) {
                const auto payload =
                    StaticCodec<RequestT>::
                        encode(value);

                const auto started =
                    requests.start(
                        app_id_,
                        key,
                        std::span<
                            const std::uint8_t>(
                            payload.data(),
                            payload.size()),
                        completion);

                if (
                    started.dispatched &&
                    !result_state->ready()) {
                    std::weak_ptr<
                        detail::
                            EmbeddedHostRuntime>
                        weak_runtime =
                            runtime_;

                    result_state->cancel_ =
                        [weak_runtime =
                             std::move(
                                 weak_runtime),
                         id = started.id] {
                            const auto runtime =
                                weak_runtime
                                    .lock();

                            return
                                runtime &&
                                runtime->
                                    requests()
                                    .cancel(id);
                        };
                }

                return request;
            }

            // A concrete network-session manager installs a wait hook. In
            // that mode the request object is retained without serialization
            // while discovery/route/session readiness converges.
            if (network.waiting_enabled()) {
                const auto deadline =
                    detail::
                        RequestNetworkAdapter::
                            Clock::now() +
                    options.timeout;

                const auto wait_id =
                    network.defer(
                        app_id_,
                        key,
                        [value] {
                            return
                                StaticCodec<
                                    RequestT>::
                                    encode(
                                        value);
                        },
                        completion,
                        deadline);

                if (
                    wait_id != 0 &&
                    !result_state->ready()) {
                    std::weak_ptr<
                        detail::
                            EmbeddedHostRuntime>
                        weak_runtime =
                            runtime_;

                    result_state->cancel_ =
                        [weak_runtime =
                             std::move(
                                 weak_runtime),
                         wait_id] {
                            const auto runtime =
                                weak_runtime
                                    .lock();

                            return
                                runtime &&
                                runtime->
                                    request_network()
                                    .cancel_wait(
                                        wait_id);
                        };
                }

                return request;
            }

            // Compatibility path used when no network-session manager owns
            // readiness progression. It preserves the original fail-fast
            // no-session semantics for local/unit embedders.
            if (!network.can_send(
                    key)) {
                result_state->finish(
                    Result<ReplyT>::
                        failure(
                            RequestStatus::
                                Unavailable));
                return request;
            }

            const auto payload =
                StaticCodec<RequestT>::
                    encode(value);

            const auto started =
                network.start(
                    app_id_,
                    key,
                    std::span<
                        const std::uint8_t>(
                        payload.data(),
                        payload.size()),
                    completion);

            if (
                started.request_id != 0 &&
                !result_state->ready()) {
                std::weak_ptr<
                    detail::
                        EmbeddedHostRuntime>
                    weak_runtime =
                        runtime_;

                result_state->cancel_ =
                    [weak_runtime =
                         std::move(
                             weak_runtime),
                     id = started.request_id] {
                        const auto runtime =
                            weak_runtime
                                .lock();

                        return
                            runtime &&
                            runtime->
                                request_network()
                                .cancel(id);
                    };
            }

            return request;
        }

        void run() {
            runtime_->run();
        }

        void stop() noexcept {
            runtime_->stop();
        }

    private:
        explicit State(std::string_view scope)
            : scope_id_(stable_name_id(scope)),
              runtime_(detail::acquire_embedded_host_runtime()),
              app_id_(runtime_->allocate_app_id()) {}

        InterestKey make_key(
            TypeId type_id,
            std::string_view resource) const noexcept {
            return {
                scope_id_,
                stable_name_id(resource),
                type_id};
        }

        template <class RequestT>
        detail::RequestKey
        make_request_key(
            std::string_view resource)
            const {
            return {
                scope_id_,
                stable_name_id(
                    resource),
                static_type_ref<
                    RequestT>()
                    .type_id()};
        }

        template <class T>
        void enable_wire(std::size_t id) {
            auto locator_it = locators.find(id);
            if (locator_it == locators.end()) {
                throw std::logic_error("Cito subscription locator missing");
            }

            auto& locator = locator_it->second;
            const auto type_id = static_type_ref<T>().type_id();
            auto& resources = wire_routes[type_id];
            auto [route_it, inserted] =
                resources.try_emplace(locator.resource);
            auto& route = route_it->second;

            if (inserted) {
                const auto resource = locator.resource;
                route.deliver =
                    [this, resource](std::span<const std::uint8_t> payload) {
                        const auto value = StaticCodec<T>::decode(payload);
                        return publish_local(resource, value);
                    };
            }

            ++route.count;
            const auto key = make_key(type_id, locator.resource);

            try {
                const auto change =
                    runtime_->host().add_interest(app_id_, key);
                if (!change.changed) {
                    throw std::logic_error(
                        "Cito host interest registration failed");
                }
                if (!runtime_->host().register_resource_name(key, locator.resource)) {
                    runtime_->host().remove_interest(app_id_, key);
                    throw std::logic_error("Cito resource hash collision");
                }
            } catch (...) {
                if (--route.count == 0) resources.erase(route_it);
                if (resources.empty()) wire_routes.erase(type_id);
                throw;
            }

            locator.wire_enabled = true;
            locator.wire_type = type_id;
            locator.wire_key = key;
        }

        void disable_wire(Locator& locator) {
            if (!locator.wire_enabled) return;

            runtime_->host().unregister_resource_name(locator.wire_key);
            runtime_->host().remove_interest(app_id_, locator.wire_key);

            auto type_it = wire_routes.find(locator.wire_type);
            if (type_it != wire_routes.end()) {
                auto resource_it =
                    type_it->second.find(locator.resource);

                if (resource_it != type_it->second.end()) {
                    if (
                        resource_it->second.count > 0 &&
                        --resource_it->second.count == 0) {
                        type_it->second.erase(resource_it);
                    }
                }

                if (type_it->second.empty()) {
                    wire_routes.erase(type_it);
                }
            }

            locator.wire_enabled = false;
        }

        std::size_t deliver_wire(
            TypeId type_id,
            std::string_view resource,
            std::span<const std::uint8_t> payload) {
            const auto type_it = wire_routes.find(type_id);
            if (type_it == wire_routes.end()) return 0;

            const auto resource_it = type_it->second.find(resource);
            if (
                resource_it == type_it->second.end() ||
                !resource_it->second.deliver) {
                return 0;
            }

            // Copy so a callback may close its final subscription safely.
            auto deliver = resource_it->second.deliver;
            return deliver(payload);
        }

        template <class T>
        std::size_t publish_local(
            std::string_view resource,
            const T& value) {
            const auto type = std::type_index(typeid(T));
            auto type_it = handlers.find(type);
            if (type_it == handlers.end()) return 0;

            auto resource_it = type_it->second.find(resource);
            if (resource_it == type_it->second.end()) return 0;

            auto& bucket = resource_it->second;
            const auto initial_size = bucket.size();

            std::size_t delivered = 0;
            ++dispatch_depth;

            try {
                for (std::size_t i = 0; i < initial_size; ++i) {
                    if (!bucket[i].active) continue;
                    bucket[i].invoke(&value);
                    ++delivered;
                }
            } catch (...) {
                finish_dispatch();
                throw;
            }

            finish_dispatch();
            return delivered;
        }

        Handler* find_handler(const Locator& locator, std::size_t id) {
            auto type_it = handlers.find(locator.type);
            if (type_it == handlers.end()) return nullptr;

            auto resource_it =
                type_it->second.find(locator.resource);
            if (resource_it == type_it->second.end()) return nullptr;

            for (auto& handler : resource_it->second) {
                if (handler.id == id) return &handler;
            }
            return nullptr;
        }

        void remove_now(std::size_t id) {
            auto locator_it = locators.find(id);
            if (locator_it == locators.end()) return;

            disable_wire(locator_it->second);
            const auto locator = locator_it->second;

            auto type_it = handlers.find(locator.type);
            if (type_it != handlers.end()) {
                auto resource_it =
                    type_it->second.find(locator.resource);

                if (resource_it != type_it->second.end()) {
                    auto& bucket = resource_it->second;
                    bucket.erase(
                        std::remove_if(
                            bucket.begin(),
                            bucket.end(),
                            [id](const Handler& handler) {
                                return handler.id == id;
                            }),
                        bucket.end());

                    if (bucket.empty()) {
                        type_it->second.erase(resource_it);
                    }
                }

                if (type_it->second.empty()) {
                    handlers.erase(type_it);
                }
            }

            locators.erase(locator_it);
        }

        void finish_dispatch() {
            --dispatch_depth;
            if (dispatch_depth != 0) return;

            auto removals = std::move(pending_removals);
            pending_removals.clear();

            for (const auto id : removals) {
                remove_now(id);
            }
        }

        ScopeId scope_id_{};
        std::shared_ptr<detail::EmbeddedHostRuntime> runtime_;
        detail::AppId app_id_{};
        bool registered_{false};

        std::size_t next_id{1};
        std::size_t dispatch_depth{0};

        std::unordered_map<std::type_index, ResourceMap> handlers;
        std::unordered_map<TypeId, WireResourceMap> wire_routes;
        std::unordered_map<std::size_t, Locator> locators;
        std::vector<std::size_t> pending_removals;
        std::unordered_set<InterestKey, InterestKeyHash> published_keys_;
    };

public:
    Context() : Context(std::string{}) {}

    explicit Context(std::string scope)
        : scope_(std::move(scope)),
          state_(State::create(scope_)) {}

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) noexcept = default;
    Context& operator=(Context&&) noexcept = default;

    const std::string& scope() const noexcept {
        return scope_;
    }

    template <class T, class F>
    Subscription subscribe(F&& handler) {
        return subscribe<T>(
            "",
            std::forward<F>(handler));
    }

    template <class T, class F>
    Subscription subscribe(
        std::string resource,
        F&& handler) {
        const auto id =
            state_->add<T>(
                std::move(resource),
                std::forward<F>(handler));

        // Intent-first subscribe(): ignoring the returned handle must not
        // unsubscribe at the end of the statement. The Context owns the
        // registration; the handle is only for explicit early close().
        return make_subscription(
            id,
            false);
    }

    // Compatibility API: on() keeps the original scoped/RAII lifetime.
    template <class T, class F>
    Subscription on(F&& handler) {
        return on<T>(
            "",
            std::forward<F>(handler));
    }

    template <class T, class F>
    Subscription on(
        std::string resource,
        F&& handler) {
        const auto id =
            state_->add<T>(
                std::move(resource),
                std::forward<F>(handler));

        return make_subscription(
            id,
            true);
    }

    template <class RequestT, class F>
    HandlerRegistration handle(
        F&& handler) {
        return handle<RequestT>(
            "",
            std::forward<F>(
                handler));
    }

    template <class RequestT, class F>
    HandlerRegistration handle(
        std::string resource,
        F&& handler) {
        return state_->
            template handle<RequestT>(
                std::move(resource),
                std::forward<F>(
                    handler));
    }

    template <class RequestT>
    Request<ReplyTypeT<RequestT>>
    request(
        const RequestT& value) {
        return request(
            "",
            value,
            {});
    }

    template <class RequestT>
    Request<ReplyTypeT<RequestT>>
    request(
        const RequestT& value,
        RequestOptions options) {
        return request(
            "",
            value,
            options);
    }

    template <class RequestT>
    Request<ReplyTypeT<RequestT>>
    request(
        std::string_view resource,
        const RequestT& value) {
        return request(
            resource,
            value,
            {});
    }

    template <class RequestT>
    Request<ReplyTypeT<RequestT>>
    request(
        std::string_view resource,
        const RequestT& value,
        RequestOptions options) {
        return state_->
            template request<RequestT>(
                resource,
                value,
                options);
    }

    template <class T>
    void advertise(std::string_view resource = {}) {
        state_->template advertise<T>(resource);
    }

    template <class T>
    std::size_t publish(const T& value) {
        return publish("", value);
    }

    template <class T>
    std::size_t publish(
        std::string_view resource,
        const T& value) {
        return state_->publish(resource, value);
    }

    void run() {
        if (state_) {
            state_->run();
        }
    }

    void stop() noexcept {
        if (state_) {
            state_->stop();
        }
    }

private:
    Subscription make_subscription(
        std::size_t id,
        bool close_on_destroy) {
        std::weak_ptr<State> weak =
            state_;

        return Subscription(
            [weak = std::move(weak), id] {
                if (const auto state =
                        weak.lock()) {
                    state->remove(id);
                }
            },
            close_on_destroy);
    }

    std::string scope_;
    std::shared_ptr<State> state_;
};

} // namespace cito
