#include <citoq/detail/udp_runtime_adapter.hpp>
#include <array>
#include <cassert>
#include <span>
#include <vector>
using citoq::InterestKey;
using citoq::RouteEndpoint;
using citoq::detail::HostRuntimeCore;
using citoq::detail::UdpEndpoint;
using citoq::detail::UdpRuntimeAdapter;
using citoq::detail::UdpSend;

static void transfer_snapshot(
    UdpRuntimeAdapter& pub, UdpRuntimeAdapter& sub,
    UdpEndpoint sub_ip, UdpEndpoint pub_ip) {
    auto reqs=pub.process_control(sub.announcement(sub_ip.port, 5000),
                                  sub_ip, 1000);
    assert(reqs.size()==1);
    auto reply=sub.process_control(reqs[0].bytes,pub_ip,1000);
    assert(reply.size()==1);
    auto done=pub.process_control(reply[0].bytes,sub_ip,1000);
    assert(done.empty());
}
static std::vector<std::uint8_t> request_route(
    UdpRuntimeAdapter& pub, UdpRuntimeAdapter& sub,
    const InterestKey& key, UdpEndpoint pub_ip) {
    auto reqs=pub.ensure_routes(key);
    assert(reqs.size()==1);
    auto reply=sub.process_control(reqs[0].bytes,pub_ip,1000);
    assert(reply.size()==1);
    return reply[0].bytes;
}
static std::vector<std::uint8_t> version_for(
    const UdpRuntimeAdapter& sub, const InterestKey& key) {
    for (const auto& packet:sub.route_version_announcements()) {
        if (citoq::decode_route_version_announcement(packet).key==key)
            return packet;
    }
    assert(false && "missing advertised local DemandKey");
    return {};
}
int main() {
    const InterestKey a{2,10,7},b{2,11,7};
    HostRuntimeCore local;
    assert(local.register_app(10,[](auto,auto,auto){return std::size_t{0};}));
    local.add_interest(10,a);
    local.add_interest(10,b);
    UdpRuntimeAdapter sub(local,10,1001);
    const std::array<RouteEndpoint,1> a1{{{71,0,4441}}};
    const std::array<RouteEndpoint,1> b1{{{72,0,4442}}};
    sub.set_local_route_endpoints_for_key(a,a1);
    sub.set_local_route_endpoints_for_key(b,b1);
    HostRuntimeCore remote;
    UdpRuntimeAdapter pub(remote,20,2001);
    const UdpEndpoint sub_ip{0x0a000002,47010};
    const UdpEndpoint pub_ip{0x0a000001,47020};
    transfer_snapshot(pub,sub,sub_ip,pub_ip);
    auto old_a=request_route(pub,sub,a,pub_ip);
    assert(pub.process_control(old_a,sub_ip,1000).empty());
    auto old_b=request_route(pub,sub,b,pub_ip);
    assert(pub.process_control(old_b,sub_ip,1000).empty());
    assert(pub.quic_endpoint(71)->port==4441);
    assert(pub.quic_endpoint(72)->port==4442);
    assert(remote.remote_destinations(a).contains(71));
    assert(remote.remote_destinations(b).contains(72));

    // Endpoint A changes while InterestKey set remains unchanged.
    const std::array<RouteEndpoint,1> a2{{{73,0,4443}}};
    sub.set_local_route_endpoints_for_key(a,a2);
    auto ads=version_for(sub,a);
    auto request=pub.process_control(ads,sub_ip,1150);
    assert(request.size()==1);
    assert(citoq::decode_route_request(request[0].bytes).key==a);
    // Drop the request or its reply. Version remains outstanding; retry
    // without receiving any additional periodic Announcement.
    auto retry=pub.poll_retries(1300);
    assert(retry.size()==1);
    auto updated=sub.process_control(retry[0].bytes,pub_ip,1300);
    assert(updated.size()==1);
    auto v6=citoq::decode_route_batch(updated[0].bytes);
    assert(v6.version>citoq::decode_route_batch(old_a).version);
    assert(pub.process_control(updated[0].bytes,sub_ip,1301).empty());
    assert(!pub.quic_endpoint(71));
    assert(pub.quic_endpoint(73)->port==4443);
    assert(pub.quic_endpoint(72)->port==4442);
    assert(!remote.remote_destinations(a).contains(71));
    assert(remote.remote_destinations(a).contains(73));
    // Delayed CTR6 from old request cannot roll back the newer state.
    assert(pub.process_control(old_a,sub_ip,1302).empty());
    assert(pub.quic_endpoint(73)->port==4443);
    assert(pub.process_control(ads,sub_ip,1303).empty());

    // Update only B's port; its DestinationId is unchanged.
    const std::array<RouteEndpoint,1> b2{{{72,0,5555}}};
    sub.set_local_route_endpoints_for_key(b,b2);
    request=pub.process_control(version_for(sub,b),sub_ip,1450);
    assert(request.size()==1);
    updated=sub.process_control(request[0].bytes,pub_ip,1450);
    assert(updated.size()==1);
    assert(pub.process_control(updated[0].bytes,sub_ip,1450).empty());
    assert(pub.quic_endpoint(72)->port==5555);
    assert(pub.process_control(old_b,sub_ip,1451).empty());
    assert(pub.quic_endpoint(72)->port==5555);
    assert(pub.quic_endpoint(73)->port==4443);

    // Endpoints removed even though the same InterestKey is still present.
    const std::array<RouteEndpoint,0> none{};
    sub.set_local_route_endpoints_for_key(a,none);
    request=pub.process_control(version_for(sub,a),sub_ip,1500);
    assert(request.size()==1);
    updated=sub.process_control(request[0].bytes,pub_ip,1500);
    assert(updated.size()==1);
    assert(citoq::decode_route_batch(updated[0].bytes).endpoints.empty());
    assert(pub.process_control(updated[0].bytes,sub_ip,1500).empty());
    assert(remote.remote_destinations(a).empty());
    assert(!pub.quic_endpoint(73));
    assert(remote.remote_destinations(b).contains(72));
    assert(pub.quic_endpoint(72)->port==5555);
    return 0;
}
