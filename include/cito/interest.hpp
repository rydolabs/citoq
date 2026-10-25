#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cito {

using ScopeId = std::uint64_t;
using ResourceId = std::uint64_t;
using TypeId = std::uint64_t;
using DestinationId = std::uint64_t;
using CoordinatorId = std::uint64_t;

struct DemandSummaryStamp {
    std::uint64_t incarnation{};
    std::uint64_t version{};
    std::uint32_t key_count{};

    friend bool operator==(
        const DemandSummaryStamp&,
        const DemandSummaryStamp&) = default;
};

class RemoteSummaryTracker {
private:
    struct State {
        DemandSummaryStamp current{};
        std::array<std::uint64_t, 4> retired{};
        std::size_t retired_count{0};
        std::size_t retired_next{0};

        bool is_retired(std::uint64_t incarnation) const noexcept {
            for (std::size_t i = 0; i < retired_count; ++i) {
                if (retired[i] == incarnation) return true;
            }
            return false;
        }

        void retire(std::uint64_t incarnation) noexcept {
            retired[retired_next] = incarnation;
            retired_next = (retired_next + 1) % retired.size();
            if (retired_count < retired.size()) {
                ++retired_count;
            }
        }
    };

public:
    bool needs_snapshot(
        CoordinatorId coordinator,
        const DemandSummaryStamp& advertised) const {
        const auto it = applied_.find(coordinator);
        if (it == applied_.end()) return true;

        const auto& state = it->second;
        if (advertised.incarnation != state.current.incarnation) {
            return !state.is_retired(advertised.incarnation);
        }

        if (advertised.version < state.current.version) {
            return false;
        }
        if (advertised.version > state.current.version) {
            return true;
        }

        return advertised.key_count != state.current.key_count;
    }

    bool mark_applied(
        CoordinatorId coordinator,
        DemandSummaryStamp stamp) {
        auto it = applied_.find(coordinator);
        if (it == applied_.end()) {
            applied_.emplace(coordinator, State{stamp});
            return true;
        }

        auto& state = it->second;

        if (stamp.incarnation == state.current.incarnation) {
            if (stamp.version < state.current.version) {
                return false;
            }
            state.current = stamp;
            return true;
        }

        if (state.is_retired(stamp.incarnation)) {
            return false;
        }

        state.retire(state.current.incarnation);
        state.current = stamp;
        return true;
    }

    bool forget(CoordinatorId coordinator) {
        return applied_.erase(coordinator) != 0;
    }

    std::size_t size() const noexcept {
        return applied_.size();
    }

private:
    std::unordered_map<CoordinatorId, State> applied_;
};

struct InterestKey {
    ScopeId scope{};
    ResourceId resource{};
    TypeId type{};

    friend bool operator==(const InterestKey&, const InterestKey&) = default;
};

struct InterestKeyHash {
    std::size_t operator()(const InterestKey& key) const noexcept {
        auto mix = [](std::uint64_t x) {
            x ^= x >> 33;
            x *= 0xff51afd7ed558ccdULL;
            x ^= x >> 33;
            x *= 0xc4ceb9fe1a85ec53ULL;
            x ^= x >> 33;
            return x;
        };

        auto h = mix(key.scope);
        h ^= mix(
            key.resource +
            0x9e3779b97f4a7c15ULL +
            (h << 6) +
            (h >> 2));
        h ^= mix(
            key.type +
            0x9e3779b97f4a7c15ULL +
            (h << 6) +
            (h >> 2));
        return static_cast<std::size_t>(h);
    }
};

class HostInterests {
public:
    explicit HostInterests(std::size_t expected_unique = 0) {
        if (expected_unique != 0) {
            counts_.reserve(expected_unique);
        }
    }

    bool add(const InterestKey& key) {
        auto& count = counts_[key];
        ++count;

        if (count == 1) {
            ++version_;
            return true;
        }
        return false;
    }

    bool remove(const InterestKey& key) {
        auto it = counts_.find(key);
        if (it == counts_.end()) return false;

        if (--it->second == 0) {
            counts_.erase(it);
            ++version_;
            return true;
        }
        return false;
    }

    bool contains(const InterestKey& key) const {
        return counts_.contains(key);
    }

    std::size_t unique_interest_count() const noexcept {
        return counts_.size();
    }

    std::uint64_t version() const noexcept {
        return version_;
    }

    DemandSummaryStamp stamp(
        std::uint64_t incarnation) const {
        if (counts_.size() >
            std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error(
                "Cito host demand summary exceeds uint32 key count");
        }

        return DemandSummaryStamp{
            incarnation,
            version_,
            static_cast<std::uint32_t>(counts_.size())};
    }

    std::vector<InterestKey> snapshot() const {
        std::vector<InterestKey> keys;
        keys.reserve(counts_.size());

        for (const auto& [key, count] : counts_) {
            (void)count;
            keys.push_back(key);
        }

        std::sort(
            keys.begin(),
            keys.end(),
            [](const InterestKey& a, const InterestKey& b) {
                if (a.scope != b.scope) {
                    return a.scope < b.scope;
                }
                if (a.resource != b.resource) {
                    return a.resource < b.resource;
                }
                return a.type < b.type;
            });

        return keys;
    }

private:
    std::unordered_map<
        InterestKey,
        std::size_t,
        InterestKeyHash> counts_;
    std::uint64_t version_{0};
};

class InterestIndex {
public:
    explicit InterestIndex(std::size_t expected_keys = 0) {
        if (expected_keys != 0) {
            table_.reserve(expected_keys);
        }
    }

    void reserve(std::size_t expected_keys) {
        table_.reserve(expected_keys);
    }

    bool add(
        const InterestKey& key,
        DestinationId destination) {
        auto& destinations = table_[key];
        const auto [_, inserted] =
            destinations.insert(destination);

        if (inserted) ++edge_count_;
        return inserted;
    }

    bool remove(
        const InterestKey& key,
        DestinationId destination) {
        auto it = table_.find(key);
        if (it == table_.end()) return false;

        if (it->second.erase(destination) == 0) {
            return false;
        }

        --edge_count_;
        if (it->second.empty()) {
            table_.erase(it);
        }
        return true;
    }

    const std::unordered_set<DestinationId>& lookup(
        const InterestKey& key) const noexcept {
        const auto it = table_.find(key);
        return it == table_.end() ? empty_ : it->second;
    }

    std::size_t key_count() const noexcept {
        return table_.size();
    }

    std::size_t edge_count() const noexcept {
        return edge_count_;
    }

private:
    std::unordered_map<
        InterestKey,
        std::unordered_set<DestinationId>,
        InterestKeyHash> table_;
    std::unordered_set<DestinationId> empty_;
    std::size_t edge_count_{0};
};

} // namespace cito
