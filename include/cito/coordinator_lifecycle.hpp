#pragma once

#include <cito/interest.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace cito {

enum class CoordinatorLeaseObservation : std::uint8_t {
    Added,
    Refreshed,
    Restarted,
    Stale,
};

class CoordinatorLeaseTable {
private:
    using ExpiryMap =
        std::multimap<std::uint64_t, CoordinatorId>;

    struct State {
        std::uint64_t incarnation{};
        std::uint32_t lease_ms{};
        ExpiryMap::iterator expiry_it;

        std::array<std::uint64_t, 4> retired{};
        std::size_t retired_count{0};
        std::size_t retired_next{0};

        bool is_retired(
            std::uint64_t candidate) const noexcept {
            for (std::size_t i = 0;
                 i < retired_count;
                 ++i) {
                if (retired[i] == candidate) {
                    return true;
                }
            }
            return false;
        }

        void retire(
            std::uint64_t old_incarnation) noexcept {
            retired[retired_next] =
                old_incarnation;
            retired_next =
                (retired_next + 1) %
                retired.size();

            if (retired_count <
                retired.size()) {
                ++retired_count;
            }
        }
    };

public:
    CoordinatorLeaseObservation observe(
        CoordinatorId coordinator,
        std::uint64_t incarnation,
        std::uint32_t lease_ms,
        std::uint64_t now_ms) {
        if (lease_ms == 0) {
            throw std::invalid_argument(
                "Cito coordinator lease must be non-zero");
        }

        const auto expires_at =
            now_ms + lease_ms;
        auto it = states_.find(coordinator);

        if (it == states_.end()) {
            const auto expiry_it =
                expiries_.emplace(
                    expires_at,
                    coordinator);

            State state;
            state.incarnation = incarnation;
            state.lease_ms = lease_ms;
            state.expiry_it = expiry_it;

            states_.emplace(
                coordinator,
                std::move(state));

            return CoordinatorLeaseObservation::Added;
        }

        auto& state = it->second;

        if (incarnation ==
            state.incarnation) {
            reschedule(
                coordinator,
                state,
                expires_at,
                lease_ms);

            return
                CoordinatorLeaseObservation::Refreshed;
        }

        if (state.is_retired(
                incarnation)) {
            return
                CoordinatorLeaseObservation::Stale;
        }

        state.retire(
            state.incarnation);
        state.incarnation =
            incarnation;

        reschedule(
            coordinator,
            state,
            expires_at,
            lease_ms);

        return
            CoordinatorLeaseObservation::Restarted;
    }

    bool is_current(
        CoordinatorId coordinator,
        std::uint64_t incarnation) const noexcept {
        const auto it =
            states_.find(coordinator);

        return
            it != states_.end() &&
            it->second.incarnation ==
                incarnation;
    }

    std::optional<std::uint64_t>
    current_incarnation(
        CoordinatorId coordinator) const noexcept {
        const auto it =
            states_.find(coordinator);

        if (it == states_.end()) {
            return std::nullopt;
        }

        return it->second.incarnation;
    }

    std::vector<CoordinatorId> expire(
        std::uint64_t now_ms) {
        std::vector<CoordinatorId> expired;

        while (!expiries_.empty() &&
               expiries_.begin()->first <=
                   now_ms) {
            const auto expiry_it =
                expiries_.begin();
            const auto coordinator =
                expiry_it->second;

            auto state_it =
                states_.find(coordinator);

            if (state_it !=
                    states_.end() &&
                state_it->second.expiry_it ==
                    expiry_it) {
                states_.erase(state_it);
                expired.push_back(
                    coordinator);
            }

            expiries_.erase(
                expiry_it);
        }

        return expired;
    }

    bool remove(
        CoordinatorId coordinator) {
        const auto it =
            states_.find(coordinator);

        if (it == states_.end()) {
            return false;
        }

        expiries_.erase(
            it->second.expiry_it);
        states_.erase(it);
        return true;
    }

    std::size_t size() const noexcept {
        return states_.size();
    }

    std::size_t scheduled_expiry_count()
        const noexcept {
        return expiries_.size();
    }

private:
    void reschedule(
        CoordinatorId coordinator,
        State& state,
        std::uint64_t expires_at,
        std::uint32_t lease_ms) {
        expiries_.erase(
            state.expiry_it);

        state.lease_ms =
            lease_ms;
        state.expiry_it =
            expiries_.emplace(
                expires_at,
                coordinator);
    }

    std::unordered_map<
        CoordinatorId,
        State> states_;
    ExpiryMap expiries_;
};

} // namespace cito
