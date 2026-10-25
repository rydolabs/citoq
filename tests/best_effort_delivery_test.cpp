#include <cito/detail/best_effort_delivery.hpp>

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    using cito::detail::DeliveryTarget;
    using cito::detail::Locality;

    int encode_calls = 0;
    int local_calls = 0;
    int shm_calls = 0;
    int udp_calls = 0;

    auto encode = [&] {
        ++encode_calls;
        return std::vector<std::uint8_t>{
            1, 2, 3};
    };

    auto local_send =
        [&](cito::DestinationId) {
            ++local_calls;
        };

    const void* first_payload = nullptr;
    const void* second_payload = nullptr;
    const void* udp_payload = nullptr;

    auto shm_send =
        [&](cito::DestinationId,
            const auto& payload) {
            ++shm_calls;

            if (!first_payload) {
                first_payload =
                    &payload;
            } else {
                second_payload =
                    &payload;
            }
        };

    auto udp_send =
        [&](cito::DestinationId,
            const auto& payload) {
            ++udp_calls;
            udp_payload =
                &payload;
        };

    // No destination: no work at all.
    const std::vector<
        DeliveryTarget> none;

    auto result =
        cito::detail::
            deliver_best_effort(
                none,
                local_send,
                encode,
                shm_send,
                udp_send);

    assert(result.delivered() == 0);
    assert(encode_calls == 0);

    // Same-process callbacks stay object-native and never invoke the codec.
    const std::vector<
        DeliveryTarget> local_only{
            {1, Locality::SameProcess},
            {2, Locality::SameProcess}};

    result =
        cito::detail::
            deliver_best_effort(
                local_only,
                local_send,
                encode,
                shm_send,
                udp_send);

    assert(result.in_process == 2);
    assert(result.shared_memory == 0);
    assert(result.udp == 0);
    assert(encode_calls == 0);
    assert(local_calls == 2);

    // Mixed transport fan-out generates the canonical wire once. Both SHM
    // destinations and the UDP destination receive the same payload object.
    const std::vector<
        DeliveryTarget> mixed{
            {3, Locality::SameProcess},
            {4, Locality::SameHost},
            {5, Locality::Remote},
            {6, Locality::SameHost}};

    result =
        cito::detail::
            deliver_best_effort(
                mixed,
                local_send,
                encode,
                shm_send,
                udp_send);

    assert(result.in_process == 1);
    assert(result.shared_memory == 2);
    assert(result.udp == 1);
    assert(result.delivered() == 4);

    assert(encode_calls == 1);
    assert(shm_calls == 2);
    assert(udp_calls == 1);

    assert(first_payload != nullptr);
    assert(first_payload == second_payload);
    assert(second_payload == udp_payload);

    return 0;
}
