#include <cito/cito.hpp>

#include <cassert>
#include <string>

struct Position {
    double x;
    double y;
};

struct Battery {
    int percent;
};

int main() {
    cito::Context ctx;

    int position_calls = 0;
    auto pos_sub = ctx.on<Position>([&](const Position& p) {
        ++position_calls;
        assert(p.x == 1.0);
        assert(p.y == 2.0);
    });

    assert(ctx.publish(Position{1.0, 2.0}) == 1);
    assert(position_calls == 1);
    assert(ctx.publish(Battery{50}) == 0);

    int resource_calls = 0;
    auto resource_sub = ctx.on<Position>("drone_17", [&](const Position&) {
        ++resource_calls;
    });

    assert(ctx.publish("drone_17", Position{3.0, 4.0}) == 1);
    assert(ctx.publish("drone_18", Position{3.0, 4.0}) == 0);
    assert(resource_calls == 1);

    resource_sub.close();
    assert(ctx.publish("drone_17", Position{3.0, 4.0}) == 0);

    cito::Context scoped("acme.robotics");
    assert(scoped.scope() == "acme.robotics");

    return 0;
}
