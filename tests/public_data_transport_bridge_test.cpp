#include <citoq/citoq.hpp>
#include <citoq/data_packet.hpp>
#include <citoq/detail/embedded_host_runtime.hpp>
#include <citoq/identity.hpp>

#include <cassert>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

struct PacketValue { std::uint32_t value; };

namespace citoq {
template <> struct StaticType<PacketValue> {
    static Type type() {
        return TypeBuilder("tests.PacketValue")
            .member<std::uint32_t>(1, "value").build();
    }
};
template <> struct StaticCodec<PacketValue> {
    static constexpr bool direct = true;
    static std::vector<std::uint8_t> encode(const PacketValue& value) {
        return {static_cast<std::uint8_t>(value.value)};
    }
    static PacketValue decode(std::span<const std::uint8_t> bytes) {
        if (bytes.size() != 1) throw std::runtime_error("invalid data value");
        return {bytes[0]};
    }
};
}

int main() {
    auto runtime = citoq::detail::acquire_embedded_host_runtime();
    citoq::Context receiver("g1-transport");
    std::size_t received = 0;
    auto sub = receiver.on<PacketValue>("sensor", [&](const PacketValue& v) {
        assert(v.value == 23);
        ++received;
    });
    const citoq::InterestKey key{
        citoq::stable_name_id("g1-transport"),
        citoq::stable_name_id("sensor"),
        citoq::StaticType<PacketValue>::type().type_id()};
    std::array<std::uint8_t, 1> payload{23};
    const auto packet = citoq::encode_data_packet(key, payload);
    auto& network = runtime->request_network();
    assert(network.receive_data(packet) == 1);
    assert(received == 1);
    assert(network.receive_data(std::span(packet.data(), 4)) == 0);
    assert(!network.send_data(777, packet));
    std::vector<std::uint8_t> sent;
    network.attach_session(777, [&](std::span<const std::uint8_t> b) {
        sent.assign(b.begin(), b.end());
        return true;
    });
    assert(network.send_data(777, packet));
    assert(sent == packet);
    assert(!network.send_data(777, std::span(packet.data(), 4)));
    network.detach_session(777);
    sub.close();
    assert(network.receive_data(packet) == 0);
}
