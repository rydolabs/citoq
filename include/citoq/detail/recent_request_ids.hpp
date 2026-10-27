#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_set>

namespace citoq::detail {

// Bounded, session-scoped replay tombstones for CTX1 request IDs.
// A peer identifier must be unique for the lifetime of each accepted
// transport session; it is NOT a certificate identity or idempotency key.
class RecentRequestIds {
public:
    struct Key {
        std::uint64_t peer{};
        std::uint64_t request_id{};

        bool operator==(const Key&) const noexcept = default;
    };

    struct Hash {
        std::size_t operator()(const Key& key) const noexcept {
            auto mix = [](std::uint64_t value) {
                value ^= value >> 33;
                value *= 0xff51afd7ed558ccdULL;
                value ^= value >> 33;
                value *= 0xc4ceb9fe1a85ec53ULL;
                value ^= value >> 33;
                return value;
            };
            return static_cast<std::size_t>(
                mix(key.peer) ^ (mix(key.request_id) +
                                  0x9e3779b97f4a7c15ULL));
        }
    };

    explicit RecentRequestIds(std::size_t capacity = 1024)
        : capacity_(capacity) {}

    bool contains(std::uint64_t peer, std::uint64_t id) const noexcept {
        return entries_.contains({peer, id});
    }

    // False means it was already remembered. Oldest entries are retired
    // when full, so this is bounded duplicate suppression, NOT exactly-once.
    bool remember(std::uint64_t peer, std::uint64_t id) {
        if (capacity_ == 0) return false;
        const Key key{peer, id};
        if (entries_.contains(key)) return false;
        // Push first so allocation failure does not evict existing records.
        order_.push_back(key);
        try {
            entries_.insert(key);
        } catch (...) {
            order_.pop_back();
            throw;
        }
        if (order_.size() > capacity_) {
            entries_.erase(order_.front());
            order_.pop_front();
        }
        return true;
    }

    void forget_peer(std::uint64_t peer) {
        for (auto it = order_.begin(); it != order_.end();) {
            if (it->peer == peer) {
                entries_.erase(*it);
                it = order_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::size_t size() const noexcept { return entries_.size(); }
    std::size_t capacity() const noexcept { return capacity_; }

private:
    const std::size_t capacity_;
    std::deque<Key> order_;
    std::unordered_set<Key, Hash> entries_;
};

} // namespace citoq::detail
