#include <cito/remote_routes.hpp>

#include <array>
#include <cassert>

int main() {
    const cito::InterestKey position{
        1,
        17,
        100};

    cito::RemoteRouteIndex routes;

    const std::array first{
        cito::DestinationId{101},
        cito::DestinationId{102}};

    assert(routes.replace(
        10,
        position,
        first));

    assert(
        routes.lookup(position).size() ==
        2);
    assert(
        routes.lookup(position).contains(
            101));
    assert(
        routes.edge_count() == 2);

    // Same route snapshot is idempotent.
    assert(!routes.replace(
        10,
        position,
        first));

    const std::array second{
        cito::DestinationId{103}};

    assert(routes.replace(
        10,
        position,
        second));

    assert(
        routes.lookup(position).size() ==
        1);
    assert(
        routes.lookup(position).contains(
            103));

    // A second coordinator may reference the same destination; removing
    // one coordinator must not remove the still-referenced visible route.
    assert(routes.replace(
        20,
        position,
        second));

    const auto removed =
        routes.remove_coordinator(10);

    assert(removed.size() == 1);
    assert(
        routes.lookup(position).contains(
            103));

    routes.remove_coordinator(20);
    assert(
        routes.lookup(position).empty());
    assert(
        routes.edge_count() == 0);

    return 0;
}
