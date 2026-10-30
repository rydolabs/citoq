#pragma once

#include <citoq/detail/request_reply_core.hpp>
#include <citoq/detail/udp_runtime_adapter.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace citoq::detail {

enum class RequestSessionActionKind :
    std::uint8_t {
    Connect,
    Disconnect,
};

struct RequestSessionAction {
    RequestSessionActionKind kind{
        RequestSessionActionKind::Connect};
    DestinationId destination{};
    UdpEndpoint endpoint{};
};

class RequestSessionLifecycleCore {
private:
    enum class State :
        std::uint8_t {
        Connecting,
        Connected,
    };

    struct Tracked {
        UdpEndpoint endpoint{};
        State state{State::Connecting};
#if defined(CITOQ_STABILIZE_RECONNECT) && CITOQ_STABILIZE_RECONNECT
        std::chrono::steady_clock::time_point started{};
#endif
    };
#if defined(CITOQ_STABILIZE_RECONNECT) && CITOQ_STABILIZE_RECONNECT
    struct Retry {
        UdpEndpoint endpoint{};
        std::uint32_t failures{0};
        std::chrono::steady_clock::time_point not_before{};
    };
    void note_failure(DestinationId id, UdpEndpoint endpoint) {
        auto& retry = retries_[id];
        if (retry.endpoint != endpoint) retry.failures = 0;
        retry.endpoint = endpoint;
        if (retry.failures < 6) ++retry.failures;
        const auto delay_ms = std::min<std::uint32_t>(
            4000, (125u << std::min<std::uint32_t>(retry.failures - 1, 5))
            + static_cast<std::uint32_t>(id % 101));
        retry.not_before = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(delay_ms);
    }
#endif

public:
    RequestSessionLifecycleCore(
        HostRuntimeCore& host,
        UdpRuntimeAdapter& routes)
        : host_(host),
          routes_(routes) {}

    bool ensure(
        const RequestKey& key) {
        if (!desired_.insert(
                key).second) {
            return false;
        }

        auto sends =
            routes_.ensure_routes(
                request_handler_interest_key(
                    key));

        pending_control_.insert(
            pending_control_.end(),
            std::make_move_iterator(
                sends.begin()),
            std::make_move_iterator(
                sends.end()));

        return true;
    }

    bool release(
        const RequestKey& key) {
        if (desired_.erase(
                key) == 0) {
            return false;
        }

        routes_.release_routes(
            request_handler_interest_key(
                key));

        return true;
    }

    // Data producers need the same Discovery and connection lifecycle as RPC.
    bool ensure_data(const InterestKey& key) {
        if (!desired_data_.insert(key).second) return false;
        auto sends = routes_.ensure_routes(key);
        pending_control_.insert(pending_control_.end(),
            std::make_move_iterator(sends.begin()),
            std::make_move_iterator(sends.end()));
        return true;
    }

    bool release_data(const InterestKey& key) {
        if (desired_data_.erase(key) == 0) return false;
        routes_.release_routes(key);
        return true;
    }

    std::vector<UdpSend>
    take_control_sends() {
        auto sends =
            std::move(
                pending_control_);

        pending_control_.clear();
        return sends;
    }

