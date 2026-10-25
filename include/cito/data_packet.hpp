#pragma once

#include <cito/interest.hpp>

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace cito {

struct DataPacket {
    InterestKey key{};
    std::vector<std::uint8_t> payload;
};

struct DataPacketView {
    InterestKey key{};
    std::span<const std::uint8_t> payload;
};

namespace data_detail {

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

inline std::uint32_t read_u32(
    std::span<const std::uint8_t> bytes,
    std::size_t& pos) {
    if (pos + 4 > bytes.size()) {
        throw std::runtime_error("Cito data: truncated u32");
    }

    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(bytes[pos++]) << (i * 8);
    }
    return value;
}

inline std::uint64_t read_u64(
    std::span<const std::uint8_t> bytes,
    std::size_t& pos) {
    if (pos + 8 > bytes.size()) {
        throw std::runtime_error("Cito data: truncated u64");
    }

    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(bytes[pos++]) << (i * 8);
    }
    return value;
}

} // namespace data_detail

inline std::vector<std::uint8_t> encode_data_packet(
    const InterestKey& key,
    std::span<const std::uint8_t> payload) {
    if (payload.size() > 0xffffffffu) {
        throw std::length_error("Cito data: payload too large");
    }

    std::vector<std::uint8_t> out;
    out.reserve(32 + payload.size());
    out.insert(out.end(), {'C', 'T', 'D', '1'});

    data_detail::write_u64(out, key.scope);
    data_detail::write_u64(out, key.resource);
    data_detail::write_u64(out, key.type);
    data_detail::write_u32(
        out,
        static_cast<std::uint32_t>(payload.size()));
    out.insert(
        out.end(),
        payload.begin(),
        payload.end());
    return out;
}

inline std::vector<std::uint8_t> encode_data_packet(
    const DataPacket& packet) {
    return encode_data_packet(
        packet.key,
        std::span<const std::uint8_t>(
            packet.payload.data(),
            packet.payload.size()));
}

inline DataPacketView decode_data_packet_view(
    std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 32 ||
        bytes[0] != 'C' ||
        bytes[1] != 'T' ||
        bytes[2] != 'D' ||
        bytes[3] != '1') {
        throw std::runtime_error("Cito data: invalid packet");
    }

    std::size_t pos = 4;
    DataPacketView packet;
    packet.key.scope = data_detail::read_u64(bytes, pos);
    packet.key.resource = data_detail::read_u64(bytes, pos);
    packet.key.type = data_detail::read_u64(bytes, pos);
    const auto size = data_detail::read_u32(bytes, pos);

    if (pos + size != bytes.size()) {
        throw std::runtime_error("Cito data: payload length mismatch");
    }

    packet.payload = bytes.subspan(pos, size);
    return packet;
}

inline DataPacket decode_data_packet(
    const std::vector<std::uint8_t>& bytes) {
    const auto view = decode_data_packet_view(
        std::span<const std::uint8_t>(bytes.data(), bytes.size()));

    return DataPacket{
        view.key,
        std::vector<std::uint8_t>(
            view.payload.begin(),
            view.payload.end())};
}

} // namespace cito
