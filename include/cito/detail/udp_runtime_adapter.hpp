#pragma once

#include <cito/detail/discovery_control_core.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cito::detail {

struct UdpEndpoint {
    std::uint32_t ipv4{};
    std::uint16_t port{};

    friend bool operator==(
        const UdpEndpoint&,
        const UdpEndpoint&) = default;
};

struct UdpSend {
    UdpEndpoint endpoint{};
    std::vector<std::uint8_t> bytes;
};

class UdpRuntimeAdapter {
private:
    using RouteMap =
        std::unordered_map<
            InterestKey,
            std::vector<DestinationId>,
            InterestKeyHash>;

public:
    UdpRuntimeAdapter(
        HostRuntimeCore& host,
        CoordinatorId self,
        std::uint64_t incarnation)
        : host_(host),
          control_(
              host,
              self,
              incarnation) {}

    std::vector<std::uint8_t>
    announcement(
        std::uint16_t control_port,
        std::uint32_t lease_ms) const {
        return encode_summary_announcement(
            control_.announcement(
                control_port,
                lease_ms));
    }

    void set_local_route_endpoints(
        std::span<const RouteEndpoint>
            endpoints) {
        local_route_endpoints_.assign(
            endpoints.begin(),
            endpoints.end());
    }

    std::vector<UdpSend>
    process_control(
        std::span<const std::uint8_t> bytes,
        UdpEndpoint source,
        std::uint64_t now_ms) {
        std::vector<UdpSend> sends;

        if (bytes.size() < 4 ||
            bytes[0] != 'C' ||
            bytes[1] != 'T') {
            return sends;
        }

        const auto family =
            static_cast<char>(bytes[2]);
        const auto version =
            static_cast<char>(bytes[3]);

        if (
            family == 'S' &&
            version == '2') {
            const auto message =
                decode_summary_announcement(
                    bytes);

            const auto before =
                host_.coordinator_incarnation(
                    message.coordinator);

            const auto actions =
                control_.on_summary(
                    message,
                    now_ms);

            const auto after =
                host_.coordinator_incarnation(
                    message.coordinator);

            if (
                after &&
                *after ==
                    message.stamp.incarnation) {
                if (
                    before &&
                    *before != *after) {
                    clear_coordinator(
                        message.coordinator);
                }

                control_endpoints_[
                    message.coordinator] =
                    UdpEndpoint{
                        source.ipv4,
                        message.control_port};
            }

            append_actions(
                actions,
                source,
                sends);
            return sends;
        }

        if (
            family == 'Q' &&
            version == '1') {
            const auto action =
                control_.on_snapshot_request(
                    decode_snapshot_request(
                        bytes));
            append_action(
                action,
                source,
                sends);
            return sends;
        }

        if (
            family == 'B' &&
            version == '1') {
            const auto batch =
                decode_snapshot_batch(
                    bytes);

            const auto actions =
                control_.on_snapshot_batch(
                    batch);

            prune_coordinator_routes(
                batch.coordinator);

            append_actions(
                actions,
                source,
                sends);
            return sends;
        }

        if (
            family == 'R' &&
            version == '3') {
            const auto action =
                control_.on_route_request(
                    decode_route_request(
                        bytes),
                    local_route_endpoints_);
            append_action(
                action,
                source,
                sends);
            return sends;
        }

        if (
            family == 'R' &&
            (
                version == '4' ||
                version == '5'
            )) {
            const auto batch =
                decode_route_batch(
                    bytes);

            if (!control_.on_route_batch(
                    batch)) {
                return sends;
            }

            replace_endpoint_route(
                batch.coordinator,
                batch.key,
                batch.endpoints,
                source.ipv4);
            return sends;
        }

        return sends;
    }

    std::vector<UdpSend>
    ensure_routes(
        const InterestKey& key) {
        std::vector<UdpSend> sends;
        append_actions(
            control_.ensure_routes(
                key),
            {},
            sends);
        return sends;
    }

    bool release_routes(
        const InterestKey& key) {
        const auto changed =
            control_.release_routes(
                key);

        if (!changed) {
            return false;
        }

        for (auto coordinator_it =
                 endpoint_routes_.begin();
             coordinator_it !=
                 endpoint_routes_.end();) {
            auto& routes =
                coordinator_it->second;
            const auto it =
                routes.find(key);

            if (it !=
                routes.end()) {
                release_destinations(
                    it->second);
                routes.erase(it);
            }

            if (routes.empty()) {
                coordinator_it =
                    endpoint_routes_.erase(
                        coordinator_it);
            } else {
                ++coordinator_it;
            }
        }

        return true;
    }

    std::size_t expire(
        std::uint64_t now_ms) {
        const auto expired =
            control_.expire_coordinators(
                now_ms);

        for (const auto coordinator :
             expired) {
            clear_coordinator(
                coordinator);
        }

        return expired.size();
    }

    template <class Send>
    HostPublishResult publish(
        const InterestKey& key,
        std::string_view resource,
        std::span<const std::uint8_t> payload,
        Send&& send) {
        return host_.publish(
            key,
            resource,
            payload,
            [&](DestinationId destination,
                std::span<const std::uint8_t>
                    packet) {
                const auto endpoint =
                    data_endpoints_.find(
                        destination);

                if (endpoint ==
                    data_endpoints_.end()) {
                    return false;
                }

                return std::invoke(
                    send,
                    endpoint->second,
                    packet);
            });
    }

