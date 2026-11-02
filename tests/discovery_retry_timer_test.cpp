#include <citoq/detail/discovery_control_core.hpp>
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

using citoq::detail::DiscoveryControlCore;
using citoq::detail::HostRuntimeCore;

static citoq::DemandSnapshotRequest snapshot(
    const citoq::detail::ControlAction& action) {
    return citoq::decode_snapshot_request(action.bytes);
}
static citoq::RouteRequest route(
    const citoq::detail::ControlAction& action) {
    return citoq::decode_route_request(action.bytes);
}

int main() {
    const citoq::InterestKey key{1,17,99};
    HostRuntimeCore provider;
    assert(provider.register_app(10,[](auto,auto,auto) {
        return std::size_t{1};
    }));
    provider.add_interest(10,key);
    HostRuntimeCore consumer;
    DiscoveryControlCore p(provider,10,101);
    DiscoveryControlCore c(consumer,20,201);

    // Drop the initial SnapshotRequest. There are no new Announcements.
    const auto announcement = p.announcement(47010,5000);
    auto sends = c.on_summary(announcement,1000);
    assert(sends.size()==1);
    assert(snapshot(sends[0]).offset==0);
    assert(c.next_retry_ms().value()==1300);
    assert(c.poll_retries(1299).empty());
    sends=c.poll_retries(1300);
    assert(sends.size()==1 && snapshot(sends[0]).offset==0);
    const auto next=c.next_retry_ms().value();
    assert(next>=1780 && next<=2020); // 600ms +/- 20% from 1300.
    assert(c.poll_retries(next-1).empty());
    // Drop first retransmitted request as well.
    sends=c.poll_retries(next);
    assert(sends.size()==1 && snapshot(sends[0]).offset==0);

    const auto snap_action=p.on_snapshot_request(snapshot(sends[0]));
    assert(!snap_action.bytes.empty());
    // Drop the SnapshotBatch response, and recover on the next timer.
    const auto deadline=c.next_retry_ms().value();
    sends=c.poll_retries(deadline);
    assert(sends.size()==1);
    const auto good=p.on_snapshot_request(snapshot(sends[0]));
    sends=c.on_snapshot_batch(citoq::decode_snapshot_batch(good.bytes));
    assert(sends.empty());
    assert(consumer.candidate_hosts(key).contains(10));
    assert(!c.next_retry_ms());

    // Lose the RouteRequest (or its response) and retry independently.
    sends=c.ensure_routes(key);
    assert(sends.size()==1 && route(sends[0]).key==key);
    const auto route_deadline=c.next_retry_ms().value();
    assert(c.poll_retries(route_deadline-1).empty());
    sends=c.poll_retries(route_deadline);
    assert(sends.size()==1 && route(sends[0]).key==key);
    const std::array<citoq::RouteEndpoint,1> endpoints{{{77,0,4433}}};
    auto reply=p.on_route_request(route(sends[0]),endpoints);
    assert(!reply.bytes.empty());
    const auto batch=citoq::decode_route_batch(reply.bytes);
    assert(c.on_route_batch(batch));
    assert(consumer.remote_destinations(key).contains(77));
    assert(!c.next_retry_ms());
    assert(c.poll_retries(route_deadline+100000).empty());
    assert(c.stats().snapshot_retries==3);
    assert(c.stats().route_retries==1);

    // Multi-page Snapshot is applied only after all pages are received.
    HostRuntimeCore many;
    assert(many.register_app(1,[](auto,auto,auto){return std::size_t{0};}));
    for (std::uint64_t i=1;i<=33;++i)
        many.add_interest(1,{7,i,9});
    HostRuntimeCore view;
    DiscoveryControlCore p2(many,30,300);
    DiscoveryControlCore c2(view,40,400);
    auto pages=c2.on_summary(p2.announcement(47011,5000),5000);
    assert(pages.size()==1 && snapshot(pages[0]).offset==0);
    auto first=p2.on_snapshot_request(snapshot(pages[0]));
    pages=c2.on_snapshot_batch(citoq::decode_snapshot_batch(first.bytes));
    assert(pages.size()==1 && snapshot(pages[0]).offset==32);
    assert(view.candidate_hosts({7,1,9}).empty());
    // Drop page 32 and recover without another Announcement.
    pages=c2.poll_retries(5300);
    assert(pages.size()==1 && snapshot(pages[0]).offset==32);
    auto last=p2.on_snapshot_request(snapshot(pages[0]));
    pages=c2.on_snapshot_batch(citoq::decode_snapshot_batch(last.bytes));
    assert(pages.empty());
    assert(view.candidate_hosts({7,1,9}).contains(30));
    assert(view.candidate_hosts({7,33,9}).contains(30));
    assert(!c2.next_retry_ms());

    // Old incarnation cannot complete a pending request after restart.
    const auto old=citoq::decode_snapshot_batch(first.bytes);
    HostRuntimeCore restarted;
    assert(restarted.register_app(1,[](auto,auto,auto){return std::size_t{0};}));
    restarted.add_interest(1,{7,1,9});
    DiscoveryControlCore p3(restarted,30,301);
    pages=c2.on_summary(p3.announcement(47012,5000),6000);
    assert(pages.size()==1);
    assert(c2.on_snapshot_batch(old).empty());
    auto newer=p3.on_snapshot_request(snapshot(pages[0]));
    assert(c2.on_snapshot_batch(citoq::decode_snapshot_batch(newer.bytes)).empty());
    assert(view.candidate_hosts({7,33,9}).empty());
    assert(view.candidate_hosts({7,1,9}).contains(30));
    return 0;
}
