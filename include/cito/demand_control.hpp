#pragma once

#include <cito/interest.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace cito {

inline constexpr std::uint16_t kMaxDemandSnapshotBatch = 32;
inline constexpr std::uint16_t kMaxRouteBatch = 64;

struct DemandSummaryAnnouncement {
    CoordinatorId coordinator{};
    std::uint16_t control_port{};
    std::uint32_t lease_ms{};
    DemandSummaryStamp stamp{};
};

struct DemandSnapshotRequest {
    CoordinatorId coordinator{};
    std::uint64_t incarnation{};
    std::uint64_t version{};
    std::uint32_t offset{};
    std::uint16_t limit{kMaxDemandSnapshotBatch};
};

struct DemandSnapshotBatch {
    CoordinatorId coordinator{};
    DemandSummaryStamp stamp{};
    std::uint32_t offset{};
    std::vector<InterestKey> keys;
};

struct RouteRequest {
    CoordinatorId coordinator{};
    std::uint64_t incarnation{};
    InterestKey key{};
};

struct RouteEndpoint {
    DestinationId destination{};
    std::uint16_t data_port{};
    std::uint16_t quic_port{};
};

struct RouteBatch {
    CoordinatorId coordinator{};
    std::uint64_t incarnation{};
    InterestKey key{};
    std::vector<RouteEndpoint> endpoints;
};

namespace demand_control_detail {

using Bytes = std::span<const std::uint8_t>;

inline void write_u16(
    std::vector<std::uint8_t>& out,
    std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffu));
    out.push_back(
        static_cast<std::uint8_t>((value >> 8) & 0xffu));
}

inline void write_u32(
    std::vector<std::uint8_t>& out,
    std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(
            static_cast<std::uint8_t>(
                (value >> (i * 8)) & 0xffu));
    }
}

inline void write_u64(
    std::vector<std::uint8_t>& out,
    std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(
            static_cast<std::uint8_t>(
                (value >> (i * 8)) & 0xffu));
    }
}

inline void require(
    Bytes bytes,
    std::size_t pos,
    std::size_t size,
    const char* message) {
    if (pos > bytes.size() ||
        size > bytes.size() - pos) {
        throw std::runtime_error(message);
    }
}

inline std::uint16_t read_u16(
    Bytes bytes,
    std::size_t& pos) {
    require(
        bytes,
        pos,
        2,
        "Cito demand control: truncated u16");

    const auto value =
        static_cast<std::uint16_t>(
            bytes[pos] |
            (static_cast<std::uint16_t>(
                 bytes[pos + 1]) << 8));
    pos += 2;
    return value;
}

inline std::uint32_t read_u32(
    Bytes bytes,
    std::size_t& pos) {
    require(
        bytes,
        pos,
        4,
        "Cito demand control: truncated u32");

    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |=
            static_cast<std::uint32_t>(
                bytes[pos++]) << (i * 8);
    }
    return value;
}

inline std::uint64_t read_u64(
    Bytes bytes,
    std::size_t& pos) {
    require(
        bytes,
        pos,
        8,
        "Cito demand control: truncated u64");

    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |=
            static_cast<std::uint64_t>(
                bytes[pos++]) << (i * 8);
    }
    return value;
}

inline void write_key(
    std::vector<std::uint8_t>& out,
    const InterestKey& key) {
    write_u64(out, key.scope);
    write_u64(out, key.resource);
    write_u64(out, key.type);
}

inline InterestKey read_key(
    Bytes bytes,
    std::size_t& pos) {
    return {
        read_u64(bytes, pos),
        read_u64(bytes, pos),
        read_u64(bytes, pos)};
}

inline void write_magic(
    std::vector<std::uint8_t>& out,
    char family,
    char version) {
    out.insert(
        out.end(),
        {
            'C',
            'T',
            static_cast<std::uint8_t>(family),
            static_cast<std::uint8_t>(version)});
}

inline void check_magic(
    Bytes bytes,
    char family,
    char version,
    std::size_t minimum_size) {
    if (bytes.size() < minimum_size ||
        bytes[0] != 'C' ||
        bytes[1] != 'T' ||
        bytes[2] !=
            static_cast<std::uint8_t>(family) ||
        bytes[3] !=
            static_cast<std::uint8_t>(version)) {
        throw std::runtime_error(
            "Cito demand control: invalid packet");
    }
}

} // namespace demand_control_detail

