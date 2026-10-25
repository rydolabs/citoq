#include <cito/data_packet.hpp>

#include <cassert>
#include <cstdint>
#include <span>

int main() {
    cito::DataPacket packet{{1, 2, 3}, {1, 2, 3, 4}};
    const auto bytes = cito::encode_data_packet(packet);
    const auto span_bytes =
        cito::encode_data_packet(
            packet.key,
            std::span<const std::uint8_t>(
                packet.payload.data(),
                packet.payload.size()));
    assert(span_bytes == bytes);

    const auto view = cito::decode_data_packet_view(bytes);
    assert(view.key == packet.key);
    assert(view.payload.size() == packet.payload.size());
    assert(view.payload.data() == bytes.data() + 32);
    assert(view.payload[2] == 3);

    const auto decoded = cito::decode_data_packet(bytes);
    assert(decoded.key == packet.key);
    assert(decoded.payload == packet.payload);

    auto truncated = bytes;
    truncated.pop_back();

    bool failed = false;
    try {
        (void)cito::decode_data_packet_view(truncated);
    } catch (const std::runtime_error&) {
        failed = true;
    }
    assert(failed);

    return 0;
}
