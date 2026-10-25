#include <cito/cito_c.h>

#include <cito/detail/embedded_host_runtime.hpp>
#include <cito/detail/raw_context.hpp>
#include <cito/identity.hpp>

#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <unordered_set>

namespace {

bool valid_bytes(
    const void* data,
    size_t size) noexcept {
    return
        size == 0 ||
        data != nullptr;
}

bool valid_request_status(
    cito_request_status_t status) noexcept {
    switch (status) {
        case CITO_REQUEST_OK:
        case CITO_REQUEST_CANCELLED:
        case CITO_REQUEST_UNAVAILABLE:
        case CITO_REQUEST_INVALID_ARGUMENT:
        case CITO_REQUEST_DEADLINE_EXCEEDED:
        case CITO_REQUEST_INTERNAL:
            return true;
    }

    return false;
}

cito::detail::RequestStatus
to_request_status(
    cito_request_status_t status) noexcept {
    return static_cast<
        cito::detail::RequestStatus>(
            static_cast<unsigned>(
                status));
}

cito_request_status_t
from_request_status(
    cito::detail::RequestStatus status) noexcept {
    return static_cast<
        cito_request_status_t>(
            static_cast<unsigned>(
                status));
}

std::string copy_text(
    const char* data,
    size_t size) {
    return
        size == 0
            ? std::string{}
            : std::string(
                data,
                size);
}

std::span<const std::uint8_t>
payload_view(
    const std::uint8_t* payload,
    size_t payload_size) noexcept {
    return
        payload_size == 0
            ? std::span<
                const std::uint8_t>{}
            : std::span<
                const std::uint8_t>(
                payload,
                payload_size);
}

struct ContextState {
    explicit ContextState(
        std::string scope)
        : scope_(std::move(scope)),
          scope_id_(
              cito::stable_name_id(
                  scope_)),
          runtime_(
              cito::detail::
                  acquire_embedded_host_runtime()),
          app_id_(
              runtime_->allocate_app_id()),
          local_(scope_) {
        const auto registered =
            runtime_->host().register_app(
                app_id_,
                [this](
                    cito::TypeId type_id,
                    std::string_view resource,
                    std::span<
                        const std::uint8_t>
                        payload) {
                    return local_.publish(
                        type_id,
                        resource,
                        payload);
                });

        if (!registered) {
            throw std::logic_error(
                "Cito C ABI app registration failed");
        }
    }

    ~ContextState() {
        for (const auto& [key, count] : subscribed_names_) {
            for (std::size_t i = 0; i < count; ++i)
                runtime_->host().unregister_resource_name(key);
        }
        for (const auto& key : published_keys_)
            runtime_->request_network().release_data_interest(key);
        // Raw C Context destruction suppresses requester callbacks. Remote
        // correlations are abandoned locally while CTX1 Cancel is sent
        // best-effort; handler-side work is failed by unregister_app.
        runtime_->request_network()
            .drop_requests(
                app_id_);

        runtime_->requests()
            .unregister_app(
                app_id_);

        runtime_->host()
            .unregister_app(
                app_id_);
    }

    bool advertise(cito::TypeId type_id, std::string_view resource) {
        const auto intent = key(type_id, resource);
        auto& network = request_network();
        if (published_keys_.contains(intent) ||
            !network.data_interest_enabled()) return true;
        if (published_keys_.size() >= 256) return false;
        if (!network.ensure_data_interest(intent)) return false;
        try { published_keys_.insert(intent); }
        catch (...) { network.release_data_interest(intent); throw; }
        return true;
    }

    void subscription_closed(const cito::InterestKey& key) noexcept {
        const auto it = subscribed_names_.find(key);
        if (it != subscribed_names_.end()) {
            host().unregister_resource_name(key);
            if (--it->second == 0) subscribed_names_.erase(it);
        }
        host().remove_interest(app_id_, key);
    }

    cito::detail::HostRuntimeCore&
    host() noexcept {
        return runtime_->host();
    }

    cito::detail::RequestReplyCore&
    requests() noexcept {
        return runtime_->requests();
    }

    cito::detail::RequestNetworkAdapter&
    request_network() noexcept {
        return runtime_->request_network();
    }

    cito::InterestKey key(
        cito::TypeId type_id,
        std::string_view resource)
        const noexcept {
        return {
            scope_id_,
            cito::stable_name_id(
                resource),
            type_id};
    }

