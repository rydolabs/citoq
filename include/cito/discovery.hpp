#pragma once

#include <cito/interest.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace cito {

enum class InterestOp : std::uint8_t {
    Add = 1,
    Remove = 2,
};

struct InterestAdvertisement {
    InterestOp op{InterestOp::Add};
    std::uint16_t data_port{};
    std::uint32_t lease_ms{};
    DestinationId destination{};
    InterestKey key{};
};

namespace discovery_detail {

inline void write_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
}

inline void write_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xffu));
    }
}

inline void write_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xffu));
    }
}

inline std::uint16_t read_u16(
    const std::vector<std::uint8_t>& bytes,
    std::size_t& pos) {
    if (pos + 2 > bytes.size()) {
        throw std::runtime_error("Cito discovery: truncated u16");
    }
    const auto value = static_cast<std::uint16_t>(
        bytes[pos] | (static_cast<std::uint16_t>(bytes[pos + 1]) << 8));
    pos += 2;
    return value;
}

inline std::uint32_t read_u32(
    const std::vector<std::uint8_t>& bytes,
    std::size_t& pos) {
    if (pos + 4 > bytes.size()) {
        throw std::runtime_error("Cito discovery: truncated u32");
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(bytes[pos++]) << (i * 8);
    }
    return value;
}

inline std::uint64_t read_u64(
    const std::vector<std::uint8_t>& bytes,
    std::size_t& pos) {
    if (pos + 8 > bytes.size()) {
        throw std::runtime_error("Cito discovery: truncated u64");
    }
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(bytes[pos++]) << (i * 8);
    }
    return value;
}

} // namespace discovery_detail

inline std::vector<std::uint8_t> encode_interest(
    const InterestAdvertisement& advertisement) {
    if (advertisement.op == InterestOp::Add &&
        advertisement.lease_ms == 0) {
        throw std::invalid_argument(
            "Cito discovery: ADD lease must be non-zero");
    }

    std::vector<std::uint8_t> out{
        'C', 'T', 'I', '1',
        static_cast<std::uint8_t>(advertisement.op),
        0};

    discovery_detail::write_u16(out, advertisement.data_port);
    discovery_detail::write_u32(out, advertisement.lease_ms);
    discovery_detail::write_u64(out, advertisement.destination);
    discovery_detail::write_u64(out, advertisement.key.scope);
    discovery_detail::write_u64(out, advertisement.key.resource);
    discovery_detail::write_u64(out, advertisement.key.type);
    return out;
}

inline InterestAdvertisement decode_interest(
    const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != 44 ||
        bytes[0] != 'C' ||
        bytes[1] != 'T' ||
        bytes[2] != 'I' ||
        bytes[3] != '1') {
        throw std::runtime_error("Cito discovery: invalid packet");
    }

    if (bytes[4] != static_cast<std::uint8_t>(InterestOp::Add) &&
        bytes[4] != static_cast<std::uint8_t>(InterestOp::Remove)) {
        throw std::runtime_error("Cito discovery: invalid op");
    }

    std::size_t pos = 6;
    InterestAdvertisement advertisement;
    advertisement.op = static_cast<InterestOp>(bytes[4]);
    advertisement.data_port = discovery_detail::read_u16(bytes, pos);
    advertisement.lease_ms = discovery_detail::read_u32(bytes, pos);
    advertisement.destination = discovery_detail::read_u64(bytes, pos);
    advertisement.key.scope = discovery_detail::read_u64(bytes, pos);
    advertisement.key.resource = discovery_detail::read_u64(bytes, pos);
    advertisement.key.type = discovery_detail::read_u64(bytes, pos);

    if (advertisement.op == InterestOp::Add &&
        advertisement.lease_ms == 0) {
        throw std::runtime_error("Cito discovery: zero ADD lease");
    }

    return advertisement;
}

struct LeaseKey {
    InterestKey interest{};
    DestinationId destination{};

    friend bool operator==(const LeaseKey&, const LeaseKey&) = default;
};

struct LeaseKeyHash {
    std::size_t operator()(const LeaseKey& key) const noexcept {
        return InterestKeyHash{}(key.interest) ^
            (static_cast<std::size_t>(key.destination) + 0x9e3779b9u);
    }
};

class InterestLeaseTable {
private:
    using ExpiryMap = std::multimap<std::uint64_t, LeaseKey>;

    struct LeaseEntry {
        std::uint64_t expires_at{};
        ExpiryMap::iterator expiry_it;
    };

public:
    explicit InterestLeaseTable(InterestIndex& index) : index_(index) {}

    bool observe(
        const InterestAdvertisement& advertisement,
        std::uint64_t now_ms) {
        const LeaseKey lease_key{
            advertisement.key,
            advertisement.destination};

        auto existing = leases_.find(lease_key);

        if (advertisement.op == InterestOp::Remove) {
            if (existing == leases_.end()) {
                index_.remove(
                    advertisement.key,
                    advertisement.destination);
                return false;
            }

            expiries_.erase(existing->second.expiry_it);
            leases_.erase(existing);
            index_.remove(
                advertisement.key,
                advertisement.destination);
            return true;
        }

        const auto expires_at = now_ms + advertisement.lease_ms;

        if (existing != leases_.end()) {
            expiries_.erase(existing->second.expiry_it);
            const auto expiry_it =
                expiries_.emplace(expires_at, lease_key);
            existing->second = LeaseEntry{expires_at, expiry_it};
            index_.add(
                advertisement.key,
                advertisement.destination);
            return false;
        }

        const auto expiry_it =
            expiries_.emplace(expires_at, lease_key);
        leases_.emplace(
            lease_key,
            LeaseEntry{expires_at, expiry_it});
        index_.add(
            advertisement.key,
            advertisement.destination);
        return true;
    }

    std::size_t expire(std::uint64_t now_ms) {
        std::size_t expired = 0;

        while (!expiries_.empty() &&
               expiries_.begin()->first <= now_ms) {
            auto expiry_it = expiries_.begin();
            const auto lease_key = expiry_it->second;
            auto lease_it = leases_.find(lease_key);

            if (lease_it != leases_.end() &&
                lease_it->second.expiry_it == expiry_it) {
                index_.remove(
                    lease_key.interest,
                    lease_key.destination);
                leases_.erase(lease_it);
                ++expired;
            }

            expiries_.erase(expiry_it);
        }

        return expired;
    }

    std::size_t size() const noexcept {
        return leases_.size();
    }

    std::size_t scheduled_expiry_count() const noexcept {
        return expiries_.size();
    }

private:
    InterestIndex& index_;
    std::unordered_map<
        LeaseKey,
        LeaseEntry,
        LeaseKeyHash> leases_;
    ExpiryMap expiries_;
};

} // namespace cito
