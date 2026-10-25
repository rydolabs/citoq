#pragma once

#include <cito/interest.hpp>

#include <span>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cito {

class RemoteDemandIndex {
public:
    bool apply_snapshot(
        CoordinatorId coordinator,
        DemandSummaryStamp stamp,
        std::span<const InterestKey> keys) {
        std::unordered_set<
            InterestKey,
            InterestKeyHash> unique;
        unique.reserve(keys.size());

        for (const auto& key : keys) {
            unique.insert(key);
        }

        if (unique.size() != keys.size() ||
            unique.size() != stamp.key_count) {
            throw std::invalid_argument(
                "Cito demand summary: key_count or uniqueness mismatch");
        }

        auto existing = hosts_.find(coordinator);

        if (existing != hosts_.end()) {
            if (stamp.incarnation ==
                    existing->second.stamp.incarnation &&
                stamp.version <
                    existing->second.stamp.version) {
                return false;
            }

            if (stamp == existing->second.stamp) {
                return false;
            }

            remove_inverse(
                coordinator,
                existing->second.keys);
        }

        for (const auto& key : unique) {
            inverse_[key].insert(coordinator);
        }

        hosts_.insert_or_assign(
            coordinator,
            HostState{
                stamp,
                std::move(unique)});

        return true;
    }

    bool remove_host(CoordinatorId coordinator) {
        const auto it = hosts_.find(coordinator);
        if (it == hosts_.end()) return false;

        remove_inverse(
            coordinator,
            it->second.keys);
        hosts_.erase(it);
        return true;
    }

    const std::unordered_set<CoordinatorId>& lookup_hosts(
        const InterestKey& key) const noexcept {
        const auto it = inverse_.find(key);
        return it == inverse_.end()
            ? empty_
            : it->second;
    }

    std::size_t host_count() const noexcept {
        return hosts_.size();
    }

    std::size_t key_count() const noexcept {
        return inverse_.size();
    }

private:
    struct HostState {
        DemandSummaryStamp stamp{};
        std::unordered_set<
            InterestKey,
            InterestKeyHash> keys;
    };

    void remove_inverse(
        CoordinatorId coordinator,
        const std::unordered_set<
            InterestKey,
            InterestKeyHash>& keys) {
        for (const auto& key : keys) {
            auto it = inverse_.find(key);
            if (it == inverse_.end()) continue;

            it->second.erase(coordinator);

            if (it->second.empty()) {
                inverse_.erase(it);
            }
        }
    }

    std::unordered_map<
        CoordinatorId,
        HostState> hosts_;
    std::unordered_map<
        InterestKey,
        std::unordered_set<CoordinatorId>,
        InterestKeyHash> inverse_;
    std::unordered_set<CoordinatorId> empty_;
};

} // namespace cito
