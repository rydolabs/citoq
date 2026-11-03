#pragma once

#include <citoq/detail/reliability.hpp>
#include <citoq/interest.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace citoq::detail::reliable {

// Opt-in discrete-message recovery across QUIC connection loss.
// MsQuic already retransmits within a live connection: this ledger tracks
// *application* receipt, and is not a second packet-level QUIC retransmitter.
// This single-owner state machine has no networking or callback side effects.
struct QuicDeliveryId {
    InterestKey key{};
    std::uint64_t stream{};
    SequenceNumber sequence{};

    friend bool operator==(const QuicDeliveryId&, const QuicDeliveryId&) = default;
};

struct QuicDeliveryTarget {
    DestinationId destination{};
    QuicDeliveryId id{};

    friend bool operator==(const QuicDeliveryTarget&, const QuicDeliveryTarget&) = default;
};

struct QuicReplayAction {
    QuicDeliveryTarget target{};
    // Valid only until the ledger is mutated; the transport must copy/queue
    // the bytes before acknowledging, expiring, or canceling the target.
    std::span<const std::uint8_t> payload;
    std::uint32_t attempt{};
};

struct QuicDeliveryTargetLess {
    bool operator()(
        const QuicDeliveryTarget& lhs,
        const QuicDeliveryTarget& rhs) const noexcept {
        return std::tie(
            lhs.destination, lhs.id.key.scope, lhs.id.key.resource,
            lhs.id.key.type, lhs.id.stream, lhs.id.sequence) <
            std::tie(
            rhs.destination, rhs.id.key.scope, rhs.id.key.resource,
            rhs.id.key.type, rhs.id.stream, rhs.id.sequence);
    }
};

class QuicDeliveryLedger {
private:
    struct Pending {
        std::vector<std::uint8_t> payload;
        std::uint64_t created_ms{};
        std::uint64_t due_ms{};
        std::uint32_t attempts{};
    };

public:
    QuicDeliveryLedger(
        std::size_t max_messages,
        std::size_t max_payload_bytes,
        std::uint64_t first_retry_ms = 100,
        std::uint64_t max_retry_ms = 2000)
        : max_messages_(max_messages),
          max_payload_bytes_(max_payload_bytes),
          first_retry_ms_(first_retry_ms),
          max_retry_ms_(max_retry_ms) {
        if (max_messages == 0 || max_payload_bytes == 0 ||
            first_retry_ms == 0 || first_retry_ms > max_retry_ms) {
            throw std::invalid_argument("invalid QUIC receipt ledger bounds");
        }
    }

    // Rejects duplicate IDs and exhausted memory rather than dropping old
    // unacknowledged messages. No copies are made for rejected publications.
    bool track(
        QuicDeliveryTarget target,
        std::span<const std::uint8_t> payload,
        std::uint64_t now_ms) {
        if (target.destination == 0 || target.id.stream == 0 ||
            target.id.sequence == 0) {
            throw std::invalid_argument("invalid QUIC delivery identity");
        }
        if (pending_.contains(target) || pending_.size() == max_messages_ ||
            payload.size() > max_payload_bytes_ - pending_bytes_) {
            return false;
        }

        Pending value;
        value.payload.assign(payload.begin(), payload.end());
        value.created_ms = now_ms;
        value.due_ms = now_ms;
        auto [it, inserted] = pending_.emplace(target, std::move(value));
        if (inserted) pending_bytes_ += payload.size();
        return inserted;
    }

    // The caller must authenticate the ACK's source and DemandKey before
    // invoking this function; an unknown/stale or duplicate receipt is ignored.
    bool acknowledge(const QuicDeliveryTarget& target) {
        const auto it = pending_.find(target);
        if (it == pending_.end()) return false;
        pending_bytes_ -= it->second.payload.size();
        pending_.erase(it);
        return true;
    }

    void connected(DestinationId destination, std::uint64_t now_ms) {
        if (destination == 0) {
            throw std::invalid_argument("invalid QUIC destination");
        }
        connected_.insert(destination);
        for (auto& [target, entry] : pending_) {
            if (target.destination == destination)
                entry.due_ms = now_ms; // replay only this destination
        }
    }

    void disconnected(DestinationId destination) {
        connected_.erase(destination);
    }

    // Return only bounded, still-unacknowledged messages. The transport is
    // free to re-send after an ACK timeout; receiver dedup is mandatory.
    std::vector<QuicReplayAction> due(
        std::uint64_t now_ms, std::size_t max_actions) {
        std::vector<QuicReplayAction> actions;
        for (auto& [target, entry] : pending_) {
            if (actions.size() >= max_actions) break;
            if (!connected_.contains(target.destination) ||
                entry.due_ms > now_ms) continue;

            if (entry.attempts != std::numeric_limits<std::uint32_t>::max())
                ++entry.attempts;
            actions.push_back(QuicReplayAction{
                target,
                std::span<const std::uint8_t>(
                    entry.payload.data(), entry.payload.size()),
                entry.attempts});
            entry.due_ms = saturated_add(
                now_ms, delay_after(entry.attempts));
        }
        return actions;
    }

    // Expiration is explicitly reported to the caller as failed delivery,
    // never silently turned into a success. A durable outbox is not provided.
    std::vector<QuicDeliveryTarget> expire(
        std::uint64_t now_ms, std::uint64_t retention_ms) {
        if (retention_ms == 0)
            throw std::invalid_argument("zero QUIC ledger retention");
        std::vector<QuicDeliveryTarget> expired;
        for (auto it = pending_.begin(); it != pending_.end();) {
            const auto& entry = it->second;
            if (now_ms >= entry.created_ms &&
                now_ms - entry.created_ms >= retention_ms) {
                expired.push_back(it->first);
                pending_bytes_ -= entry.payload.size();
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        return expired;
    }

    std::size_t cancel_destination(DestinationId destination) {
        connected_.erase(destination);
        std::size_t removed = 0;
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->first.destination == destination) {
                pending_bytes_ -= it->second.payload.size();
                it = pending_.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }

    std::size_t pending_count() const noexcept { return pending_.size(); }
    std::size_t pending_bytes() const noexcept { return pending_bytes_; }

private:
    static std::uint64_t saturated_add(
        std::uint64_t lhs, std::uint64_t rhs) noexcept {
        const auto max = std::numeric_limits<std::uint64_t>::max();
        return lhs > max - rhs ? max : lhs + rhs;
    }

    std::uint64_t delay_after(std::uint32_t attempt) const noexcept {
        auto delay = first_retry_ms_;
        for (std::uint32_t n = 1; n < attempt && delay < max_retry_ms_; ++n)
            delay = delay > max_retry_ms_ / 2 ? max_retry_ms_ : delay * 2;
        return std::min(delay, max_retry_ms_);
    }

    const std::size_t max_messages_;
    const std::size_t max_payload_bytes_;
    const std::uint64_t first_retry_ms_;
    const std::uint64_t max_retry_ms_;
    std::size_t pending_bytes_{};
    std::map<QuicDeliveryTarget, Pending, QuicDeliveryTargetLess> pending_;
    std::set<DestinationId> connected_;
};

} // namespace citoq::detail::reliable
