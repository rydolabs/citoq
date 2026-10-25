#pragma once

#include <cito/demand_control.hpp>
#include <cito/detail/host_runtime_core.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cito::detail {

enum class ControlTargetKind : std::uint8_t {
    Source,
    Coordinator,
};

struct ControlAction {
    ControlTargetKind target{
        ControlTargetKind::Coordinator};
    CoordinatorId coordinator{};
    std::vector<std::uint8_t> bytes;
};

struct DiscoveryControlStats {
    std::size_t snapshot_requests{0};
    std::size_t route_requests{0};
    std::size_t stale_packets{0};
    std::size_t restarts{0};
    std::size_t expired_coordinators{0};
};

class DiscoveryControlCore {
private:
    struct PendingSnapshot {
        DemandSummaryStamp stamp{};
        std::vector<InterestKey> keys;
        std::uint32_t next_offset{};
    };

    using RequestedRoutes =
        std::unordered_map<
            InterestKey,
            std::uint64_t,
            InterestKeyHash>;

public:
    DiscoveryControlCore(
        HostRuntimeCore& host,
        CoordinatorId self,
        std::uint64_t incarnation)
        : host_(host),
          self_(self),
          incarnation_(incarnation) {}

    DemandSummaryAnnouncement announcement(
        std::uint16_t control_port,
        std::uint32_t lease_ms) const {
        return {
            self_,
            control_port,
            lease_ms,
            host_.local_stamp(
                incarnation_)};
    }

    std::vector<ControlAction>
    on_summary(
        const DemandSummaryAnnouncement& message,
        std::uint64_t now_ms) {
        std::vector<ControlAction> actions;

        if (message.coordinator == self_) {
            return actions;
        }

        const auto observation =
            host_.observe_coordinator(
                message.coordinator,
                message.stamp.incarnation,
                message.lease_ms,
                now_ms);

        if (
            observation ==
            CoordinatorLeaseObservation::Stale) {
            ++stats_.stale_packets;
            return actions;
        }

        if (
            observation ==
            CoordinatorLeaseObservation::Restarted) {
            ++stats_.restarts;
            pending_.erase(
                message.coordinator);
            requested_routes_.erase(
                message.coordinator);
        }

        if (!host_.needs_remote_snapshot(
                message.coordinator,
                message.stamp)) {
            return actions;
        }

        const auto pending_it =
            pending_.find(
                message.coordinator);

        if (
            pending_it != pending_.end() &&
            pending_it->second.stamp ==
                message.stamp) {
            return actions;
        }

        pending_.insert_or_assign(
            message.coordinator,
            PendingSnapshot{
                message.stamp,
                {},
                0});

        actions.push_back(
            snapshot_request_action(
                message.coordinator,
                message.stamp,
                0));

        return actions;
    }

    std::vector<ControlAction>
    on_snapshot_batch(
        const DemandSnapshotBatch& batch) {
        std::vector<ControlAction> actions;

        if (!host_.coordinator_is_current(
                batch.coordinator,
                batch.stamp.incarnation)) {
            ++stats_.stale_packets;
            return actions;
        }

        auto pending_it =
            pending_.find(
                batch.coordinator);

        if (pending_it ==
            pending_.end()) {
            return actions;
        }

        auto& pending =
            pending_it->second;

        if (
            pending.stamp !=
                batch.stamp ||
            pending.next_offset !=
                batch.offset) {
            return actions;
        }

        pending.keys.insert(
            pending.keys.end(),
            batch.keys.begin(),
            batch.keys.end());

        pending.next_offset +=
            static_cast<std::uint32_t>(
                batch.keys.size());

        if (
            pending.next_offset <
            pending.stamp.key_count) {
            actions.push_back(
                snapshot_request_action(
                    batch.coordinator,
                    pending.stamp,
                    pending.next_offset));
            return actions;
        }

        const auto stamp =
            pending.stamp;
        auto keys =
            std::move(pending.keys);
        pending_.erase(
            pending_it);

        if (!host_.apply_remote_snapshot(
                batch.coordinator,
                stamp,
                keys)) {
            return actions;
        }

        reconcile_routes(
            batch.coordinator,
            stamp.incarnation,
            actions);

        return actions;
    }

