#include <cito/detail/runtime.hpp>
#include <atomic>
#include <cassert>
#include <thread>

int main() {
    {
        cito::detail::EventLoop loop(2);
        int count = 0;
        assert(loop.try_post([&] { ++count; }));
        assert(loop.try_post([&] { ++count; loop.stop(); }));
        assert(!loop.try_post([&] { ++count; }));
        loop.run();
        assert(count == 2);
    }

    {
        cito::detail::WorkerPool pool(1, 1);
        assert(!pool.started());
        std::atomic<bool> first_started{false};
        std::atomic<bool> release{false};
        assert(pool.try_submit([&] {
            first_started.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        }));
        while (!first_started.load(std::memory_order_acquire)) std::this_thread::yield();
        assert(pool.started());
        assert(pool.try_submit([] {}));
        assert(!pool.try_submit([] {}));
        release.store(true, std::memory_order_release);
        pool.shutdown();
    }

    {
        cito::detail::Runtime runtime(2, 16, 8);
        std::thread::id loop_id;
        std::thread::id worker_id;
        std::thread::id completion_id;
        int result = 0;

        assert(!runtime.workers_started());
        assert(runtime.try_post([&] {
            loop_id = std::this_thread::get_id();
            assert(runtime.is_loop_thread());
            const bool accepted = runtime.try_offload(
                [&] {
                    worker_id = std::this_thread::get_id();
                    return 42;
                },
                [&](int value) {
                    completion_id = std::this_thread::get_id();
                    result = value;
                    runtime.stop();
                });
            assert(accepted);
        }));

        runtime.run();
        assert(result == 42);
        assert(worker_id != loop_id);
        assert(completion_id == loop_id);
        assert(runtime.workers_started());
    }
}