    std::vector<RequestSessionAction>
    reconcile() {
        std::unordered_map<
            DestinationId,
            UdpEndpoint> desired_endpoints;

        for (const auto& key :
             desired_) {
            const auto capability =
                request_handler_interest_key(
                    key);

            const auto& destinations =
                host_.remote_destinations(
                    capability);

            for (const auto destination :
                 destinations) {
                const auto* endpoint =
                    routes_.quic_endpoint(
                        destination);

                if (
                    !endpoint ||
                    endpoint->port == 0) {
                    continue;
                }

                desired_endpoints
                    .insert_or_assign(
                        destination,
                        *endpoint);
            }
        }

        for (const auto& key : desired_data_) {
            for (const auto destination : host_.remote_destinations(key)) {
                const auto* endpoint = routes_.quic_endpoint(destination);
                if (endpoint && endpoint->port != 0)
                    desired_endpoints.insert_or_assign(destination, *endpoint);
            }
        }

        std::vector<RequestSessionAction> actions;
#if defined(CITOQ_STABILIZE_RECONNECT) && CITOQ_STABILIZE_RECONNECT
        const auto now = std::chrono::steady_clock::now();
        for (auto it = retries_.begin(); it != retries_.end();) {
            const auto found = desired_endpoints.find(it->first);
            if (found == desired_endpoints.end() ||
                found->second != it->second.endpoint)
                it = retries_.erase(it);
            else ++it;
        }
#endif
        for (auto it = tracked_.begin(); it != tracked_.end();) {
            const auto desired = desired_endpoints.find(it->first);
            if (desired != desired_endpoints.end() &&
                desired->second == it->second.endpoint) {
#if defined(CITOQ_STABILIZE_RECONNECT) && CITOQ_STABILIZE_RECONNECT
                if (it->second.state == State::Connecting &&
                    now - it->second.started > std::chrono::seconds(10)) {
                    note_failure(it->first, it->second.endpoint);
                } else {
                    ++it;
                    continue;
                }
#else
                ++it;
                continue;
#endif
            }
            actions.push_back(RequestSessionAction{
                RequestSessionActionKind::Disconnect,
                it->first, it->second.endpoint});
            it = tracked_.erase(it);
        }

        for (const auto& [destination, endpoint] : desired_endpoints) {
            if (tracked_.contains(destination)) continue;
#if defined(CITOQ_STABILIZE_RECONNECT) && CITOQ_STABILIZE_RECONNECT
            const auto retry = retries_.find(destination);
            if (retry != retries_.end() &&
                retry->second.endpoint == endpoint &&
                now < retry->second.not_before) continue;
            std::size_t connecting = 0;
            for (const auto& [id, tracked] : tracked_)
                if (tracked.state == State::Connecting) ++connecting;
            if (connecting >= 64) continue;
            tracked_.emplace(destination, Tracked{
                endpoint, State::Connecting, now});
#else
            tracked_.emplace(destination, Tracked{
                endpoint, State::Connecting});
#endif
            actions.push_back(RequestSessionAction{
                RequestSessionActionKind::Connect,
                destination, endpoint});
        }
        return actions;
    }

    bool mark_connected(
        DestinationId destination,
        UdpEndpoint endpoint) {
        const auto it =
            tracked_.find(
                destination);

        if (
            it == tracked_.end() ||
            it->second.endpoint !=
                endpoint ||
            it->second.state !=
                State::Connecting) {
            return false;
        }

        it->second.state =
            State::Connected;
#if defined(CITOQ_STABILIZE_RECONNECT) && CITOQ_STABILIZE_RECONNECT
        retries_.erase(destination);
#endif
        return true;
    }

    bool mark_connect_failed(
        DestinationId destination,
        UdpEndpoint endpoint) {
        const auto it =
            tracked_.find(
                destination);

        if (
            it == tracked_.end() ||
            it->second.endpoint !=
                endpoint ||
            it->second.state !=
                State::Connecting) {
            return false;
        }

#if defined(CITOQ_STABILIZE_RECONNECT) && CITOQ_STABILIZE_RECONNECT
        note_failure(destination, endpoint);
#endif
        tracked_.erase(it);
        return true;
    }

    bool mark_disconnected(
        DestinationId destination) {
        const auto it = tracked_.find(destination);
        if (it == tracked_.end()) return false;
#if defined(CITOQ_STABILIZE_RECONNECT) && CITOQ_STABILIZE_RECONNECT
        note_failure(destination, it->second.endpoint);
#endif
        tracked_.erase(it);
        return true;
    }

    bool is_connecting(
        DestinationId destination)
        const noexcept {
        const auto it =
            tracked_.find(
                destination);

        return
            it != tracked_.end() &&
            it->second.state ==
                State::Connecting;
    }

    bool is_connected(
        DestinationId destination)
        const noexcept {
        const auto it =
            tracked_.find(
                destination);

        return
            it != tracked_.end() &&
            it->second.state ==
                State::Connected;
    }

    std::size_t desired_count()
        const noexcept {
        return desired_.size();
    }

    std::size_t tracked_count()
        const noexcept {
        return tracked_.size();
    }

private:
    HostRuntimeCore& host_;
    UdpRuntimeAdapter& routes_;

    std::unordered_set<
        RequestKey,
        RequestKeyHash> desired_;

    std::unordered_set<InterestKey, InterestKeyHash> desired_data_;

    std::vector<UdpSend>
        pending_control_;

    std::unordered_map<
        DestinationId,
        Tracked> tracked_;
#if defined(CITOQ_STABILIZE_RECONNECT) && CITOQ_STABILIZE_RECONNECT
    std::unordered_map<DestinationId, Retry> retries_;
#endif
};

} // namespace citoq::detail