    std::vector<ControlAction>
    ensure_routes(
        const InterestKey& key) {
        desired_routes_.insert(key);

        std::vector<ControlAction> actions;
        request_routes_for_key(
            key,
            actions);
        return actions;
    }

    bool release_routes(
        const InterestKey& key) {
        if (desired_routes_.erase(
                key) == 0) {
            return false;
        }

        for (auto requested_it =
                 requested_routes_.begin();
             requested_it !=
                 requested_routes_.end();) {
            const auto coordinator =
                requested_it->first;
            auto& requested =
                requested_it->second;
            const auto incarnation =
                host_.coordinator_incarnation(
                    coordinator);

            if (!incarnation) {
                requested_it =
                    requested_routes_.erase(
                        requested_it);
                continue;
            }

            const auto request_it =
                requested.find(key);

            if (request_it ==
                requested.end()) {
                ++requested_it;
                continue;
            }

            const std::span<
                const DestinationId> none;

            host_.replace_remote_routes(
                coordinator,
                *incarnation,
                key,
                none);
            requested.erase(
                request_it);

            if (requested.empty()) {
                requested_it =
                    requested_routes_.erase(
                        requested_it);
            } else {
                ++requested_it;
            }
        }

        return true;
    }

    ControlAction on_snapshot_request(
        const DemandSnapshotRequest& request)
        const {
        const auto stamp =
            host_.local_stamp(
                incarnation_);

        if (
            request.coordinator != self_ ||
            request.incarnation !=
                incarnation_ ||
            request.version !=
                stamp.version) {
            return {};
        }

        const auto snapshot =
            host_.local_snapshot();

        const auto batch =
            make_snapshot_batch(
                self_,
                stamp,
                snapshot,
                request.offset,
                request.limit);

        return {
            ControlTargetKind::Source,
            self_,
            encode_snapshot_batch(
                batch)};
    }

    ControlAction on_route_request(
        const RouteRequest& request,
        std::span<const RouteEndpoint>
            endpoints) const {
        if (
            request.coordinator != self_ ||
            request.incarnation !=
                incarnation_ ||
            !host_.has_local_demand(
                request.key)) {
            return {};
        }

        if (
            endpoints.size() >
            kMaxRouteBatch) {
            throw std::length_error(
                "Cito discovery control: route endpoint batch too large");
        }

        RouteBatch batch;
        batch.coordinator = self_;
        batch.incarnation =
            incarnation_;
        batch.key = request.key;
        batch.endpoints.assign(
            endpoints.begin(),
            endpoints.end());

        return {
            ControlTargetKind::Source,
            self_,
            encode_route_batch(
                batch)};
    }

    bool on_route_batch(
        const RouteBatch& batch) {
        if (
            !desired_routes_.contains(
                batch.key) ||
            !host_.coordinator_is_current(
                batch.coordinator,
                batch.incarnation) ||
            !host_.candidate_hosts(
                batch.key)
                 .contains(
                     batch.coordinator)) {
            ++stats_.stale_packets;
            return false;
        }

        std::vector<DestinationId>
            destinations;
        destinations.reserve(
            batch.endpoints.size());

        for (const auto& endpoint :
             batch.endpoints) {
            destinations.push_back(
                endpoint.destination);
        }

        host_.replace_remote_routes(
            batch.coordinator,
            batch.incarnation,
            batch.key,
            destinations);

        requested_routes_[
            batch.coordinator][
                batch.key] =
            batch.incarnation;

        return true;
    }

