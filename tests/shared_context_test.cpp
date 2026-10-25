#include <cito/cito.hpp>

#include <cassert>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

struct WireValue {
    std::uint32_t value{};
};

namespace cito {

template <>
struct StaticType<WireValue> {
    static Type type() {
        return TypeBuilder("tests.WireValue")
            .member<std::uint32_t>(1, "value")
            .build();
    }
};

template <>
struct StaticCodec<WireValue> {
    static constexpr bool direct = true;
    static inline std::size_t encodes = 0;
    static inline std::size_t decodes = 0;

    static std::vector<std::uint8_t> encode(const WireValue& value) {
        ++encodes;
        return {
            static_cast<std::uint8_t>(value.value & 0xffu),
            static_cast<std::uint8_t>((value.value >> 8) & 0xffu),
            static_cast<std::uint8_t>((value.value >> 16) & 0xffu),
            static_cast<std::uint8_t>((value.value >> 24) & 0xffu)};
    }

    static WireValue decode(std::span<const std::uint8_t> bytes) {
        ++decodes;
        if (bytes.size() != 4) {
            throw std::invalid_argument("bad WireValue payload");
        }
        return WireValue{
            static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1]) << 8) |
            (static_cast<std::uint32_t>(bytes[2]) << 16) |
            (static_cast<std::uint32_t>(bytes[3]) << 24)};
    }
};

} // namespace cito

int main() {
    using Codec = cito::StaticCodec<WireValue>;

    Codec::encodes = 0;
    Codec::decodes = 0;

    // Same Context remains object-native: no serialization for self demand.
    {
        cito::Context local("acme");
        int calls = 0;
        auto sub = local.subscribe<WireValue>(
            "drone_17",
            [&](const WireValue& value) {
                ++calls;
                assert(value.value == 7);
            });

        assert(local.publish("drone_17", WireValue{7}) == 1);
        assert(calls == 1);
        assert(Codec::encodes == 0);
        assert(Codec::decodes == 0);
    }

    Codec::encodes = 0;
    Codec::decodes = 0;

    // Separate C++ Contexts share exact scope/resource/type demand.
    {
        cito::Context publisher("acme");
        cito::Context subscriber("acme");
        cito::Context isolated("other");

        int calls = 0;
        auto sub = subscriber.subscribe<WireValue>(
            "drone_17",
            [&](const WireValue& value) {
                ++calls;
                assert(value.value == 42);
            });

        assert(publisher.publish("drone_18", WireValue{42}) == 0);
        assert(Codec::encodes == 0);

        assert(isolated.publish("drone_17", WireValue{42}) == 0);
        assert(Codec::encodes == 0);

        assert(publisher.publish("drone_17", WireValue{42}) == 1);
        assert(calls == 1);
        assert(Codec::encodes == 1);
        assert(Codec::decodes == 1);

        sub.close();
        assert(publisher.publish("drone_17", WireValue{42}) == 0);
        assert(Codec::encodes == 1);
    }

    // A publishing app that also subscribes must not receive twice.
    {
        cito::Context publisher("acme");
        cito::Context peer("acme");

        int own_calls = 0;
        int peer_calls = 0;
        auto own = publisher.subscribe<WireValue>(
            "shared",
            [&](const WireValue&) { ++own_calls; });
        auto remote = peer.subscribe<WireValue>(
            "shared",
            [&](const WireValue&) { ++peer_calls; });

        Codec::encodes = 0;
        Codec::decodes = 0;

        assert(publisher.publish("shared", WireValue{1}) == 2);
        assert(own_calls == 1);
        assert(peer_calls == 1);
        assert(Codec::encodes == 1);
        assert(Codec::decodes == 1);
    }

    return 0;
}
