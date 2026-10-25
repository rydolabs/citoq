#pragma once

#include <cito/detail/request_reply_core.hpp>
#include <cito/detail/udp_runtime_adapter.hpp>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cito::detail {

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
        State state{
            State::Connecting};
    };

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

        std::vector<RequestSessionAction>
            actions;

        for (auto it =
                 tracked_.begin();
             it != tracked_.end();) {
            const auto desired =
                desired_endpoints.find(
                    it->first);

            if (
                desired !=
                    desired_endpoints.end() &&
                desired->second ==
                    it->second.endpoint) {
                ++it;
                continue;
            }

            actions.push_back(
                RequestSessionAction{
                    RequestSessionActionKind::
                        Disconnect,
                    it->first,
                    it->second.endpoint});

            it = tracked_.erase(it);
        }

        for (const auto& [destination,
                           endpoint] :
             desired_endpoints) {
            if (tracked_.contains(
                    destination)) {
                continue;
            }

            tracked_.emplace(
                destination,
                Tracked{
                    endpoint,
                    State::Connecting});

            actions.push_back(
                RequestSessionAction{
                    RequestSessionActionKind::
                        Connect,
                    destination,
                    endpoint});
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

        tracked_.erase(it);
        return true;
    }

    bool mark_disconnected(
        DestinationId destination) {
        return
            tracked_.erase(
                destination) != 0;
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
};

} // namespace cito::detail
