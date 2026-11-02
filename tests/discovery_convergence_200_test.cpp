#include <citoq/detail/discovery_control_core.hpp>
#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using citoq::CoordinatorId;
using citoq::DestinationId;
using citoq::InterestKey;
using citoq::RouteEndpoint;
using citoq::detail::ControlAction;
using citoq::detail::DiscoveryControlCore;
using citoq::detail::HostRuntimeCore;

struct Peer {
    HostRuntimeCore host;
    DiscoveryControlCore control;
    std::vector<InterestKey> keys;
    RouteEndpoint endpoint;
    std::uint64_t version{1};
    Peer(CoordinatorId id, std::uint64_t incarnation,
         std::vector<InterestKey> subscribed, RouteEndpoint route)
        : control(host,id,incarnation),
          keys(std::move(subscribed)), endpoint(route) {
        assert(host.register_app(1,[](auto,auto,auto) {
            return std::size_t{1};
        }));
        for (const auto& key:keys) host.add_interest(1,key);
    }
};

int main() {
    constexpr std::size_t N=200; // 1 publisher + 199 separate host states
    const std::array<InterestKey,4> keys{{{1,1,8},{1,2,8},
                                          {1,3,8},{1,4,8}}};
    HostRuntimeCore publisher;
    DiscoveryControlCore discover(publisher,9999,777);
    std::array<std::unique_ptr<Peer>,N> peers{};
    std::array<std::unordered_set<DestinationId>,4> expected{};
    for (std::size_t i=1;i<N;++i) {
        const auto k=i%4;
        std::vector<InterestKey> advertised{keys[k]};
        if (i%10==0) advertised.push_back(keys[(k+1)%4]);
        RouteEndpoint ep{10000+i,0,static_cast<std::uint16_t>(4000+i)};
        peers[i]=std::make_unique<Peer>(i,1000+i,advertised,ep);
        for (const auto& key:advertised)
            for (std::size_t n=0;n<keys.size();++n)
                if (key==keys[n]) expected[n].insert(ep.destination);
    }
    for(const auto& key:keys) assert(discover.ensure_routes(key).empty());

    std::array<unsigned,N> snapshot_req{},snapshot_resp{},
                            route_req{},route_resp{};
    bool inject_loss=true;
    auto handle=[&](std::vector<ControlAction> pending) {
        std::deque<ControlAction> queue(pending.begin(),pending.end());
        std::size_t processed=0;
        while(!queue.empty()) {
            assert(++processed < 30000);
            auto action=std::move(queue.front());queue.pop_front();
            assert(action.target==citoq::detail::ControlTargetKind::Coordinator);
            const auto coordinator=action.coordinator;
            assert(coordinator>0 && coordinator<N);
            auto& peer=*peers[coordinator];
            const auto family=static_cast<char>(action.bytes[2]);
            if(family=='Q') {
                ++snapshot_req[coordinator];
                if(inject_loss && coordinator%7==0 &&
                   snapshot_req[coordinator]==1) continue;
                auto response=peer.control.on_snapshot_request(
                    citoq::decode_snapshot_request(action.bytes));
                assert(!response.bytes.empty());
                ++snapshot_resp[coordinator];
                if(inject_loss && coordinator%11==0 &&
                   snapshot_resp[coordinator]==1) continue;
                auto follow=discover.on_snapshot_batch(
                    citoq::decode_snapshot_batch(response.bytes));
                for(auto& f:follow)queue.push_back(std::move(f));
            } else if(family=='R') {
                ++route_req[coordinator];
                if(inject_loss && coordinator%13==0 &&
                   route_req[coordinator]==1) continue;
                auto response=peer.control.on_route_request(
                    citoq::decode_route_request(action.bytes),
                    std::span<const RouteEndpoint>(&peer.endpoint,1),
                    peer.version);
                assert(!response.bytes.empty());
                ++route_resp[coordinator];
                if(inject_loss && coordinator%17==0 &&
                   route_resp[coordinator]==1) continue;
                (void)discover.on_route_batch(
                    citoq::decode_route_batch(response.bytes));
            } else assert(false && "unexpected Discovery request family");
        }
    };
    const auto equal=[&] {
        for (std::size_t k=0;k<keys.size();++k)
            if (publisher.remote_destinations(keys[k])!=expected[k])
                return false;
        return true;
    };

    for (std::size_t i=1;i<N;++i)
        handle(discover.on_summary(
            peers[i]->control.announcement(47000,5000),1000));
    assert(!equal()); // deliberate packet loss injects incompleteness
    inject_loss=false;  // network repaired; no more announcements needed
    for(std::uint64_t now=1100;now<=5000;now+=100) {
        handle(discover.poll_retries(now));
        if(equal())break;
    }
    assert(equal()); // exact multi-Key Endpoint sets, no false positives
    assert(discover.stats().snapshot_retries>0);
    assert(discover.stats().route_retries>0);
    for(std::size_t i=1;i<N;++i) {
        assert(publisher.candidate_hosts(peers[i]->keys[0]).contains(i));
    }

    // Same InterestKey, changed Endpoint: refresh only one Key; replay
    // of the old CTR5 response must not overwrite CTR6 version 2.
    constexpr std::size_t changed=42;
    auto& p=*peers[changed];
    const auto key=p.keys[0];
    const auto old=p.endpoint.destination;
    auto legacy=p.control.on_route_request(
        citoq::RouteRequest{changed,1000+changed,key},
        std::span<const RouteEndpoint>(&p.endpoint,1),1);
    const auto old_batch=citoq::decode_route_batch(legacy.bytes);
    p.endpoint.destination=25042;
    p.version=2;
    discover.poll_retries(5100);
    const auto update=discover.on_route_version(
        citoq::RouteVersionAnnouncement{changed,1000+changed,key,2});
    assert(!update.bytes.empty());
    assert(citoq::decode_route_request(update.bytes).supports_route_version);
    // Drop update response, then retry the specific route via timer.
    handle(discover.poll_retries(5500));
    for(std::size_t k=0;k<keys.size();++k)
        if(keys[k]==key) {
            expected[k].erase(old);
            expected[k].insert(p.endpoint.destination);
        }
    assert(equal());
    assert(!discover.on_route_batch(old_batch));

    // Coordinator restarts with new incarnation & endpoint; no stale host
    // destinations are allowed to survive even when InterestKey is unchanged.
    constexpr std::size_t restarted=19;
    const auto old_destination=peers[restarted]->endpoint.destination;
    const auto previous_keys=peers[restarted]->keys;
    peers[restarted]=std::make_unique<Peer>(
        restarted,99999,previous_keys,RouteEndpoint{30019,0,5033});
    handle(discover.on_summary(
        peers[restarted]->control.announcement(47000,5000),6000));
    for (const auto& subscribed:previous_keys)
        for (std::size_t k=0;k<keys.size();++k)
            if(keys[k]==subscribed) {
                expected[k].erase(old_destination);
                expected[k].insert(30019);
            }
    assert(equal());

    // Partition one Coordinator while all others renew leases. It must
    // expire, be removed, and reappear cleanly with a new incarnation.
    constexpr std::size_t parted=5;
    const auto old_parted_dest=peers[parted]->endpoint.destination;
    const auto parted_keys=peers[parted]->keys;
    for (std::size_t i=1;i<N;++i) if(i!=parted)
        handle(discover.on_summary(
            peers[i]->control.announcement(47000,5000),17000));
    (void)discover.expire_coordinators(20000);
    for(const auto& subscribed:parted_keys)
        for(std::size_t k=0;k<keys.size();++k)
            if(keys[k]==subscribed) expected[k].erase(old_parted_dest);
    assert(equal());
    peers[parted]=std::make_unique<Peer>(
        parted,50005,parted_keys,RouteEndpoint{35005,0,5055});
    handle(discover.on_summary(
        peers[parted]->control.announcement(47000,5000),20100));
    for(const auto& subscribed:parted_keys)
        for(std::size_t k=0;k<keys.size();++k)
            if(keys[k]==subscribed) expected[k].insert(35005);
    assert(equal());
    std::cout<<"DISCOVERY_200_HOST_EXACT_CONVERGENCE=PASS\n";
    return 0;
}
