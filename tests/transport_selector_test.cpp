#include <cito/detail/transport_selector.hpp>
#include <cassert>
int main() {
    using cito::detail::DeliveryReliability;
    using cito::detail::Locality;
    using cito::detail::TransportKind;
    static_assert(cito::detail::select_transport(Locality::SameProcess) == TransportKind::InProcess);
    static_assert(cito::detail::select_transport(Locality::SameHost) == TransportKind::SharedMemory);
    static_assert(cito::detail::select_transport(Locality::Remote) == TransportKind::Udp);
    static_assert(cito::detail::select_remote_transport(false, DeliveryReliability::BestEffort) == TransportKind::Udp);
    static_assert(cito::detail::select_remote_transport(false, DeliveryReliability::Reliable) == TransportKind::Udp);
    static_assert(cito::detail::select_remote_transport(true, DeliveryReliability::BestEffort) == TransportKind::QuicDatagram);
    static_assert(cito::detail::select_remote_transport(true, DeliveryReliability::Reliable) == TransportKind::QuicStream);
    assert(cito::detail::select_remote_transport(true, DeliveryReliability::Reliable) == TransportKind::QuicStream);
    return 0;
}
