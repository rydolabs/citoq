#pragma once

#include <cito/detail/transport_selector.hpp>
#include <cito/interest.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace cito::detail {

struct DeliveryTarget {
    DestinationId destination{};
    Locality locality{Locality::Remote};
};

struct DeliveryResult {
    std::size_t in_process{};
    std::size_t shared_memory{};
    std::size_t udp{};

    std::size_t delivered() const noexcept {
        return
            in_process +
            shared_memory +
            udp;
    }
};

template <
    class LocalSend,
    class Encode,
    class ShmSend,
    class UdpSend>
DeliveryResult deliver_best_effort(
    std::span<const DeliveryTarget> targets,
    LocalSend&& local_send,
    Encode&& encode,
    ShmSend&& shm_send,
    UdpSend&& udp_send) {
    using Payload =
        std::decay_t<
            std::invoke_result_t<Encode>>;

    static_assert(
        !std::is_void_v<Payload>,
        "Cito BestEffort encoder must return a payload");

    std::optional<Payload> encoded;
    DeliveryResult result;

    // Encoding is lazy: same-process delivery can stay object-native and
    // an empty/no-remote publish pays no serialization cost.
    auto payload =
        [&]() -> const Payload& {
            if (!encoded) {
                encoded.emplace(
                    std::invoke(
                        encode));
            }
            return *encoded;
        };

    for (const auto& target :
         targets) {
        switch (
            select_transport(
                target.locality)) {
            case TransportKind::InProcess:
                std::invoke(
                    local_send,
                    target.destination);
                ++result.in_process;
                break;

            case TransportKind::SharedMemory:
                std::invoke(
                    shm_send,
                    target.destination,
                    payload());
                ++result.shared_memory;
                break;

            case TransportKind::Udp:
                std::invoke(
                    udp_send,
                    target.destination,
                    payload());
                ++result.udp;
                break;

            case TransportKind::QuicDatagram:
            case TransportKind::QuicStream:
                throw std::logic_error(
                    "Cito BestEffort locality path received a secure remote transport");
        }
    }

    return result;
}

} // namespace cito::detail
