#include <cito/cito.hpp>

#include <cassert>
#include <string>

struct Position {
    double x;
    double y;
};

int main() {
    cito::Context ctx("smoke");

    int calls = 0;

    // Intent-first subscribe() belongs to the Context. Ignoring its return
    // value must keep the registration active.
    ctx.subscribe<Position>(
        "drone_17",
        [&](const Position& p) {
            ++calls;
            assert(p.x == 1.0);
            assert(p.y == 2.0);
        });

    assert(ctx.scope() == "smoke");
    assert(ctx.publish("drone_17", Position{1.0, 2.0}) == 1);
    assert(ctx.publish("drone_18", Position{1.0, 2.0}) == 0);
    assert(calls == 1);

    // Keeping the returned handle allows explicit early unsubscribe.
    int explicit_calls = 0;
    auto subscription =
        ctx.subscribe<Position>(
            "explicit",
            [&](const Position&) {
                ++explicit_calls;
            });

    assert(ctx.publish("explicit", Position{}) == 1);
    subscription.close();
    assert(ctx.publish("explicit", Position{}) == 0);
    assert(explicit_calls == 1);

    // Legacy on() keeps its original scoped/RAII behavior.
    {
        auto scoped =
            ctx.on<Position>(
                "scoped",
                [&](const Position&) {});
        assert(ctx.publish("scoped", Position{}) == 1);
    }
    assert(ctx.publish("scoped", Position{}) == 0);

    ctx.run();
    return 0;
}