    cito::detail::RequestKey
    request_key(
        cito::TypeId request_type_id,
        std::string_view resource)
        const noexcept {
        return {
            scope_id_,
            cito::stable_name_id(
                resource),
            request_type_id};
    }

    std::string scope_;
    cito::ScopeId scope_id_{};
    std::shared_ptr<
        cito::detail::EmbeddedHostRuntime>
        runtime_;
    cito::detail::AppId app_id_{};
    cito::detail::RawContext local_;
    std::unordered_map<cito::InterestKey, std::size_t, cito::InterestKeyHash>
        subscribed_names_;
    std::unordered_set<cito::InterestKey, cito::InterestKeyHash>
        published_keys_;
};

struct RequestCallbackState {
    std::weak_ptr<ContextState>
        context;
    cito_request_completion_callback_t
        callback{};
    void* user_data{};
    cito::detail::RequestId id{};
    bool remote{false};
    bool active{true};
};

} // namespace

struct cito_context {
    explicit cito_context(
        std::string scope)
        : state(
              std::make_shared<
                  ContextState>(
                  std::move(scope))) {}

    std::shared_ptr<
        ContextState> state;
};

struct cito_subscription {
    cito_subscription(
        cito::detail::RawSubscription local,
        std::weak_ptr<ContextState> state,
        cito::InterestKey key)
        : local(std::move(local)),
          state(std::move(state)),
          key(key) {}

    ~cito_subscription() {
        close();
    }

    void close() noexcept {
        if (!active) {
            return;
        }

        active = false;
        local.close();

        if (const auto locked =
                state.lock()) {
            locked->subscription_closed(key);
        }
    }

    cito::detail::RawSubscription local;
    std::weak_ptr<ContextState> state;
    cito::InterestKey key{};
    bool active{true};
};

struct cito_handler {
    cito_handler(
        std::weak_ptr<ContextState> state,
        cito::detail::HandlerToken token,
        cito::InterestKey capability)
        : state(std::move(state)),
          token(token),
          capability(capability) {}

    ~cito_handler() {
        close();
    }

    void close() noexcept {
        if (!active) {
            return;
        }

        active = false;

        if (const auto locked =
                state.lock()) {
            locked->requests()
                .unregister_handler(
                    token);
            locked->host()
                .remove_interest(
                    locked->app_id_,
                    capability);
        }
    }

    std::weak_ptr<ContextState> state;
    cito::detail::HandlerToken token{};
    cito::InterestKey capability{};
    bool active{true};
};

struct cito_request {
    explicit cito_request(
        std::shared_ptr<
            RequestCallbackState> state)
        : state(std::move(state)) {}

    ~cito_request() {
        close();
    }

    void close() noexcept {
        const auto local = state;

        if (
            !local ||
            !local->active) {
            return;
        }

        // Destruction suppresses the user callback. Explicit cancel uses the
        // public cancel function and does report Cancelled.
        local->active = false;

        if (
            local->id != 0) {
            if (const auto context =
                    local->context.lock()) {
                if (local->remote) {
                    (void)context->
                        request_network()
                        .cancel(
                            local->id);
                } else {
                    (void)context->
                        requests()
                        .cancel(
                            local->id);
                }
            }
        }

        local->id = 0;
    }

    std::shared_ptr<
        RequestCallbackState> state;
};

