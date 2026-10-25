#include <cito/cito.hpp>
#include <cito/cito_c.h>

#include <cassert>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

struct BridgeValue {
    std::uint32_t value{};
};

namespace cito {

template <>
struct StaticType<BridgeValue> {
    static Type type() {
        return TypeBuilder("tests.BridgeValue")
            .member<std::uint32_t>(1, "value")
            .build();
    }
};

template <>
struct StaticCodec<BridgeValue> {
    static constexpr bool direct = true;

    static std::vector<std::uint8_t> encode(const BridgeValue& value) {
        return {
            static_cast<std::uint8_t>(value.value & 0xffu),
            static_cast<std::uint8_t>((value.value >> 8) & 0xffu),
            static_cast<std::uint8_t>((value.value >> 16) & 0xffu),
            static_cast<std::uint8_t>((value.value >> 24) & 0xffu)};
    }

    static BridgeValue decode(std::span<const std::uint8_t> bytes) {
        if (bytes.size() != 4) {
            throw std::invalid_argument("bad BridgeValue payload");
        }
        return BridgeValue{
            static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1]) << 8) |
            (static_cast<std::uint32_t>(bytes[2]) << 16) |
            (static_cast<std::uint32_t>(bytes[3]) << 24)};
    }
};

} // namespace cito

struct CState {
    int calls{0};
    std::uint32_t value{0};
};

static void c_callback(
    void* user,
    std::uint64_t,
    const char*,
    std::size_t,
    const std::uint8_t* payload,
    std::size_t payload_size) {
    auto* state = static_cast<CState*>(user);
    const auto decoded = cito::StaticCodec<BridgeValue>::decode(
        std::span<const std::uint8_t>(payload, payload_size));
    ++state->calls;
    state->value = decoded.value;
}

int main() {
    const auto type_id =
        cito::static_type_ref<BridgeValue>().type_id();

    // C++ publisher -> C ABI subscriber crosses executable/DSO boundary.
    cito_context_t* c_subscriber = nullptr;
    assert(cito_context_create("acme", 4, &c_subscriber) == CITO_STATUS_OK);

    CState c_state;
    cito_subscription_t* c_sub = nullptr;
    assert(cito_context_subscribe(
        c_subscriber,
        type_id,
        "bridge",
        6,
        c_callback,
        &c_state,
        &c_sub) == CITO_STATUS_OK);

    {
        cito::Context cpp_publisher("acme");
        assert(cpp_publisher.publish("bridge", BridgeValue{77}) == 1);
    }

    assert(c_state.calls == 1);
    assert(c_state.value == 77);

    // C ABI publisher -> C++ subscriber proves the reverse direction.
    int cpp_calls = 0;
    {
        cito::Context cpp_subscriber("acme");
        auto cpp_sub = cpp_subscriber.subscribe<BridgeValue>(
            "reverse",
            [&](const BridgeValue& value) {
                ++cpp_calls;
                assert(value.value == 91);
            });

        cito_context_t* c_publisher = nullptr;
        assert(cito_context_create("acme", 4, &c_publisher) == CITO_STATUS_OK);

        const auto bytes =
            cito::StaticCodec<BridgeValue>::encode(BridgeValue{91});
        std::size_t delivered = 0;
        assert(cito_context_publish(
            c_publisher,
            type_id,
            "reverse",
            7,
            bytes.data(),
            bytes.size(),
            &delivered) == CITO_STATUS_OK);

        assert(delivered == 1);
        assert(cpp_calls == 1);
        cito_context_destroy(c_publisher);
    }

    cito_subscription_destroy(c_sub);
    cito_context_destroy(c_subscriber);
    return 0;
}
