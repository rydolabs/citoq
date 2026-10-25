#include <cito/cito.hpp>
#include <cito/cito_c.h>
#include <cito/detail/raw_context.hpp>

#include <cassert>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

int main() {
    // The executing callback used to live inside a vector element that was
    // freed by vector growth. In ASan builds this was a heap-use-after-free.
    {
        cito::detail::RawContext ctx("reentrant");
        std::vector<cito::detail::RawSubscription> subscriptions;
        std::size_t callbacks = 0;
        auto root = ctx.on(42, "updates",
            [&](std::uint64_t, std::string_view, std::span<const std::uint8_t>) {
                if (callbacks == 0) {
                    for (int i = 0; i < 256; ++i) {
                        subscriptions.push_back(ctx.on(42, "updates",
                            [&](std::uint64_t, std::string_view,
                                std::span<const std::uint8_t>) { ++callbacks; }));
                    }
                }
                ++callbacks; // check after growth of the subscription bucket
            });
        const std::uint8_t payload = 7;
        assert(ctx.publish(42, "updates", std::span(&payload, 1)) == 1);
        assert(callbacks == 1);
        assert(ctx.publish(42, "updates", std::span(&payload, 1)) == 257);
        assert(callbacks == 258);
    }
    {
        cito::Context ctx("reentrant");
        std::size_t callbacks = 0;
        auto root = ctx.subscribe<int>("updates", [&](const int&) {
            if (callbacks == 0) {
                for (int i = 0; i < 256; ++i) {
                    ctx.subscribe<int>("updates",
                        [&](const int&) { ++callbacks; });
                }
            }
            ++callbacks;
        });
        assert(ctx.publish<int>("updates", 1) == 1);
        assert(callbacks == 1);
        assert(ctx.publish<int>("updates", 2) == 257);
        assert(callbacks == 258);
    }
    return 0;
}
