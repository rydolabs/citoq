#pragma once

#include <citoq/demand_control.hpp>
#include <citoq/detail/host_runtime_core.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace citoq::detail {

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
    std::size_t snapshot_retries{0};
    std::size_t route_retries{0};
    std::size_t route_anti_entropy_probes{0};
};

class DiscoveryControlCore {
private:
    struct PendingSnapshot {
        DemandSummaryStamp stamp{};
        std::vector<InterestKey> keys;
        std::uint32_t next_offset{};
        std::uint64_t next_retry_ms{};
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
        last_observed_now_ms_ = now_ms;

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
            unconfirmed_routes_.erase(message.coordinator);
            next_route_retry_ms_.erase(message.coordinator);
            next_audit_ms_.erase(message.coordinator);
        }

        if (!host_.needs_remote_snapshot(
                message.coordinator,
                message.stamp)) {
#if !defined(CITOQ_DISCOVERY_RETRY) || CITOQ_DISCOVERY_RETRY
            retry_unconfirmed_routes(message.coordinator,
                message.stamp.incarnation, now_ms, actions);
#endif
#if defined(CITOQ_PLUS_DISCOVERY) && CITOQ_PLUS_DISCOVERY
            anti_entropy_confirmed_routes(message.coordinator,
                message.stamp.incarnation, now_ms, actions);
#endif
            return actions;
        }

        const auto pending_it =
            pending_.find(
                message.coordinator);

        if (
            pending_it != pending_.end() &&
            pending_it->second.stamp ==
                message.stamp) {
#if !defined(CITOQ_DISCOVERY_RETRY) || CITOQ_DISCOVERY_RETRY
            // Repeated announcements prove the coordinator is alive, but do
            // not prove its earlier UDP snapshot reply was received.
            // Retry from the last verified page, never replay applied pages.
            if (now_ms >= pending_it->second.next_retry_ms) {
                pending_it->second.next_retry_ms = now_ms + 300;
                ++stats_.snapshot_retries;
                actions.push_back(snapshot_request_action(
                    message.coordinator, message.stamp,
                    pending_it->second.next_offset));
            }
#endif
            return actions;
        }

        pending_.insert_or_assign(
            message.coordinator,
            PendingSnapshot{
                message.stamp,
                {},
                0,
                now_ms + 300});

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

        for (auto& [coordinator, pending] : unconfirmed_routes_) {
            pending.erase(key);
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
            if (auto pending = unconfirmed_routes_.find(coordinator);
                pending != unconfirmed_routes_.end()) {
                pending->second.erase(key);
            }

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
                "CitoQ discovery control: route endpoint batch too large");
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
#if defined(CITOQ_PLUS_DISCOVERY) && CITOQ_PLUS_DISCOVERY
        next_audit_ms_[batch.coordinator] = last_observed_now_ms_ + 2400;
#endif
        if (auto it = unconfirmed_routes_.find(batch.coordinator);
            it != unconfirmed_routes_.end()) {
            it->second.erase(batch.key);
            if (it->second.empty()) {
                unconfirmed_routes_.erase(it);
                next_route_retry_ms_.erase(batch.coordinator);
            }
        }

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
            unconfirmed_routes_.erase(coordinator);
            next_route_retry_ms_.erase(coordinator);
            next_audit_ms_.erase(coordinator);
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
#if defined(CITOQ_PLUS_DISCOVERY) && CITOQ_PLUS_DISCOVERY
    // A+-inspired anti-entropy: periodically verify *only desired* DemandKey
    // endpoint routes, not a full peer/destination membership catalogue.
    // Existing CTR3/CTR5 request/response encoding remains unchanged.
    void anti_entropy_confirmed_routes(
        CoordinatorId coordinator,
        std::uint64_t incarnation,
        std::uint64_t now_ms,
        std::vector<ControlAction>& actions) {
        auto& due = next_audit_ms_[coordinator];
        if (due == 0) {
            due = now_ms + 2400 + (coordinator % 401);
            return;
        }
        if (now_ms < due) return;
        due = now_ms + 2400 + (coordinator % 401);
        const auto it = requested_routes_.find(coordinator);
        if (it == requested_routes_.end()) return;
        std::size_t budget = 4;
        for (const auto& [key, version] : it->second) {
            if (budget == 0) break;
            if (version != incarnation || !desired_routes_.contains(key)
                || !host_.candidate_hosts(key).contains(coordinator)) continue;
            const auto unconfirmed = unconfirmed_routes_.find(coordinator);
            if (unconfirmed != unconfirmed_routes_.end() &&
                unconfirmed->second.contains(key)) continue;
            actions.push_back(route_request_action(coordinator, incarnation, key));
            ++stats_.route_anti_entropy_probes;
            --budget;
        }
    }
#endif

    void retry_unconfirmed_routes(
        CoordinatorId coordinator,
        std::uint64_t incarnation,
        std::uint64_t now_ms,
        std::vector<ControlAction>& actions) {
        const auto pending = unconfirmed_routes_.find(coordinator);
        if (pending == unconfirmed_routes_.end() || pending->second.empty())
            return;
        auto& next_retry = next_route_retry_ms_[coordinator];
        if (next_retry != 0 && now_ms < next_retry) return;
        next_retry = now_ms + 300;
        for (const auto& key : pending->second) {
            if (!desired_routes_.contains(key) ||
                !host_.candidate_hosts(key).contains(coordinator))
                continue;
            const auto request_it = requested_routes_.find(coordinator);
            if (request_it == requested_routes_.end())
                continue;
            const auto requested = request_it->second.find(key);
            if (requested == request_it->second.end() ||
                requested->second != incarnation)
                continue;
            ++stats_.route_retries;
            actions.push_back(route_request_action(
                coordinator, incarnation, key));
        }
    }

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
            unconfirmed_routes_[coordinator].insert(key);

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

            if (auto pending = unconfirmed_routes_.find(coordinator);
                pending != unconfirmed_routes_.end()) {
                pending->second.erase(it->first);
            }
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
            unconfirmed_routes_[coordinator].insert(key);

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

    // Route request registration is not a delivery acknowledgement. Keep a
    // separate pending set until a validated CTR4/CTR5 batch arrives.
    std::unordered_map<
        CoordinatorId,
        std::unordered_set<InterestKey, InterestKeyHash>> unconfirmed_routes_;
    std::unordered_map<CoordinatorId, std::uint64_t> next_route_retry_ms_;

    std::uint64_t last_observed_now_ms_{0};
    std::unordered_map<CoordinatorId, std::uint64_t> next_audit_ms_;
    DiscoveryControlStats stats_;
};

} // namespace citoq::detail
