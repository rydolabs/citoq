#include <cito/detail/reliable_packet.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <vector>

int main() {
    using namespace
        cito::detail::reliable;

    const cito::InterestKey key{
        1,
        17,
        100};

    const std::vector<std::uint8_t>
        payload{9, 8, 7};

    const auto data_bytes =
        encode_reliable_data(
            {
                key,
                55,
                7,
                payload});

    const auto data =
        decode_reliable_data_view(
            data_bytes);

    assert(data.key == key);
    assert(data.stream == 55);
    assert(data.sequence == 7);
    assert(data.payload.size() == 3);
    assert(data.payload[1] == 8);

    // Transport framing does not copy the DATA payload.
    assert(
        data.payload.data() ==
        data_bytes.data() + 48);

    const HeartbeatFrame heartbeat{
        key,
        55,
        {3, 7}};

    const auto heartbeat_roundtrip =
        decode_heartbeat_frame(
            encode_heartbeat_frame(
                heartbeat));

    assert(
        heartbeat_roundtrip.key ==
        key);
    assert(
        heartbeat_roundtrip.stream ==
        55);
    assert(
        heartbeat_roundtrip.heartbeat
            .first_available ==
        3);
    assert(
        heartbeat_roundtrip.heartbeat
            .last_published ==
        7);

    const NackFrame nack{
        key,
        55,
        {4, 0b1011}};

    const auto nack_roundtrip =
        decode_nack_frame(
            encode_nack_frame(
                nack));

    assert(
        nack_roundtrip.nack.base ==
        4);
    assert(
        nack_roundtrip.nack.bitmap ==
        0b1011);

    const GapFrame gap{
        key,
        55,
        {1, 3}};

    const auto gap_roundtrip =
        decode_gap_frame(
            encode_gap_frame(
                gap));

    assert(
        gap_roundtrip.gap.first ==
        1);
    assert(
        gap_roundtrip.gap.last ==
        3);

    assert(
        encode_heartbeat_frame(
            heartbeat).size() ==
        52);
    assert(
        encode_nack_frame(
            nack).size() ==
        52);
    assert(
        encode_gap_frame(
            gap).size() ==
        52);

    auto truncated =
        data_bytes;
    truncated.pop_back();

    bool truncated_failed = false;
    try {
        (void)
            decode_reliable_data_view(
                truncated);
    } catch (
        const std::runtime_error&) {
        truncated_failed = true;
    }
    assert(truncated_failed);

    bool empty_nack_failed = false;
    try {
        (void)encode_nack_frame(
            {
                key,
                55,
                {4, 0}});
    } catch (
        const std::invalid_argument&) {
        empty_nack_failed = true;
    }
    assert(empty_nack_failed);

    return 0;
}