inline std::vector<std::uint8_t>
encode_summary_announcement(
    const DemandSummaryAnnouncement& message) {
    if (message.lease_ms == 0) {
        throw std::invalid_argument(
            "Cito demand control: zero summary lease");
    }

    std::vector<std::uint8_t> out;
    out.reserve(40);

    demand_control_detail::write_magic(
        out,
        'S',
        '2');
    demand_control_detail::write_u16(
        out,
        message.control_port);
    demand_control_detail::write_u16(out, 0);
    demand_control_detail::write_u64(
        out,
        message.coordinator);
    demand_control_detail::write_u64(
        out,
        message.stamp.incarnation);
    demand_control_detail::write_u64(
        out,
        message.stamp.version);
    demand_control_detail::write_u32(
        out,
        message.stamp.key_count);
    demand_control_detail::write_u32(
        out,
        message.lease_ms);
    return out;
}

inline DemandSummaryAnnouncement
decode_summary_announcement(
    std::span<const std::uint8_t> bytes) {
    demand_control_detail::check_magic(
        bytes,
        'S',
        '2',
        40);

    if (bytes.size() != 40) {
        throw std::runtime_error(
            "Cito demand control: invalid announcement length");
    }

    std::size_t pos = 4;
    DemandSummaryAnnouncement message;
    message.control_port =
        demand_control_detail::read_u16(
            bytes,
            pos);
    (void)demand_control_detail::read_u16(
        bytes,
        pos);
    message.coordinator =
        demand_control_detail::read_u64(
            bytes,
            pos);
    message.stamp.incarnation =
        demand_control_detail::read_u64(
            bytes,
            pos);
    message.stamp.version =
        demand_control_detail::read_u64(
            bytes,
            pos);
    message.stamp.key_count =
        demand_control_detail::read_u32(
            bytes,
            pos);
    message.lease_ms =
        demand_control_detail::read_u32(
            bytes,
            pos);

    if (message.lease_ms == 0) {
        throw std::runtime_error(
            "Cito demand control: zero summary lease");
    }

    return message;
}

inline std::vector<std::uint8_t>
encode_snapshot_request(
    const DemandSnapshotRequest& message) {
    if (message.limit == 0 ||
        message.limit > kMaxDemandSnapshotBatch) {
        throw std::invalid_argument(
            "Cito demand control: invalid snapshot request limit");
    }

    std::vector<std::uint8_t> out;
    out.reserve(36);

    demand_control_detail::write_magic(
        out,
        'Q',
        '1');
    demand_control_detail::write_u64(
        out,
        message.coordinator);
    demand_control_detail::write_u64(
        out,
        message.incarnation);
    demand_control_detail::write_u64(
        out,
        message.version);
    demand_control_detail::write_u32(
        out,
        message.offset);
    demand_control_detail::write_u16(
        out,
        message.limit);
    demand_control_detail::write_u16(
        out,
        0);
    return out;
}

inline DemandSnapshotRequest
decode_snapshot_request(
    std::span<const std::uint8_t> bytes) {
    demand_control_detail::check_magic(
        bytes,
        'Q',
        '1',
        36);

    if (bytes.size() != 36) {
        throw std::runtime_error(
            "Cito demand control: invalid request length");
    }

    std::size_t pos = 4;
    DemandSnapshotRequest message;
    message.coordinator =
        demand_control_detail::read_u64(
            bytes,
            pos);
    message.incarnation =
        demand_control_detail::read_u64(
            bytes,
            pos);
    message.version =
        demand_control_detail::read_u64(
            bytes,
            pos);
    message.offset =
        demand_control_detail::read_u32(
            bytes,
            pos);
    message.limit =
        demand_control_detail::read_u16(
            bytes,
            pos);
    (void)demand_control_detail::read_u16(
        bytes,
        pos);

    if (message.limit == 0 ||
        message.limit > kMaxDemandSnapshotBatch) {
        throw std::runtime_error(
            "Cito demand control: invalid request limit");
    }

    return message;
}

