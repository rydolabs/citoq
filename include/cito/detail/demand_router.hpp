#pragma once

#include <cito/interest.hpp>

#include <cstddef>
#include <functional>
#include <utility>

namespace cito::detail {

class DemandRouter {
public:
    explicit DemandRouter(const InterestIndex& interests) : interests_(interests) {}

    template <class Encode, class Send>
    std::size_t deliver(const InterestKey& key, Encode&& encode, Send&& send) const {
        const auto& destinations = interests_.lookup(key);
        if (destinations.empty()) {
            return 0;
        }

        auto payload = std::invoke(std::forward<Encode>(encode));
        for (const auto destination : destinations) {
            std::invoke(send, destination, payload);
        }
        return destinations.size();
    }

private:
    const InterestIndex& interests_;
};

} // namespace cito::detail
