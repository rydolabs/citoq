#include <cito/cito.hpp>

#include <cassert>
#include <memory>
#include <string>
#include <vector>

struct Position {
    int value{};
};

struct Battery {
    int value{};
};

int main() {
    cito::Context ctx;

    int calls = 0;
    std::vector<cito::Subscription> unrelated;
    unrelated.reserve(2000);

    for (int i = 0; i < 1000; ++i) {
        unrelated.push_back(
            ctx.on<Position>(
                "other_" + std::to_string(i),
                [](const Position&) {}));

        unrelated.push_back(
            ctx.on<Battery>(
                "target",
                [](const Battery&) {}));
    }

    auto target =
        ctx.on<Position>(
            "target",
            [&](const Position& position) {
                ++calls;
                assert(position.value == 7);
            });

    assert(
        ctx.publish(
            "target",
            Position{7}) == 1);
    assert(calls == 1);

    assert(
        ctx.publish(
            "missing",
            Position{7}) == 0);

    // Closing a later subscriber from inside a callback must
    // safely suppress it during the same dispatch.
    int second_calls = 0;
    cito::Subscription second;

    auto first =
        ctx.on<Position>(
            "reentrant",
            [&](const Position&) {
                second.close();
            });

    second =
        ctx.on<Position>(
            "reentrant",
            [&](const Position&) {
                ++second_calls;
            });

    assert(
        ctx.publish(
            "reentrant",
            Position{}) == 1);
    assert(second_calls == 0);

    // A subscription added during dispatch starts with the next
    // publish, not halfway through the current sample.
    int late_calls = 0;
    cito::Subscription late;
    bool added = false;

    auto adder =
        ctx.on<Position>(
            "add",
            [&](const Position&) {
                if (!added) {
                    added = true;
                    late =
                        ctx.on<Position>(
                            "add",
                            [&](const Position&) {
                                ++late_calls;
                            });
                }
            });

    assert(
        ctx.publish(
            "add",
            Position{}) == 1);
    assert(late_calls == 0);

    assert(
        ctx.publish(
            "add",
            Position{}) == 2);
    assert(late_calls == 1);

    // A Subscription may outlive its Context without touching
    // destroyed Context storage.
    cito::Subscription survivor;

    {
        auto temporary =
            std::make_unique<cito::Context>();

        survivor =
            temporary->on<Position>(
                [](const Position&) {});
    }

    survivor.close();

    return 0;
}
