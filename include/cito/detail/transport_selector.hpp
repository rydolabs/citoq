#pragma once
#include <cstdint>
namespace cito::detail {
enum class Locality : std::uint8_t { SameProcess, SameHost, Remote };
enum class DeliveryReliability : std::uint8_t { BestEffort, Reliable };
enum class TransportKind : std::uint8_t {
    InProcess, SharedMemory, Udp, QuicDatagram, QuicStream
};
constexpr TransportKind select_transport(Locality locality) noexcept {
    switch (locality) {
        case Locality::SameProcess: return TransportKind::InProcess;
        case Locality::SameHost: return TransportKind::SharedMemory;
        case Locality::Remote: return TransportKind::Udp;
    }
    return TransportKind::Udp;
}
constexpr TransportKind select_remote_transport(
    bool secure, DeliveryReliability reliability) noexcept {
    if (!secure) return TransportKind::Udp;
    return reliability == DeliveryReliability::Reliable
        ? TransportKind::QuicStream
        : TransportKind::QuicDatagram;
}
} // namespace cito::detail
