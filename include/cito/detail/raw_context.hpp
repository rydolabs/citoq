#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <deque>
#include <vector>

namespace cito::detail {

class RawSubscription {
public:
    RawSubscription() = default;
    explicit RawSubscription(std::function<void()> close)
        : close_(std::move(close)) {}
    RawSubscription(const RawSubscription&) = delete;
    RawSubscription& operator=(const RawSubscription&) = delete;
    RawSubscription(RawSubscription&& other) noexcept
        : close_(std::move(other.close_)) {}
    RawSubscription& operator=(RawSubscription&& other) noexcept {
        if (this != &other) { close(); close_ = std::move(other.close_); }
        return *this;
    }
    ~RawSubscription() { close(); }
    void close() noexcept {
        if (!close_) return;
        auto fn = std::move(close_);
        fn();
    }
private:
    std::function<void()> close_;
};

class RawContext {
public:
    using Callback = std::function<void(
        std::uint64_t, std::string_view, std::span<const std::uint8_t>)>;
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
        Callback callback;
    };
    // Keep executing callbacks stable when subscriptions are added reentrantly.
    using Bucket = std::deque<Handler>;
    using ResourceMap = std::unordered_map<
        std::string, Bucket, TransparentStringHash, std::equal_to<>>;
    struct Locator {
        std::uint64_t type_id{};
        std::string resource;
    };
    struct State {
        std::size_t add(std::uint64_t type_id, std::string resource, Callback callback) {
            const auto id = next_id++;
            buckets[type_id][resource].push_back(
                Handler{id, true, std::move(callback)});
            locators.emplace(id, Locator{type_id, std::move(resource)});
            return id;
        }
        void remove(std::size_t id) noexcept {
            const auto loc = locators.find(id);
            if (loc == locators.end()) return;
            if (dispatch_depth == 0) { remove_now(id); return; }
            auto* handler = find_handler(loc->second, id);
            if (handler && handler->active) {
                handler->active = false;
                pending_removals.push_back(id);
            }
        }
        bool has_demand(
            std::uint64_t type_id,
            std::string_view resource) const noexcept {
            const auto type_it =
                buckets.find(type_id);

            if (type_it ==
                buckets.end()) {
                return false;
            }

            const auto resource_it =
                type_it->second.find(
                    resource);

            return
                resource_it !=
                    type_it->second.end() &&
                !resource_it->second.empty();
        }

        std::size_t publish(
            std::uint64_t type_id, std::string_view resource,
            std::span<const std::uint8_t> payload) {
            const auto type_it = buckets.find(type_id);
            if (type_it == buckets.end()) return 0;
            const auto resource_it = type_it->second.find(resource);
            if (resource_it == type_it->second.end()) return 0;
            auto& bucket = resource_it->second;
            const auto initial_size = bucket.size();
            std::size_t delivered = 0;
            ++dispatch_depth;
            try {
                for (std::size_t i = 0; i < initial_size; ++i) {
                    if (!bucket[i].active) continue;
                    bucket[i].callback(type_id, resource, payload);
                    ++delivered;
                }
            } catch (...) { finish_dispatch(); throw; }
            finish_dispatch();
            return delivered;
        }
        Handler* find_handler(const Locator& loc, std::size_t id) noexcept {
            const auto type_it = buckets.find(loc.type_id);
            if (type_it == buckets.end()) return nullptr;
            const auto resource_it = type_it->second.find(loc.resource);
            if (resource_it == type_it->second.end()) return nullptr;
            for (auto& handler : resource_it->second) {
                if (handler.id == id) return &handler;
            }
            return nullptr;
        }
        void remove_now(std::size_t id) noexcept {
            const auto loc_it = locators.find(id);
            if (loc_it == locators.end()) return;
            const auto loc = loc_it->second;
            const auto type_it = buckets.find(loc.type_id);
            if (type_it != buckets.end()) {
                const auto resource_it = type_it->second.find(loc.resource);
                if (resource_it != type_it->second.end()) {
                    auto& bucket = resource_it->second;
                    bucket.erase(std::remove_if(bucket.begin(), bucket.end(),
                        [id](const Handler& h) { return h.id == id; }), bucket.end());
                    if (bucket.empty()) type_it->second.erase(resource_it);
                }
                if (type_it->second.empty()) buckets.erase(type_it);
            }
            locators.erase(loc_it);
        }
        void finish_dispatch() noexcept {
            --dispatch_depth;
            if (dispatch_depth != 0) return;
            auto removals = std::move(pending_removals);
            pending_removals.clear();
            for (const auto id : removals) remove_now(id);
        }
        std::size_t next_id{1};
        std::size_t dispatch_depth{0};
        std::unordered_map<std::uint64_t, ResourceMap> buckets;
        std::unordered_map<std::size_t, Locator> locators;
        std::vector<std::size_t> pending_removals;
    };
public:
    explicit RawContext(std::string scope = {})
        : scope_(std::move(scope)), state_(std::make_shared<State>()) {}
    const std::string& scope() const noexcept { return scope_; }
    RawSubscription on(std::uint64_t type_id, std::string resource, Callback callback) {
        const auto id = state_->add(type_id, std::move(resource), std::move(callback));
        std::weak_ptr<State> weak = state_;
        return RawSubscription([weak = std::move(weak), id] {
            if (const auto state = weak.lock()) state->remove(id);
        });
    }
    bool has_demand(
        std::uint64_t type_id,
        std::string_view resource) const noexcept {
        return state_->has_demand(
            type_id,
            resource);
    }
    std::size_t publish(std::uint64_t type_id, std::string_view resource,
                        std::span<const std::uint8_t> payload) {
        return state_->publish(type_id, resource, payload);
    }
    void run() noexcept {}
private:
    std::string scope_;
    std::shared_ptr<State> state_;
};

} // namespace cito::detail