    std::vector<CoordinatorId>
    expire_coordinators(
        std::uint64_t now_ms) {
        auto expired =
            host_
                .expire_remote_coordinators(
                    now_ms);

        for (const auto coordinator :
             expired) {
            pending_.erase(
                coordinator);
            requested_routes_.erase(
                coordinator);
        }

        stats_.expired_coordinators +=
            expired.size();

        return expired;
    }

    std::size_t expire(
        std::uint64_t now_ms) {
        return expire_coordinators(
            now_ms).size();
    }

    const DiscoveryControlStats&
    stats() const noexcept {
        return stats_;
    }

    std::size_t pending_snapshot_count()
        const noexcept {
        return pending_.size();
    }

    std::size_t desired_route_count()
        const noexcept {
        return desired_routes_.size();
    }

private:
    ControlAction snapshot_request_action(
        CoordinatorId coordinator,
        const DemandSummaryStamp& stamp,
        std::uint32_t offset) {
        ++stats_.snapshot_requests;

        return {
            ControlTargetKind::Coordinator,
            coordinator,
            encode_snapshot_request(
                DemandSnapshotRequest{
                    coordinator,
                    stamp.incarnation,
                    stamp.version,
                    offset,
                    kMaxDemandSnapshotBatch})};
    }

    ControlAction route_request_action(
        CoordinatorId coordinator,
        std::uint64_t incarnation,
        const InterestKey& key) {
        ++stats_.route_requests;

        return {
            ControlTargetKind::Coordinator,
            coordinator,
            encode_route_request(
                RouteRequest{
                    coordinator,
                    incarnation,
                    key})};
    }

    void request_routes_for_key(
        const InterestKey& key,
        std::vector<ControlAction>& actions) {
        const auto candidates =
            host_.candidate_hosts(key);

        for (const auto coordinator :
             candidates) {
            const auto incarnation =
                host_.coordinator_incarnation(
                    coordinator);

            if (!incarnation) {
                continue;
            }

            auto& requested =
                requested_routes_[
                    coordinator];

            const auto it =
                requested.find(key);

            if (
                it != requested.end() &&
                it->second == *incarnation) {
                continue;
            }

            requested[key] =
                *incarnation;

            actions.push_back(
                route_request_action(
                    coordinator,
                    *incarnation,
                    key));
        }
    }

    void reconcile_routes(
        CoordinatorId coordinator,
        std::uint64_t incarnation,
        std::vector<ControlAction>& actions) {
        auto& requested =
            requested_routes_[
                coordinator];

        for (auto it =
                 requested.begin();
             it != requested.end();) {
            const auto still_desired =
                desired_routes_.contains(
                    it->first);
            const auto still_candidate =
                host_.candidate_hosts(
                    it->first)
                    .contains(
                        coordinator);

            if (
                still_desired &&
                still_candidate) {
                ++it;
                continue;
            }

            const std::span<
                const DestinationId> none;

            host_.replace_remote_routes(
                coordinator,
                incarnation,
                it->first,
                none);

            it =
                requested.erase(it);
        }

        for (const auto& key :
             desired_routes_) {
            if (
                !host_.candidate_hosts(
                    key)
                    .contains(
                        coordinator)) {
                continue;
            }

            const auto request_it =
                requested.find(key);

            if (
                request_it !=
                    requested.end() &&
                request_it->second ==
                    incarnation) {
                continue;
            }

            requested[key] =
                incarnation;

            actions.push_back(
                route_request_action(
                    coordinator,
                    incarnation,
                    key));
        }

        if (requested.empty()) {
            requested_routes_.erase(
                coordinator);
        }
    }

    HostRuntimeCore& host_;
    CoordinatorId self_{};
    std::uint64_t incarnation_{};

    std::unordered_map<
        CoordinatorId,
        PendingSnapshot> pending_;

    std::unordered_map<
        CoordinatorId,
        RequestedRoutes>
        requested_routes_;

    std::unordered_set<
        InterestKey,
        InterestKeyHash> desired_routes_;

    DiscoveryControlStats stats_;
};

} // namespace cito::detail