inline DemandSnapshotBatch make_snapshot_batch(
    CoordinatorId coordinator,
    DemandSummaryStamp stamp,
    std::span<const InterestKey> snapshot,
    std::uint32_t offset,
    std::uint16_t limit =
        kMaxDemandSnapshotBatch) {
    if (limit == 0 ||
        limit > kMaxDemandSnapshotBatch) {
        throw std::invalid_argument(
            "Cito demand control: invalid batch limit");
    }

    if (snapshot.size() != stamp.key_count) {
        throw std::invalid_argument(
            "Cito demand control: snapshot/stamp count mismatch");
    }

    if (offset > snapshot.size()) {
        throw std::out_of_range(
            "Cito demand control: snapshot offset out of range");
    }

    const auto count =
        std::min<std::size_t>(
            limit,
            snapshot.size() - offset);

    DemandSnapshotBatch batch;
    batch.coordinator = coordinator;
    batch.stamp = stamp;
    batch.offset = offset;
    batch.keys.assign(
        snapshot.begin() +
            static_cast<std::ptrdiff_t>(offset),
        snapshot.begin() +
            static_cast<std::ptrdiff_t>(
                offset + count));
    return batch;
}

inline std::vector<std::uint8_t>
encode_snapshot_batch(
    const DemandSnapshotBatch& message) {
    if (message.keys.size() >
        kMaxDemandSnapshotBatch) {
        throw std::length_error(
            "Cito demand control: snapshot batch too large");
    }

    if (static_cast<std::uint64_t>(
            message.offset) +
            message.keys.size() >
        message.stamp.key_count ||
        (message.keys.empty() &&
         message.offset <
             message.stamp.key_count)) {
        throw std::invalid_argument(
            "Cito demand control: invalid snapshot batch range");
    }

    std::vector<std::uint8_t> out;
    out.reserve(
        40 +
        message.keys.size() * 24);

    demand_control_detail::write_magic(
        out,
        'B',
        '1');
    demand_control_detail::write_u64(
        out,
        message.coordinator);
    demand_control_detail::write_u64(
        out,
        message.stamp.incarnation);
    demand_control_detail::write_u64(
        out,
        message.stamp.version);
    demand_control_detail::write_u32(
        out,
        message.stamp.key_count);
    demand_control_detail::write_u32(
        out,
        message.offset);
    demand_control_detail::write_u16(
        out,
        static_cast<std::uint16_t>(
            message.keys.size()));
    demand_control_detail::write_u16(
        out,
        0);

    for (const auto& key : message.keys) {
        demand_control_detail::write_key(
            out,
            key);
    }

    return out;
}

inline DemandSnapshotBatch
decode_snapshot_batch(
    std::span<const std::uint8_t> bytes) {
    demand_control_detail::check_magic(
        bytes,
        'B',
        '1',
        40);

    std::size_t pos = 4;
    DemandSnapshotBatch message;
    message.coordinator =
        demand_control_detail::read_u64(
            bytes,
            pos);
    message.stamp.incarnation =
        demand_control_detail::read_u64(
            bytes,
            pos);
    message.stamp.version =
        demand_control_detail::read_u64(
            bytes,
            pos);
    message.stamp.key_count =
        demand_control_detail::read_u32(
            bytes,
            pos);
    message.offset =
        demand_control_detail::read_u32(
            bytes,
            pos);

    const auto count =
        demand_control_detail::read_u16(
            bytes,
            pos);
    (void)demand_control_detail::read_u16(
        bytes,
        pos);

    if (count > kMaxDemandSnapshotBatch ||
        static_cast<std::uint64_t>(
            message.offset) +
            count >
        message.stamp.key_count ||
        (count == 0 &&
         message.offset <
             message.stamp.key_count)) {
        throw std::runtime_error(
            "Cito demand control: invalid snapshot batch range");
    }

    if (bytes.size() !=
        40 +
            static_cast<std::size_t>(count) *
                24) {
        throw std::runtime_error(
            "Cito demand control: invalid snapshot batch length");
    }

    message.keys.reserve(count);
    for (std::uint16_t i = 0;
         i < count;
         ++i) {
        message.keys.push_back(
            demand_control_detail::read_key(
                bytes,
                pos));
    }

    return message;
}

inline std::vector<std::uint8_t>
encode_route_request(
    const RouteRequest& message) {
    std::vector<std::uint8_t> out;
    out.reserve(44);

    demand_control_detail::write_magic(
        out,
        'R',
        '3');
    demand_control_detail::write_u64(
        out,
        message.coordinator);
    demand_control_detail::write_u64(
        out,
        message.incarnation);
    demand_control_detail::write_key(
        out,
        message.key);
    return out;
}

