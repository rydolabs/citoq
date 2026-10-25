#include <cito/detail/request_transport.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

bool throws_decode(
    std::span<const std::uint8_t> bytes) {
    try {
        (void)cito::detail::
            decode_request_transport_frame(
                bytes);
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}

void seed_route(
    cito::detail::HostRuntimeCore& host,
    const cito::detail::RequestKey& key,
    std::uint64_t version) {
    const auto capability =
        cito::detail::
            request_handler_interest_key(
                key);

    if (!host.coordinator_is_current(
            10,
            1001)) {
        assert(
            host.observe_coordinator(
                10,
                1001,
                10000,
                0) ==
            cito::
                CoordinatorLeaseObservation::
                    Added);
    }

    const std::array<
        cito::InterestKey,
        1> keys{capability};

    assert(
        host.apply_remote_snapshot(
            10,
            {1001, version, 1},
            keys));

    const std::array<
        cito::DestinationId,
        2> destinations{
            88,
            77};

    assert(
        host.replace_remote_routes(
            10,
            1001,
            capability,
            destinations));
}

} // namespace

int main() {
    using namespace cito::detail;

    const RequestKey key{
        0x1112131415161718ull,
        0x2122232425262728ull,
        0x3132333435363738ull};

    const std::array<
        std::uint8_t,
        2> golden_payload{
            0xaa,
            0xbb};

    const auto golden =
        encode_request_transport_request(
            0x0102030405060708ull,
            key,
            golden_payload);

    const std::vector<std::uint8_t>
        expected{
            'C','T','X','1',
            1,0,0,0,
            0x08,0x07,0x06,0x05,
            0x04,0x03,0x02,0x01,
            0x18,0x17,0x16,0x15,
            0x14,0x13,0x12,0x11,
            0x28,0x27,0x26,0x25,
            0x24,0x23,0x22,0x21,
            0x38,0x37,0x36,0x35,
            0x34,0x33,0x32,0x31,
            0x02,0x00,0x00,0x00,
            0xaa,0xbb};

    assert(golden == expected);

    const auto decoded =
        decode_request_transport_frame(
            golden);

    assert(
        decoded.kind ==
        RequestTransportKind::Request);
    assert(
        decoded.status ==
        RequestStatus::Ok);
    assert(
        decoded.request_id ==
        0x0102030405060708ull);
    assert(decoded.key == key);
    assert(decoded.payload.size() == 2);
    assert(decoded.payload[0] == 0xaa);
    assert(
        decoded.payload.data() ==
        golden.data() + 44);

    auto bad = golden;
    bad.pop_back();
    assert(throws_decode(bad));

    bad = golden;
    bad[6] = 1;
    assert(throws_decode(bad));

    bad = golden;
    bad[4] = 99;
    assert(throws_decode(bad));

    bad = golden;
    bad[4] = static_cast<std::uint8_t>(
        RequestTransportKind::Reply);
    assert(throws_decode(bad));

    const auto cancel_frame =
        encode_request_transport_cancel(
            9);

    const auto cancel_decoded =
        decode_request_transport_frame(
            cancel_frame);

    assert(
        cancel_decoded.kind ==
        RequestTransportKind::Cancel);
    assert(
        cancel_decoded.status ==
        RequestStatus::Cancelled);
    assert(cancel_decoded.payload.empty());

    const RequestKey ping_key{
        7,
        17,
        100};

    HostRuntimeCore client_host;
    RequestReplyCore client_requests;
    RequestTransportCore client(
        client_host,
        client_requests);

    int missing_calls = 0;

    const std::array<
        std::uint8_t,
        4> ping{'p','i','n','g'};

    const auto missing =
        client.start(
            1,
            ping_key,
            ping,
            [&](RequestStatus status,
                std::span<
                    const std::uint8_t>) {
                ++missing_calls;
                assert(
                    status ==
                    RequestStatus::
                        Unavailable);
            });

    assert(!missing);
    assert(missing_calls == 1);
    assert(
        client_requests
            .pending_count() == 0);
    assert(client.outbound_count() == 0);

    seed_route(
        client_host,
        ping_key,
        1);

    HostRuntimeCore server_host;
    RequestReplyCore server_requests;
    RequestTransportCore server(
        server_host,
        server_requests);

    int handler_calls = 0;

    server_requests.register_handler(
        2,
        ping_key,
        [&](RequestId request_id,
            AppId requester_app,
            std::span<
                const std::uint8_t>
                payload,
            RequestReplyCore::Reply
                reply) {
            ++handler_calls;
            assert(request_id != 0);
            assert(requester_app == 0);
            assert(
                std::vector<
                    std::uint8_t>(
                    payload.begin(),
                    payload.end()) ==
                std::vector<
                    std::uint8_t>(
                    {'p','i','n','g'}));

            const std::array<
                std::uint8_t,
                4> pong{
                    'p','o','n','g'};

            assert(
                reply(
                    RequestStatus::Ok,
                    pong));
        });

    int completion_calls = 0;
    std::vector<std::uint8_t>
        response;

    auto send =
        client.start(
            1,
            ping_key,
            ping,
            [&](RequestStatus status,
                std::span<
                    const std::uint8_t>
                    payload) {
                ++completion_calls;
                assert(
                    status ==
                    RequestStatus::Ok);
                response.assign(
                    payload.begin(),
                    payload.end());
            });

    assert(send);
    assert(send->destination == 77);
    assert(send->request_id != 0);
    assert(client.outbound_count() == 1);
    assert(
        client_requests
            .pending_count() == 1);

    // A reply carrying the right RequestId on the wrong peer/session must
    // not complete the request.
    const auto forged =
        encode_request_transport_reply(
            send->request_id,
            RequestStatus::Ok,
            {});

    assert(
        !client.on_reply(
            88,
            forged));
    assert(client.outbound_count() == 1);
    assert(
        client_requests
            .pending_count() == 1);
    assert(completion_calls == 0);

    std::vector<std::uint8_t>
        reply_bytes;

    assert(
        server.on_request(
            77,
            send->bytes,
            [&](std::span<
                    const std::uint8_t>
                    bytes) {
                reply_bytes.assign(
                    bytes.begin(),
                    bytes.end());
                return true;
            }));

    assert(handler_calls == 1);
    assert(server.inbound_count() == 0);
    assert(!reply_bytes.empty());

    assert(
        client.on_reply(
            77,
            reply_bytes));

    assert(completion_calls == 1);
    assert(
        response ==
        std::vector<std::uint8_t>(
            {'p','o','n','g'}));
    assert(client.outbound_count() == 0);
    assert(
        client_requests
            .pending_count() == 0);

    // Unknown reply IDs are ignored.
    const auto unknown_reply =
        encode_request_transport_reply(
            999,
            RequestStatus::Ok,
            {});
    assert(
        !client.on_reply(
            77,
            unknown_reply));

    // Cancellation removes requester correlation and suppresses a late reply.
    const RequestKey slow_key{
        7,
        17,
        101};

    seed_route(
        client_host,
        slow_key,
        2);

    RequestReplyCore::Reply
        late_reply;

    server_requests.register_handler(
        3,
        slow_key,
        [&](RequestId,
            AppId,
            std::span<
                const std::uint8_t>,
            RequestReplyCore::Reply
                reply) {
            late_reply =
                std::move(reply);
        });

    int cancelled_calls = 0;

    auto slow_send =
        client.start(
            1,
            slow_key,
            ping,
            [&](RequestStatus status,
                std::span<
                    const std::uint8_t>) {
                ++cancelled_calls;
                assert(
                    status ==
                    RequestStatus::
                        Cancelled);
            });

    assert(slow_send);

    std::vector<std::uint8_t>
        unexpected_reply;

    assert(
        server.on_request(
            77,
            slow_send->bytes,
            [&](std::span<
                    const std::uint8_t>
                    bytes) {
                unexpected_reply.assign(
                    bytes.begin(),
                    bytes.end());
                return true;
            }));

    assert(server.inbound_count() == 1);
    assert(unexpected_reply.empty());

    auto cancel =
        client.cancel(
            slow_send->request_id);

    assert(cancel);
    assert(cancel->destination == 77);
    assert(cancelled_calls == 1);
    assert(client.outbound_count() == 0);
    assert(
        client_requests
            .pending_count() == 0);

    assert(
        server.on_cancel(
            77,
            cancel->bytes));
    assert(server.inbound_count() == 0);

    const std::array<
        std::uint8_t,
        4> late{
            'l','a','t','e'};

    assert(
        !late_reply(
            RequestStatus::Ok,
            late));
    assert(unexpected_reply.empty());

    // The same RequestId may be active on different QUIC peer sessions.
    RequestReplyCore::Reply peer_a_reply;
    RequestReplyCore::Reply peer_b_reply;

    const RequestKey peer_key{
        7,
        17,
        103};

    server_requests.register_handler(
        5,
        peer_key,
        [&](RequestId,
            AppId,
            std::span<
                const std::uint8_t>,
            RequestReplyCore::Reply reply) {
            if (!peer_a_reply) {
                peer_a_reply =
                    std::move(reply);
            } else {
                peer_b_reply =
                    std::move(reply);
            }
        });

    const auto peer_frame =
        encode_request_transport_request(
            1,
            peer_key,
            {});

    std::vector<std::uint8_t>
        peer_a_bytes;
    std::vector<std::uint8_t>
        peer_b_bytes;

    assert(
        server.on_request(
            1001,
            peer_frame,
            [&](std::span<
                    const std::uint8_t>
                    bytes) {
                peer_a_bytes.assign(
                    bytes.begin(),
                    bytes.end());
                return true;
            }));

    assert(
        server.on_request(
            1002,
            peer_frame,
            [&](std::span<
                    const std::uint8_t>
                    bytes) {
                peer_b_bytes.assign(
                    bytes.begin(),
                    bytes.end());
                return true;
            }));

    assert(server.inbound_count() == 2);

    const std::array<
        std::uint8_t,
        1> peer_a_payload{'a'};
    const std::array<
        std::uint8_t,
        1> peer_b_payload{'b'};

    assert(
        peer_a_reply(
            RequestStatus::Ok,
            peer_a_payload));
    assert(
        peer_b_reply(
            RequestStatus::Ok,
            peer_b_payload));

    assert(server.inbound_count() == 0);
    assert(
        decode_request_transport_frame(
            peer_a_bytes)
            .request_id == 1);
    assert(
        decode_request_transport_frame(
            peer_b_bytes)
            .request_id == 1);

    // Transport loss fails only requests targeting that destination.
    int unavailable_calls = 0;

    auto lost =
        client.start(
            1,
            slow_key,
            ping,
            [&](RequestStatus status,
                std::span<
                    const std::uint8_t>) {
                ++unavailable_calls;
                assert(
                    status ==
                    RequestStatus::
                        Unavailable);
            });

    assert(lost);
    assert(
        client.fail_destination(
            77) == 1);
    assert(unavailable_calls == 1);
    assert(client.outbound_count() == 0);

    // External handler exceptions become Internal and still emit one reply.
    const RequestKey throw_key{
        7,
        17,
        102};

    server_requests.register_handler(
        4,
        throw_key,
        [](RequestId,
           AppId,
           std::span<
               const std::uint8_t>,
           RequestReplyCore::Reply) {
            throw std::runtime_error(
                "boom");
        });

    const auto throwing =
        encode_request_transport_request(
            500,
            throw_key,
            {});

    std::vector<std::uint8_t>
        internal_reply;

    assert(
        server.on_request(
            88,
            throwing,
            [&](std::span<
                    const std::uint8_t>
                    bytes) {
                internal_reply.assign(
                    bytes.begin(),
                    bytes.end());
                return true;
            }));

    const auto internal =
        decode_request_transport_frame(
            internal_reply);

    assert(
        internal.kind ==
        RequestTransportKind::Reply);
    assert(
        internal.status ==
        RequestStatus::Internal);
    assert(internal.request_id == 500);
    assert(server.inbound_count() == 0);

    return 0;
}
