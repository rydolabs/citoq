#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace cito::detail::reliable {

using SequenceNumber = std::uint64_t;

struct Heartbeat {
    SequenceNumber first_available{};
    SequenceNumber last_published{};
};

struct Nack {
    SequenceNumber base{};
    std::uint64_t bitmap{};
};

struct Gap {
    SequenceNumber first{};
    SequenceNumber last{};
};

enum class RecoveryKind : std::uint8_t {
    Retransmit,
    Gap,
};

struct RecoveryAction {
    RecoveryKind kind{RecoveryKind::Retransmit};
    SequenceNumber first{};
    SequenceNumber last{};
};

struct RecoveryPlan {
    std::vector<RecoveryAction> actions;

    bool empty() const noexcept {
        return actions.empty();
    }
};

class SenderHistory {
private:
    struct Slot {
        SequenceNumber sequence{};
        std::vector<std::uint8_t> payload;
    };

public:
    SenderHistory(
        std::size_t sample_capacity,
        std::size_t max_payload_size)
        : sample_capacity_(sample_capacity),
          max_payload_size_(max_payload_size),
          slots_(sample_capacity) {
        if (
            sample_capacity_ == 0 ||
            max_payload_size_ == 0) {
            throw std::invalid_argument(
                "Cito reliable history bounds must be non-zero");
        }

        // Reliable memory is paid only when this object exists, and the
        // per-slot payload capacity is reserved up front so steady-state
        // publish does not grow memory with runtime duration.
        for (auto& slot : slots_) {
            slot.payload.reserve(
                max_payload_size_);
        }
    }

    SequenceNumber publish(
        std::span<const std::uint8_t> payload) {
        if (
            payload.size() >
            max_payload_size_) {
            throw std::length_error(
                "Cito reliable payload exceeds history slot bound");
        }

        if (
            next_sequence_ ==
            std::numeric_limits<
                SequenceNumber>::max()) {
            throw std::overflow_error(
                "Cito reliable sequence exhausted");
        }

        const auto sequence =
            next_sequence_++;

        auto& slot =
            slots_[
                static_cast<std::size_t>(
                    (sequence - 1) %
                    sample_capacity_)];

        slot.sequence =
            sequence;
        slot.payload.assign(
            payload.begin(),
            payload.end());

        if (
            retained_count_ <
            sample_capacity_) {
            ++retained_count_;
        }

        return sequence;
    }

    SequenceNumber first_available()
        const noexcept {
        return
            retained_count_ == 0
                ? next_sequence_
                : next_sequence_ -
                    retained_count_;
    }

    SequenceNumber last_published()
        const noexcept {
        return next_sequence_ - 1;
    }

    Heartbeat heartbeat()
        const noexcept {
        return {
            first_available(),
            last_published()};
    }

    std::optional<
        std::span<const std::uint8_t>>
    payload(
        SequenceNumber sequence)
        const noexcept {
        if (
            sequence == 0 ||
            sequence <
                first_available() ||
            sequence >
                last_published()) {
            return std::nullopt;
        }

        const auto& slot =
            slots_[
                static_cast<std::size_t>(
                    (sequence - 1) %
                    sample_capacity_)];

        if (
            slot.sequence !=
            sequence) {
            return std::nullopt;
        }

        return std::span<
            const std::uint8_t>(
                slot.payload.data(),
                slot.payload.size());
    }

    RecoveryPlan plan_recovery(
        const Nack& nack) const {
        if (nack.base == 0) {
            throw std::invalid_argument(
                "Cito reliable NACK base 0 is invalid");
        }

        RecoveryPlan plan;
        std::optional<Gap>
            pending_gap;

        const auto flush_gap =
            [&]() {
                if (pending_gap) {
                    plan.actions.push_back(
                        RecoveryAction{
                            RecoveryKind::Gap,
                            pending_gap->first,
                            pending_gap->last});
                    pending_gap.reset();
                }
            };

        for (unsigned bit = 0;
             bit < 64;
             ++bit) {
            if (
                (nack.bitmap &
                 (std::uint64_t{1}
                  << bit)) == 0) {
                continue;
            }

            if (
                nack.base >
                std::numeric_limits<
                    SequenceNumber>::max() -
                    bit) {
                break;
            }

            const auto sequence =
                nack.base + bit;

            // Ignore malformed/future requests instead of growing state.
            if (
                sequence >
                last_published()) {
                continue;
            }

            if (payload(sequence)) {
                flush_gap();
                plan.actions.push_back(
                    RecoveryAction{
                        RecoveryKind::Retransmit,
                        sequence,
                        sequence});
                continue;
            }

            if (
                pending_gap &&
                pending_gap->last !=
                    std::numeric_limits<
                        SequenceNumber>::max() &&
                pending_gap->last + 1 ==
                    sequence) {
                pending_gap->last =
                    sequence;
            } else {
                flush_gap();
                pending_gap =
                    Gap{
                        sequence,
                        sequence};
            }
        }

        flush_gap();
        return plan;
    }

