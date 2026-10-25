#pragma once

#include <cito/detail/reliability.hpp>
#include <cito/interest.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace cito::detail::reliable {

using StreamId = std::uint64_t;

struct ReliableDataView {
    InterestKey key{};
    StreamId stream{};
    SequenceNumber sequence{};
    std::span<const std::uint8_t> payload;
};

struct HeartbeatFrame {
    InterestKey key{};
    StreamId stream{};
    Heartbeat heartbeat{};
};

struct NackFrame {
    InterestKey key{};
    StreamId stream{};
    Nack nack{};
};

struct GapFrame {
    InterestKey key{};
    StreamId stream{};
    Gap gap{};
};

namespace packet_detail {

using Bytes =
    std::span<const std::uint8_t>;

inline void write_u32(
    std::vector<std::uint8_t>& out,
    std::uint32_t value) {
    for (int i = 0;
         i < 4;
         ++i) {
        out.push_back(
            static_cast<std::uint8_t>(
                (value >>
                 (i * 8)) &
                0xffu));
    }
}

inline void write_u64(
    std::vector<std::uint8_t>& out,
    std::uint64_t value) {
    for (int i = 0;
         i < 8;
         ++i) {
        out.push_back(
            static_cast<std::uint8_t>(
                (value >>
                 (i * 8)) &
                0xffu));
    }
}

inline void require(
    Bytes bytes,
    std::size_t pos,
    std::size_t size,
    const char* message) {
    if (
        pos > bytes.size() ||
        size >
            bytes.size() -
                pos) {
        throw std::runtime_error(
            message);
    }
}

inline std::uint32_t read_u32(
    Bytes bytes,
    std::size_t& pos) {
    require(
        bytes,
        pos,
        4,
        "Cito reliable packet: truncated u32");

    std::uint32_t value = 0;

    for (int i = 0;
         i < 4;
         ++i) {
        value |=
            static_cast<
                std::uint32_t>(
                    bytes[pos++])
            << (i * 8);
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
        "Cito reliable packet: truncated u64");

    std::uint64_t value = 0;

    for (int i = 0;
         i < 8;
         ++i) {
        value |=
            static_cast<
                std::uint64_t>(
                    bytes[pos++])
            << (i * 8);
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
    char family) {
    out.insert(
        out.end(),
        {
            'C',
            'R',
            static_cast<std::uint8_t>(
                family),
            '1'});
}

inline void check_magic(
    Bytes bytes,
    char family,
    std::size_t size) {
    if (
        bytes.size() != size ||
        bytes[0] != 'C' ||
        bytes[1] != 'R' ||
        bytes[2] !=
            static_cast<
                std::uint8_t>(
                    family) ||
        bytes[3] != '1') {
        throw std::runtime_error(
            "Cito reliable packet: invalid frame");
    }
}

inline void validate_stream(
    StreamId stream) {
    if (stream == 0) {
        throw std::invalid_argument(
            "Cito reliable StreamId 0 is reserved");
    }
}

} // namespace packet_detail

inline std::vector<std::uint8_t>
encode_reliable_data(
    const ReliableDataView& frame) {
    packet_detail::validate_stream(
        frame.stream);

    if (frame.sequence == 0) {
        throw std::invalid_argument(
            "Cito reliable DATA sequence 0 is invalid");
    }

    if (
        frame.payload.size() >
        std::numeric_limits<
            std::uint32_t>::max()) {
        throw std::length_error(
            "Cito reliable DATA payload too large");
    }

    std::vector<std::uint8_t> out;
    out.reserve(
        48 +
        frame.payload.size());

    packet_detail::write_magic(
        out,
        'D');
    packet_detail::write_key(
        out,
        frame.key);
    packet_detail::write_u64(
        out,
        frame.stream);
    packet_detail::write_u64(
        out,
        frame.sequence);
    packet_detail::write_u32(
        out,
        static_cast<std::uint32_t>(
            frame.payload.size()));

    out.insert(
        out.end(),
        frame.payload.begin(),
        frame.payload.end());

    return out;
}

inline ReliableDataView
decode_reliable_data_view(
    std::span<const std::uint8_t> bytes) {
    if (
        bytes.size() < 48 ||
        bytes[0] != 'C' ||
        bytes[1] != 'R' ||
        bytes[2] != 'D' ||
        bytes[3] != '1') {
        throw std::runtime_error(
            "Cito reliable packet: invalid DATA frame");
    }

    std::size_t pos = 4;
    ReliableDataView frame;
    frame.key =
        packet_detail::read_key(
            bytes,
            pos);
    frame.stream =
        packet_detail::read_u64(
            bytes,
            pos);
    frame.sequence =
        packet_detail::read_u64(
            bytes,
            pos);

    const auto payload_size =
        packet_detail::read_u32(
            bytes,
            pos);

    packet_detail::validate_stream(
        frame.stream);

    if (frame.sequence == 0) {
        throw std::runtime_error(
            "Cito reliable packet: DATA sequence 0");
    }

    packet_detail::require(
        bytes,
        pos,
        payload_size,
        "Cito reliable packet: truncated DATA payload");

    if (
        pos + payload_size !=
        bytes.size()) {
        throw std::runtime_error(
            "Cito reliable packet: DATA length mismatch");
    }

    frame.payload =
        bytes.subspan(
            pos,
            payload_size);

    return frame;
}

inline std::vector<std::uint8_t>
encode_heartbeat_frame(
    const HeartbeatFrame& frame) {
    packet_detail::validate_stream(
        frame.stream);

    if (
        frame.heartbeat.first_available ==
        0) {
        throw std::invalid_argument(
            "Cito reliable HEARTBEAT first sequence 0");
    }

    std::vector<std::uint8_t> out;
    out.reserve(52);

    packet_detail::write_magic(
        out,
        'H');
    packet_detail::write_key(
        out,
        frame.key);
    packet_detail::write_u64(
        out,
        frame.stream);
    packet_detail::write_u64(
        out,
        frame.heartbeat.first_available);
    packet_detail::write_u64(
        out,
        frame.heartbeat.last_published);

    return out;
}

inline HeartbeatFrame
decode_heartbeat_frame(
    std::span<const std::uint8_t> bytes) {
    packet_detail::check_magic(
        bytes,
        'H',
        52);

    std::size_t pos = 4;
    HeartbeatFrame frame;
    frame.key =
        packet_detail::read_key(
            bytes,
            pos);
    frame.stream =
        packet_detail::read_u64(
            bytes,
            pos);
    frame.heartbeat.first_available =
        packet_detail::read_u64(
            bytes,
            pos);
    frame.heartbeat.last_published =
        packet_detail::read_u64(
            bytes,
            pos);

    packet_detail::validate_stream(
        frame.stream);

    if (
        frame.heartbeat.first_available ==
        0) {
        throw std::runtime_error(
            "Cito reliable packet: invalid HEARTBEAT");
    }

    return frame;
}

inline std::vector<std::uint8_t>
encode_nack_frame(
    const NackFrame& frame) {
    packet_detail::validate_stream(
        frame.stream);

    if (
        frame.nack.base == 0 ||
        frame.nack.bitmap == 0) {
        throw std::invalid_argument(
            "Cito reliable NACK must request at least one sequence");
    }

    std::vector<std::uint8_t> out;
    out.reserve(52);

    packet_detail::write_magic(
        out,
        'N');
    packet_detail::write_key(
        out,
        frame.key);
    packet_detail::write_u64(
        out,
        frame.stream);
    packet_detail::write_u64(
        out,
        frame.nack.base);
    packet_detail::write_u64(
        out,
        frame.nack.bitmap);

    return out;
}

inline NackFrame decode_nack_frame(
    std::span<const std::uint8_t> bytes) {
    packet_detail::check_magic(
        bytes,
        'N',
        52);

    std::size_t pos = 4;
    NackFrame frame;
    frame.key =
        packet_detail::read_key(
            bytes,
            pos);
    frame.stream =
        packet_detail::read_u64(
            bytes,
            pos);
    frame.nack.base =
        packet_detail::read_u64(
            bytes,
            pos);
    frame.nack.bitmap =
        packet_detail::read_u64(
            bytes,
            pos);

    packet_detail::validate_stream(
        frame.stream);

    if (
        frame.nack.base == 0 ||
        frame.nack.bitmap == 0) {
        throw std::runtime_error(
            "Cito reliable packet: invalid NACK");
    }

    return frame;
}

inline std::vector<std::uint8_t>
encode_gap_frame(
    const GapFrame& frame) {
    packet_detail::validate_stream(
        frame.stream);

    if (
        frame.gap.first == 0 ||
        frame.gap.last <
            frame.gap.first) {
        throw std::invalid_argument(
            "Cito reliable GAP range is invalid");
    }

    std::vector<std::uint8_t> out;
    out.reserve(52);

    packet_detail::write_magic(
        out,
        'G');
    packet_detail::write_key(
        out,
        frame.key);
    packet_detail::write_u64(
        out,
        frame.stream);
    packet_detail::write_u64(
        out,
        frame.gap.first);
    packet_detail::write_u64(
        out,
        frame.gap.last);

    return out;
}

inline GapFrame decode_gap_frame(
    std::span<const std::uint8_t> bytes) {
    packet_detail::check_magic(
        bytes,
        'G',
        52);

    std::size_t pos = 4;
    GapFrame frame;
    frame.key =
        packet_detail::read_key(
            bytes,
            pos);
    frame.stream =
        packet_detail::read_u64(
            bytes,
            pos);
    frame.gap.first =
        packet_detail::read_u64(
            bytes,
            pos);
    frame.gap.last =
        packet_detail::read_u64(
            bytes,
            pos);

    packet_detail::validate_stream(
        frame.stream);

    if (
        frame.gap.first == 0 ||
        frame.gap.last <
            frame.gap.first) {
        throw std::runtime_error(
            "Cito reliable packet: invalid GAP");
    }

    return frame;
}

} // namespace cito::detail::reliable
