#include <cito/detail/runtime.hpp>
#include <cassert>
#include <thread>

int main() {
    cito::detail::Runtime runtime(2, 32, 8);
    std::thread::id event_thread;
    std::thread::id work_thread;
    int decoded = 0;

    assert(runtime.try_post([&] {
        event_thread = std::this_thread::get_id();
        assert(runtime.try_offload(
            [&] {
                work_thread = std::this_thread::get_id();
                return 123;
            },
            [&](int value) {
                assert(std::this_thread::get_id() == event_thread);
                decoded = value;
                runtime.stop();
            }));
    }));

    runtime.run();
    assert(decoded == 123);
    assert(work_thread != event_thread);
}