extern "C" {

uint32_t cito_abi_version(void) {
    return CITO_C_ABI_VERSION;
}

cito_status_t cito_context_create(
    const char* scope,
    size_t scope_size,
    cito_context_t** out) {
    if (
        !out ||
        !valid_bytes(
            scope,
            scope_size)) {
        return
            CITO_STATUS_INVALID_ARGUMENT;
    }

    *out = nullptr;

    try {
        *out = new cito_context(
            copy_text(
                scope,
                scope_size));

        return CITO_STATUS_OK;
    } catch (
        const std::bad_alloc&) {
        return
            CITO_STATUS_OUT_OF_MEMORY;
    } catch (...) {
        return
            CITO_STATUS_INTERNAL_ERROR;
    }
}

void cito_context_destroy(
    cito_context_t* context) {
    delete context;
}

cito_status_t cito_context_subscribe(
    cito_context_t* context,
    uint64_t type_id,
    const char* resource,
    size_t resource_size,
    cito_message_callback_t callback,
    void* user_data,
    cito_subscription_t** out) {
    if (
        !context ||
        !callback ||
        !out ||
        !valid_bytes(
            resource,
            resource_size)) {
        return
            CITO_STATUS_INVALID_ARGUMENT;
    }

    *out = nullptr;

    try {
        auto text =
            copy_text(
                resource,
                resource_size);

        const auto key =
            context->state->key(
                type_id,
                text);

        auto local =
            context->state->local_.on(
                type_id,
                text,
                [callback, user_data](
                    std::uint64_t id,
                    std::string_view name,
                    std::span<
                        const std::uint8_t>
                        payload) {
                    callback(
                        user_data,
                        id,
                        name.data(),
                        name.size(),
                        payload.data(),
                        payload.size());
                });

        const auto interest =
            context->state->host()
                .add_interest(
                    context->state->app_id_,
                    key);

        if (!interest.changed) {
            return
                CITO_STATUS_INTERNAL_ERROR;
        }

        bool named = false;
        try {
            if (!context->state->host().register_resource_name(key, text))
                throw std::logic_error("Cito C resource hash collision");
            named = true;
            ++context->state->subscribed_names_[key];
            *out =
                new cito_subscription(
                    std::move(local),
                    context->state,
                    key);
        } catch (...) {
            if (named) context->state->subscription_closed(key);
            else context->state->host().remove_interest(
                context->state->app_id_, key);
            throw;
        }

        return CITO_STATUS_OK;
    } catch (
        const std::bad_alloc&) {
        return
            CITO_STATUS_OUT_OF_MEMORY;
    } catch (...) {
        return
            CITO_STATUS_INTERNAL_ERROR;
    }
}

void cito_subscription_destroy(
    cito_subscription_t* subscription) {
    delete subscription;
}

cito_status_t cito_context_handle(
    cito_context_t* context,
    uint64_t request_type_id,
    const char* resource,
    size_t resource_size,
    cito_request_handler_callback_t callback,
    void* user_data,
    cito_handler_t** out) {
    if (
        !context ||
        !callback ||
        !out ||
        !valid_bytes(
            resource,
            resource_size)) {
        return
            CITO_STATUS_INVALID_ARGUMENT;
    }

    *out = nullptr;

    try {
        const auto state =
            context->state;

        auto resource_text =
            copy_text(
                resource,
                resource_size);

        const auto key =
            state->request_key(
                request_type_id,
                resource_text);

        const auto capability =
            cito::detail::
                request_handler_interest_key(
                    key);

        const auto token =
            state->requests()
                .register_handler(
                    state->app_id_,
                    key,
                    [callback,
                     user_data,
                     request_type_id,
                     resource_text =
                         std::move(
                             resource_text)](
                        cito::detail::RequestId
                            request_id,
                        cito::detail::AppId,
                        std::span<
                            const std::uint8_t>
                            payload,
                        cito::detail::
                            RequestReplyCore::Reply) {
                        callback(
                            user_data,
                            request_id,
                            request_type_id,
                            resource_text.data(),
                            resource_text.size(),
                            payload.data(),
                            payload.size());
                    });

        try {
            const auto interest =
                state->host()
                    .add_interest(
                        state->app_id_,
                        capability);

            if (!interest.changed) {
                throw std::logic_error(
                    "Cito request handler demand registration failed");
            }
        } catch (...) {
            state->requests()
                .unregister_handler(
                    token);
            throw;
        }

        try {
            *out = new cito_handler(
                state,
                token,
                capability);
        } catch (...) {
            state->host()
                .remove_interest(
                    state->app_id_,
                    capability);
            state->requests()
                .unregister_handler(
                    token);
            throw;
        }

        return CITO_STATUS_OK;
    } catch (
        const std::bad_alloc&) {
        return
            CITO_STATUS_OUT_OF_MEMORY;
    } catch (...) {
        return
            CITO_STATUS_INTERNAL_ERROR;
    }
}

void cito_handler_destroy(
    cito_handler_t* handler) {
    delete handler;
}

cito_status_t cito_context_discover_handler(
    cito_context_t* context, uint64_t request_type_id,
    const char* resource, size_t resource_size) {
    if (!context || !valid_bytes(resource, resource_size))
        return CITO_STATUS_INVALID_ARGUMENT;
    try {
        const auto view = resource_size == 0 ? std::string_view{} :
            std::string_view(resource, resource_size);
        context->state->request_network().discover_request_handler(
            context->state->request_key(request_type_id, view));
        return CITO_STATUS_OK;
    } catch (...) { return CITO_STATUS_INTERNAL_ERROR; }
}

cito_status_t cito_context_has_handler(
    cito_context_t* context,
    uint64_t request_type_id,
    const char* resource,
    size_t resource_size,
    int* out_has_handler) {
    if (
        !context ||
        !out_has_handler ||
        !valid_bytes(
            resource,
            resource_size)) {
        return
            CITO_STATUS_INVALID_ARGUMENT;
    }

    try {
        const auto resource_view =
            resource_size == 0
                ? std::string_view{}
                : std::string_view(
                    resource,
                    resource_size);

        const auto key =
            context->state->
                request_key(
                    request_type_id,
                    resource_view);

        *out_has_handler =
            (
                context->state->
                    requests()
                    .has_handler(key) ||
                context->state->
                    request_network()
                    .can_send(key)
            )
                ? 1
                : 0;

        return CITO_STATUS_OK;
    } catch (...) {
        return
            CITO_STATUS_INTERNAL_ERROR;
    }
}

cito_status_t cito_context_request(
    cito_context_t* context,
    uint64_t request_type_id,
    const char* resource,
    size_t resource_size,
    const uint8_t* payload,
    size_t payload_size,
    cito_request_completion_callback_t callback,
    void* user_data,
    cito_request_t** out) {
    if (
        !context ||
        !callback ||
        !out ||
        !valid_bytes(
            resource,
            resource_size) ||
        !valid_bytes(
            payload,
            payload_size)) {
        return
            CITO_STATUS_INVALID_ARGUMENT;
    }

    *out = nullptr;

    try {
        const auto context_state =
            context->state;

        auto callback_state =
            std::make_shared<
                RequestCallbackState>();

        callback_state->context =
            context_state;
        callback_state->callback =
            callback;
        callback_state->user_data =
            user_data;

        auto request =
            std::make_unique<
                cito_request>(
                callback_state);

        const auto resource_view =
            resource_size == 0
                ? std::string_view{}
                : std::string_view(
                    resource,
                    resource_size);

        const auto key =
            context_state->
                request_key(
                    request_type_id,
                    resource_view);

        auto completion =
            [callback_state](
                cito::detail::
                    RequestStatus status,
                std::span<
                    const std::uint8_t>
                    response) {
                if (
                    !callback_state->
                        active) {
                    return;
                }

                callback_state->active =
                    false;
                callback_state->id = 0;

                callback_state->
                    callback(
                        callback_state->
                            user_data,
                        from_request_status(
                            status),
                        response.data(),
                        response.size());
            };

        if (
            context_state->requests()
                .has_handler(key)) {
            const auto started =
                context_state->requests()
                    .start(
                        context_state->
                            app_id_,
                        key,
                        payload_view(
                            payload,
                            payload_size),
                        completion);

            if (
                callback_state->active &&
                started.dispatched) {
                callback_state->id =
                    started.id;
                callback_state->remote =
                    false;
            }
        } else if (
            context_state->
                request_network()
                .can_send(key)) {
            const auto started =
                context_state->
                    request_network()
                    .start(
                        context_state->
                            app_id_,
                        key,
                        payload_view(
                            payload,
                            payload_size),
                        completion);

            if (
                callback_state->active &&
                started.request_id != 0) {
                callback_state->id =
                    started.request_id;
                callback_state->remote =
                    true;
            }
        } else {
            completion(
                cito::detail::
                    RequestStatus::
                        Unavailable,
                {});
        }

        *out = request.release();

        return CITO_STATUS_OK;
    } catch (
        const std::bad_alloc&) {
        return
            CITO_STATUS_OUT_OF_MEMORY;
    } catch (...) {
        return
            CITO_STATUS_INTERNAL_ERROR;
    }
}

cito_status_t cito_request_cancel(
    cito_request_t* request,
    int* out_cancelled) {
    if (!request) {
        return
            CITO_STATUS_INVALID_ARGUMENT;
    }

    const auto state =
        request->state;

    bool cancelled = false;

    if (
        state &&
        state->active &&
        state->id != 0) {
        if (const auto context =
                state->context.lock()) {
            cancelled =
                state->remote
                    ? context->
                        request_network()
                        .cancel(
                            state->id)
                    : context->
                        requests()
                        .cancel(
                            state->id);
        }
    }

    if (out_cancelled) {
        *out_cancelled =
            cancelled ? 1 : 0;
    }

    return CITO_STATUS_OK;
}

void cito_request_destroy(
    cito_request_t* request) {
    delete request;
}

cito_status_t cito_context_reply(
    cito_context_t* context,
    uint64_t request_id,
    cito_request_status_t status,
    const uint8_t* payload,
    size_t payload_size,
    int* out_accepted) {
    if (
        !context ||
        request_id == 0 ||
        !valid_request_status(
            status) ||
        !valid_bytes(
            payload,
            payload_size)) {
        return
            CITO_STATUS_INVALID_ARGUMENT;
    }

    try {
        const auto accepted =
            context->state->requests()
                .complete_handler_reply(
                    context->state->app_id_,
                    request_id,
                    to_request_status(
                        status),
                    payload_view(
                        payload,
                        payload_size));

        if (out_accepted) {
            *out_accepted =
                accepted ? 1 : 0;
        }

        return CITO_STATUS_OK;
    } catch (...) {
        return
            CITO_STATUS_INTERNAL_ERROR;
    }
}

cito_status_t cito_context_has_demand(
    cito_context_t* context,
    uint64_t type_id,
    const char* resource,
    size_t resource_size,
    int* out_has_demand) {
    if (
        !context ||
        !out_has_demand ||
        !valid_bytes(
            resource,
            resource_size)) {
        return
            CITO_STATUS_INVALID_ARGUMENT;
    }

    try {
        const auto resource_view =
            resource_size == 0
                ? std::string_view{}
                : std::string_view(
                    resource,
                    resource_size);

        *out_has_demand =
            context->state->host()
                .has_demand(
                    context->state->key(
                        type_id,
                        resource_view))
                ? 1
                : 0;

        return CITO_STATUS_OK;
    } catch (...) {
        return
            CITO_STATUS_INTERNAL_ERROR;
    }
}

cito_status_t cito_context_advertise(
    cito_context_t* context, uint64_t type_id,
    const char* resource, size_t resource_size) {
    if (!context || !valid_bytes(resource, resource_size))
        return CITO_STATUS_INVALID_ARGUMENT;
    try {
        const auto name = resource_size == 0 ? std::string_view{} :
            std::string_view(resource, resource_size);
        return context->state->advertise(type_id, name)
            ? CITO_STATUS_OK : CITO_STATUS_INTERNAL_ERROR;
    } catch (const std::bad_alloc&) { return CITO_STATUS_OUT_OF_MEMORY; }
      catch (...) { return CITO_STATUS_INTERNAL_ERROR; }
}

cito_status_t cito_context_publish(
    cito_context_t* context,
    uint64_t type_id,
    const char* resource,
    size_t resource_size,
    const uint8_t* payload,
    size_t payload_size,
    size_t* out_delivered) {
    if (
        !context ||
        !valid_bytes(
            resource,
            resource_size) ||
        !valid_bytes(
            payload,
            payload_size)) {
        return
            CITO_STATUS_INVALID_ARGUMENT;
    }

    try {
        const auto resource_view =
            resource_size == 0
                ? std::string_view{}
                : std::string_view(
                    resource,
                    resource_size);

        if (!context->state->advertise(type_id, resource_view))
            return CITO_STATUS_INTERNAL_ERROR;

        const auto result =
            context->state->host().publish(
                context->state->key(type_id, resource_view),
                resource_view,
                payload_view(payload, payload_size),
                [state = context->state](
                    cito::DestinationId destination,
                    std::span<const std::uint8_t> packet) {
                    return state->request_network().send_data(destination, packet);
                });

        if (out_delivered) {
            *out_delivered =
                result.delivered();
        }

        return CITO_STATUS_OK;
    } catch (...) {
        return
            CITO_STATUS_INTERNAL_ERROR;
    }
}

cito_status_t cito_context_run(
    cito_context_t* context) {
    if (!context) {
        return
            CITO_STATUS_INVALID_ARGUMENT;
    }

    try {
        context->state->runtime_->run();
        return CITO_STATUS_OK;
    } catch (...) { return CITO_STATUS_INTERNAL_ERROR; }
}

void cito_context_stop(cito_context_t* context) {
    if (context) context->state->runtime_->stop();
}

const char* cito_status_string(
    cito_status_t status) {
    switch (status) {
        case CITO_STATUS_OK:
            return "ok";
        case CITO_STATUS_INVALID_ARGUMENT:
            return "invalid argument";
        case CITO_STATUS_OUT_OF_MEMORY:
            return "out of memory";
        case CITO_STATUS_INTERNAL_ERROR:
            return "internal error";
    }

    return "unknown";
}

} // extern "C"