    std::size_t receive_data(
        std::span<const std::uint8_t> bytes,
        std::string_view resource) {
        const auto packet =
            decode_data_packet_view(
                bytes);

        return host_.deliver_local(
            packet.key,
            resource,
            packet.payload);
    }

    const UdpEndpoint* control_endpoint(
        CoordinatorId coordinator) const noexcept {
        const auto it =
            control_endpoints_.find(
                coordinator);

        return
            it ==
                control_endpoints_.end()
                ? nullptr
                : &it->second;
    }

    const UdpEndpoint* data_endpoint(
        DestinationId destination) const noexcept {
        const auto it =
            data_endpoints_.find(
                destination);

        return
            it ==
                data_endpoints_.end()
                ? nullptr
                : &it->second;
    }

    const UdpEndpoint* quic_endpoint(
        DestinationId destination) const noexcept {
        const auto it =
            quic_endpoints_.find(
                destination);

        return
            it ==
                quic_endpoints_.end()
                ? nullptr
                : &it->second;
    }

    DiscoveryControlCore& control()
        noexcept {
        return control_;
    }

private:
    void append_actions(
        const std::vector<ControlAction>& actions,
        UdpEndpoint source,
        std::vector<UdpSend>& sends) {
        for (const auto& action :
             actions) {
            append_action(
                action,
                source,
                sends);
        }
    }

    void append_action(
        const ControlAction& action,
        UdpEndpoint source,
        std::vector<UdpSend>& sends) {
        if (action.bytes.empty()) {
            return;
        }

        if (
            action.target ==
            ControlTargetKind::Source) {
            sends.push_back(
                UdpSend{
                    source,
                    action.bytes});
            return;
        }

        const auto endpoint =
            control_endpoints_.find(
                action.coordinator);

        if (endpoint ==
            control_endpoints_.end()) {
            return;
        }

        sends.push_back(
            UdpSend{
                endpoint->second,
                action.bytes});
    }

    void replace_endpoint_route(
        CoordinatorId coordinator,
        const InterestKey& key,
        std::span<const RouteEndpoint>
            endpoints,
        std::uint32_t ipv4) {
        auto& routes =
            endpoint_routes_[
                coordinator];

        const auto old =
            routes.find(key);

        if (old != routes.end()) {
            release_destinations(
                old->second);
        }

        std::vector<DestinationId>
            next;
        next.reserve(
            endpoints.size());

        for (const auto& endpoint :
             endpoints) {
            auto& refs =
                destination_refs_[
                    endpoint.destination];
            ++refs;

            if (
                endpoint.data_port != 0) {
                data_endpoints_[
                    endpoint.destination] =
                    UdpEndpoint{
                        ipv4,
                        endpoint.data_port};
            }

            if (
                endpoint.quic_port != 0) {
                quic_endpoints_[
                    endpoint.destination] =
                    UdpEndpoint{
                        ipv4,
                        endpoint.quic_port};
            }

            next.push_back(
                endpoint.destination);
        }

        if (next.empty()) {
            routes.erase(key);
        } else {
            routes.insert_or_assign(
                key,
                std::move(next));
        }

        if (routes.empty()) {
            endpoint_routes_.erase(
                coordinator);
        }
    }

    void release_destinations(
        std::span<const DestinationId>
            destinations) {
        for (const auto destination :
             destinations) {
            const auto ref =
                destination_refs_.find(
                    destination);

            if (ref ==
                destination_refs_.end()) {
                continue;
            }

            if (--ref->second == 0) {
                destination_refs_.erase(
                    ref);
                data_endpoints_.erase(
                    destination);
                quic_endpoints_.erase(
                    destination);
            }
        }
    }

    void prune_coordinator_routes(
        CoordinatorId coordinator) {
        auto coordinator_it =
            endpoint_routes_.find(
                coordinator);

        if (coordinator_it ==
            endpoint_routes_.end()) {
            return;
        }

        auto& routes =
            coordinator_it->second;

        for (auto it =
                 routes.begin();
             it != routes.end();) {
            const auto& current =
                host_.remote_destinations(
                    coordinator,
                    it->first);

            if (!current.empty()) {
                ++it;
                continue;
            }

            release_destinations(
                it->second);
            it = routes.erase(it);
        }

        if (routes.empty()) {
            endpoint_routes_.erase(
                coordinator_it);
        }
    }

    void clear_coordinator(
        CoordinatorId coordinator) {
        control_endpoints_.erase(
            coordinator);

        auto routes =
            endpoint_routes_.find(
                coordinator);

        if (routes !=
            endpoint_routes_.end()) {
            for (const auto& [key, destinations] :
                 routes->second) {
                (void)key;
                release_destinations(
                    destinations);
            }

            endpoint_routes_.erase(
                routes);
        }
    }

    HostRuntimeCore& host_;
    DiscoveryControlCore control_;

    std::vector<RouteEndpoint>
        local_route_endpoints_;

    std::unordered_map<
        CoordinatorId,
        UdpEndpoint> control_endpoints_;

    std::unordered_map<
        CoordinatorId,
        RouteMap> endpoint_routes_;

    std::unordered_map<
        DestinationId,
        UdpEndpoint> data_endpoints_;

    std::unordered_map<
        DestinationId,
        UdpEndpoint> quic_endpoints_;

    std::unordered_map<
        DestinationId,
        std::size_t> destination_refs_;
};

} // namespace cito::detail