    std::size_t sample_capacity()
        const noexcept {
        return sample_capacity_;
    }

    std::size_t max_payload_size()
        const noexcept {
        return max_payload_size_;
    }

private:
    const std::size_t sample_capacity_;
    const std::size_t max_payload_size_;
    std::vector<Slot> slots_;
    std::size_t retained_count_{0};
    SequenceNumber next_sequence_{1};
};

enum class ReceiveResult : std::uint8_t {
    AcceptedOutOfOrder,
    Advanced,
    Duplicate,
    OutOfWindow,
};

class ReceiverWindow {
public:
    explicit ReceiverWindow(
        std::size_t window_size = 1024)
        : window_size_(window_size),
          seen_(window_size, 0) {
        if (window_size_ < 64) {
            throw std::invalid_argument(
                "Cito reliable receive window must be at least 64");
        }
    }

    ReceiveResult on_data(
        SequenceNumber sequence) {
        if (sequence == 0) {
            throw std::invalid_argument(
                "Cito reliable sequence 0 is invalid");
        }

        known_last_ =
            std::max(
                known_last_,
                sequence);

        if (
            sequence <
            next_expected_) {
            return
                ReceiveResult::Duplicate;
        }

        // Do not let an arbitrarily far-ahead packet allocate or enlarge
        // receiver state. It can be recovered later as the window advances.
        if (
            sequence -
                next_expected_ >=
            window_size_) {
            return
                ReceiveResult::OutOfWindow;
        }

        seen_[
            static_cast<std::size_t>(
                sequence %
                window_size_)] =
            sequence;

        if (
            sequence !=
            next_expected_) {
            return
                ReceiveResult::
                    AcceptedOutOfOrder;
        }

        advance();
        return
            ReceiveResult::Advanced;
    }

    void on_heartbeat(
        const Heartbeat& heartbeat) noexcept {
        known_last_ =
            std::max(
                known_last_,
                heartbeat.last_published);
    }

    std::optional<Nack>
    make_nack() const noexcept {
        if (
            known_last_ <
            next_expected_) {
            return std::nullopt;
        }

        std::uint64_t bitmap = 0;

        for (unsigned bit = 0;
             bit < 64;
             ++bit) {
            if (
                next_expected_ >
                std::numeric_limits<
                    SequenceNumber>::max() -
                    bit) {
                break;
            }

            const auto sequence =
                next_expected_ + bit;

            if (
                sequence >
                known_last_) {
                break;
            }

            if (!has(sequence)) {
                bitmap |=
                    std::uint64_t{1}
                    << bit;
            }
        }

        if (bitmap == 0) {
            return std::nullopt;
        }

        return Nack{
            next_expected_,
            bitmap};
    }

    bool on_gap(
        const Gap& gap) {
        if (
            gap.first == 0 ||
            gap.last <
                gap.first) {
            throw std::invalid_argument(
                "Cito reliable GAP range is invalid");
        }

        if (
            gap.last <
                next_expected_ ||
            gap.first >
                next_expected_) {
            return false;
        }

        if (
            gap.last ==
            std::numeric_limits<
                SequenceNumber>::max()) {
            throw std::overflow_error(
                "Cito reliable GAP reaches sequence limit");
        }

        next_expected_ =
            gap.last + 1;

        advance();
        return true;
    }

    SequenceNumber next_expected()
        const noexcept {
        return next_expected_;
    }

    SequenceNumber known_last()
        const noexcept {
        return known_last_;
    }

private:
    bool has(
        SequenceNumber sequence)
        const noexcept {
        return
            seen_[
                static_cast<std::size_t>(
                    sequence %
                    window_size_)] ==
            sequence;
    }

    void advance() noexcept {
        while (
            next_expected_ !=
                std::numeric_limits<
                    SequenceNumber>::max() &&
            has(next_expected_)) {
            seen_[
                static_cast<std::size_t>(
                    next_expected_ %
                    window_size_)] = 0;
            ++next_expected_;
        }
    }

    const std::size_t window_size_;
    std::vector<SequenceNumber> seen_;
    SequenceNumber next_expected_{1};
    SequenceNumber known_last_{0};
};

} // namespace cito::detail::reliable
