#pragma once

#include <cito/interest.hpp>

#include <cstddef>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cito {

class RemoteRouteIndex {
public:
    bool replace(
        CoordinatorId coordinator,
        const InterestKey& key,
        std::span<const DestinationId>
            destinations) {
        DestinationSet next;
        next.reserve(
            destinations.size());

        for (const auto destination :
             destinations) {
            next.insert(destination);
        }

        auto& host_routes =
            hosts_[coordinator];
        const auto existing =
            host_routes.find(key);

        if (existing !=
                host_routes.end() &&
            existing->second == next) {
            return false;
        }

        if (existing !=
            host_routes.end()) {
            remove_edges(
                key,
                existing->second);
        }

        if (next.empty()) {
            if (existing !=
                host_routes.end()) {
                host_routes.erase(
                    existing);
            }

            if (host_routes.empty()) {
                hosts_.erase(
                    coordinator);
            }
            return true;
        }

        add_edges(
            key,
            next);

        host_routes.insert_or_assign(
            key,
            std::move(next));
        return true;
    }

    std::vector<DestinationId>
    remove_coordinator(
        CoordinatorId coordinator) {
        const auto host_it =
            hosts_.find(coordinator);

        if (host_it ==
            hosts_.end()) {
            return {};
        }

        std::unordered_set<
            DestinationId> removed;

        for (const auto& [key, destinations] :
             host_it->second) {
            for (const auto destination :
                 destinations) {
                removed.insert(
                    destination);
            }

            remove_edges(
                key,
                destinations);
        }

        hosts_.erase(
            host_it);

        return {
            removed.begin(),
            removed.end()};
    }

    const std::unordered_set<
        DestinationId>& lookup(
        CoordinatorId coordinator,
        const InterestKey& key) const noexcept {
        const auto host_it =
            hosts_.find(coordinator);

        if (host_it ==
            hosts_.end()) {
            return empty_;
        }

        const auto route_it =
            host_it->second.find(key);

        return
            route_it ==
                host_it->second.end()
                ? empty_
                : route_it->second;
    }

    const std::unordered_set<
        DestinationId>& lookup(
        const InterestKey& key) const noexcept {
        const auto it =
            visible_.find(key);

        return it == visible_.end()
            ? empty_
            : it->second;
    }

    std::size_t coordinator_count()
        const noexcept {
        return hosts_.size();
    }

    std::size_t edge_count()
        const noexcept {
        return visible_edge_count_;
    }

private:
    using DestinationSet =
        std::unordered_set<DestinationId>;

    using HostRouteMap =
        std::unordered_map<
            InterestKey,
            DestinationSet,
            InterestKeyHash>;

    using CountMap =
        std::unordered_map<
            DestinationId,
            std::size_t>;

    void add_edges(
        const InterestKey& key,
        const DestinationSet& destinations) {
        auto& counts =
            counts_[key];
        auto& visible =
            visible_[key];

        for (const auto destination :
             destinations) {
            auto& count =
                counts[destination];

            ++count;

            if (count == 1) {
                visible.insert(
                    destination);
                ++visible_edge_count_;
            }
        }
    }

    void remove_edges(
        const InterestKey& key,
        const DestinationSet& destinations) {
        auto counts_it =
            counts_.find(key);

        if (counts_it ==
            counts_.end()) {
            return;
        }

        auto visible_it =
            visible_.find(key);

        for (const auto destination :
             destinations) {
            const auto count_it =
                counts_it->second.find(
                    destination);

            if (count_it ==
                counts_it->second.end()) {
                continue;
            }

            if (--count_it->second == 0) {
                counts_it->second.erase(
                    count_it);

                if (visible_it !=
                    visible_.end()) {
                    if (visible_it->second.erase(
                            destination) != 0) {
                        --visible_edge_count_;
                    }
                }
            }
        }

        if (counts_it->second.empty()) {
            counts_.erase(
                counts_it);
        }

        if (visible_it !=
                visible_.end() &&
            visible_it->second.empty()) {
            visible_.erase(
                visible_it);
        }
    }

    std::unordered_map<
        CoordinatorId,
        HostRouteMap> hosts_;

    std::unordered_map<
        InterestKey,
        CountMap,
        InterestKeyHash> counts_;

    std::unordered_map<
        InterestKey,
        DestinationSet,
        InterestKeyHash> visible_;

    DestinationSet empty_;
    std::size_t visible_edge_count_{0};
};

} // namespace cito