inline RouteRequest decode_route_request(
    std::span<const std::uint8_t> bytes) {
    demand_control_detail::check_magic(
        bytes,
        'R',
        '3',
        44);

    if (bytes.size() != 44) {
        throw std::runtime_error(
            "Cito demand control: invalid route request length");
    }

    std::size_t pos = 4;
    RouteRequest message;
    message.coordinator =
        demand_control_detail::read_u64(
            bytes,
            pos);
    message.incarnation =
        demand_control_detail::read_u64(
            bytes,
            pos);
    message.key =
        demand_control_detail::read_key(
            bytes,
            pos);
    return message;
}

inline std::vector<std::uint8_t>
encode_route_batch(
    const RouteBatch& message) {
    if (message.endpoints.size() >
        kMaxRouteBatch) {
        throw std::length_error(
            "Cito demand control: route batch too large");
    }

    for (const auto& endpoint :
         message.endpoints) {
        if (
            endpoint.destination == 0 ||
            (
                endpoint.data_port == 0 &&
                endpoint.quic_port == 0
            )) {
            throw std::invalid_argument(
                "Cito demand control: route endpoint has no transport");
        }
    }

    // CTR5 keeps the same 12-byte endpoint footprint as CTR4. The u16 that
    // was reserved in CTR4 now carries the QUIC port.
    std::vector<std::uint8_t> out;
    out.reserve(
        48 +
        message.endpoints.size() * 12);

    demand_control_detail::write_magic(
        out,
        'R',
        '5');
    demand_control_detail::write_u64(
        out,
        message.coordinator);
    demand_control_detail::write_u64(
        out,
        message.incarnation);
    demand_control_detail::write_key(
        out,
        message.key);
    demand_control_detail::write_u16(
        out,
        static_cast<std::uint16_t>(
            message.endpoints.size()));
    demand_control_detail::write_u16(
        out,
        0);

    for (const auto& endpoint :
         message.endpoints) {
        demand_control_detail::write_u64(
            out,
            endpoint.destination);
        demand_control_detail::write_u16(
            out,
            endpoint.data_port);
        demand_control_detail::write_u16(
            out,
            endpoint.quic_port);
    }

    return out;
}

inline RouteBatch decode_route_batch(
    std::span<const std::uint8_t> bytes) {
    if (
        bytes.size() < 48 ||
        bytes[0] != 'C' ||
        bytes[1] != 'T' ||
        bytes[2] != 'R' ||
        (
            bytes[3] != '4' &&
            bytes[3] != '5'
        )) {
        throw std::runtime_error(
            "Cito demand control: invalid route batch");
    }

    const bool legacy_v4 =
        bytes[3] == '4';

    std::size_t pos = 4;
    RouteBatch message;
    message.coordinator =
        demand_control_detail::read_u64(
            bytes,
            pos);
    message.incarnation =
        demand_control_detail::read_u64(
            bytes,
            pos);
    message.key =
        demand_control_detail::read_key(
            bytes,
            pos);

    const auto count =
        demand_control_detail::read_u16(
            bytes,
            pos);
    const auto header_reserved =
        demand_control_detail::read_u16(
            bytes,
            pos);

    if (
        header_reserved != 0 ||
        count > kMaxRouteBatch ||
        bytes.size() !=
            48 +
                static_cast<std::size_t>(
                    count) *
                    12) {
        throw std::runtime_error(
            "Cito demand control: invalid route batch");
    }

    message.endpoints.reserve(count);

    for (std::uint16_t i = 0;
         i < count;
         ++i) {
        RouteEndpoint endpoint;
        endpoint.destination =
            demand_control_detail::read_u64(
                bytes,
                pos);
        endpoint.data_port =
            demand_control_detail::read_u16(
                bytes,
                pos);

        const auto transport_port =
            demand_control_detail::read_u16(
                bytes,
                pos);

        if (legacy_v4) {
            if (transport_port != 0) {
                throw std::runtime_error(
                    "Cito demand control: invalid CTR4 reserved field");
            }
            endpoint.quic_port = 0;
        } else {
            endpoint.quic_port =
                transport_port;
        }

        if (
            endpoint.destination == 0 ||
            (
                endpoint.data_port == 0 &&
                endpoint.quic_port == 0
            )) {
            throw std::runtime_error(
                "Cito demand control: route endpoint has no transport");
        }

        message.endpoints.push_back(
            endpoint);
    }

    return message;
}

} // namespace cito
