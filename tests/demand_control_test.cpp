#include <cito/demand_control.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <stdexcept>

int main() {
    cito::DemandSummaryAnnouncement announcement{
        7,
        47000,
        750,
        {11, 9, 2}};

    const auto announcement_bytes =
        cito::encode_summary_announcement(
            announcement);
    const auto announcement_roundtrip =
        cito::decode_summary_announcement(
            announcement_bytes);

    assert(
        announcement_roundtrip.coordinator ==
        7);
    assert(
        announcement_roundtrip.control_port ==
        47000);
    assert(
        announcement_roundtrip.stamp ==
        announcement.stamp);
    assert(
        announcement_roundtrip.lease_ms ==
        750);

    cito::DemandSnapshotRequest request{
        7,
        11,
        9,
        0,
        32};

    const auto request_roundtrip =
        cito::decode_snapshot_request(
            cito::encode_snapshot_request(
                request));
    assert(request_roundtrip.coordinator == 7);
    assert(request_roundtrip.limit == 32);
    assert(request_roundtrip.version == 9);

    const std::array keys{
        cito::InterestKey{1, 2, 100},
        cito::InterestKey{1, 2, 200}};

    const auto batch =
        cito::make_snapshot_batch(
            7,
            announcement.stamp,
            keys,
            0);
    const auto batch_bytes =
        cito::encode_snapshot_batch(batch);
    const auto batch_roundtrip =
        cito::decode_snapshot_batch(
            batch_bytes);

    assert(
        batch_roundtrip.stamp ==
        announcement.stamp);
    assert(batch_roundtrip.keys.size() == 2);
    assert(
        batch_roundtrip.keys[0] ==
        keys[0]);

    // The bounded batch stays comfortably below a normal safe UDP payload.
    assert(batch_bytes.size() < 1200);

    const cito::RouteRequest route_request{
        7,
        11,
        keys[0]};

    const auto route_request_roundtrip =
        cito::decode_route_request(
            cito::encode_route_request(
                route_request));
    assert(
        route_request_roundtrip.key ==
        keys[0]);
    assert(
        route_request_roundtrip.incarnation ==
        11);

    const cito::RouteBatch routes{
        7,
        11,
        keys[0],
        {
            {91, 48001, 4431},
            {92, 48002, 4432}}};

    const auto route_bytes =
        cito::encode_route_batch(
            routes);

    assert(route_bytes[3] == '5');

    const auto route_roundtrip =
        cito::decode_route_batch(
            route_bytes);

    assert(
        route_roundtrip.incarnation ==
        11);
    assert(
        route_roundtrip.endpoints.size() ==
        2);
    assert(
        route_roundtrip.endpoints[1]
            .destination ==
        92);
    assert(
        route_roundtrip.endpoints[0]
            .data_port ==
        48001);
    assert(
        route_roundtrip.endpoints[0]
            .quic_port ==
        4431);

    // CTR4 remains decodable: its old reserved u16 maps to quic_port=0.
    cito::RouteBatch legacy_routes{
        7,
        11,
        keys[0],
        {{93, 48003, 0}}};

    auto legacy_bytes =
        cito::encode_route_batch(
            legacy_routes);

    legacy_bytes[3] = '4';

    const auto legacy_roundtrip =
        cito::decode_route_batch(
            legacy_bytes);

    assert(
        legacy_roundtrip.endpoints.size() ==
        1);
    assert(
        legacy_roundtrip.endpoints[0]
            .data_port ==
        48003);
    assert(
        legacy_roundtrip.endpoints[0]
            .quic_port ==
        0);

    bool empty_transport_failed = false;
    try {
        cito::RouteBatch bad_routes{
            7,
            11,
            keys[0],
            {{94, 0, 0}}};
        (void)cito::encode_route_batch(
            bad_routes);
    } catch (const std::invalid_argument&) {
        empty_transport_failed = true;
    }
    assert(empty_transport_failed);

    std::array<cito::InterestKey, 33> many{};
    const cito::DemandSummaryStamp stamp{
        1,
        1,
        33};

    const auto first =
        cito::make_snapshot_batch(
            1,
            stamp,
            many,
            0);
    const auto second =
        cito::make_snapshot_batch(
            1,
            stamp,
            many,
            32);

    assert(first.keys.size() == 32);
    assert(second.keys.size() == 1);

    bool invalid_limit = false;
    try {
        cito::DemandSnapshotRequest bad{
            1,
            1,
            1,
            0,
            33};
        (void)cito::encode_snapshot_request(
            bad);
    } catch (const std::invalid_argument&) {
        invalid_limit = true;
    }
    assert(invalid_limit);

    bool zero_progress_failed = false;
    try {
        cito::DemandSnapshotBatch bad{
            7,
            {11, 9, 1},
            0,
            {}};
        (void)cito::encode_snapshot_batch(
            bad);
    } catch (const std::invalid_argument&) {
        zero_progress_failed = true;
    }
    assert(zero_progress_failed);

    return 0;
}
