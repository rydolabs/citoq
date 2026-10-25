#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace cito::detail {

using Task = std::function<void()>;

class EventLoop {
public:
    explicit EventLoop(std::size_t capacity = 4096) : capacity_(capacity) {
        if (capacity_ == 0) throw std::invalid_argument("Cito EventLoop capacity must be greater than zero");
    }

    bool try_post(Task task) {
        if (!task) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_requested_ || queue_.size() >= capacity_) return false;
        queue_.push_back(std::move(task));
        work_cv_.notify_one();
        return true;
    }

    bool post(Task task) {
        if (!task) return false;
        std::unique_lock<std::mutex> lock(mutex_);
        space_cv_.wait(lock, [&] { return stop_requested_ || queue_.size() < capacity_; });
        if (stop_requested_) return false;
        queue_.push_back(std::move(task));
        work_cv_.notify_one();
        return true;
    }

    void run() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (running_) throw std::logic_error("Cito EventLoop is already running");
            running_ = true;
            loop_thread_ = std::this_thread::get_id();
        }

        try {
            for (;;) {
                Task task;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    work_cv_.wait(lock, [&] { return stop_requested_ || !queue_.empty(); });
                    if (queue_.empty() && stop_requested_) break;
                    task = std::move(queue_.front());
                    queue_.pop_front();
                    space_cv_.notify_one();
                }
                task();
            }
        } catch (...) {
            finish_run();
            throw;
        }

        finish_run();
    }

    void stop() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
        work_cv_.notify_all();
        space_cv_.notify_all();
    }

    bool is_loop_thread() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_ && loop_thread_ == std::this_thread::get_id();
    }

    std::size_t pending() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    void finish_run() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        loop_thread_ = {};
        space_cv_.notify_all();
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable space_cv_;
    std::deque<Task> queue_;
    bool stop_requested_{false};
    bool running_{false};
    std::thread::id loop_thread_{};
};

class WorkerPool {
public:
    WorkerPool(std::size_t threads, std::size_t queue_capacity)
        : thread_count_(threads), queue_capacity_(queue_capacity) {
        if (thread_count_ == 0) throw std::invalid_argument("Cito WorkerPool needs at least one thread");
        if (queue_capacity_ == 0) throw std::invalid_argument("Cito WorkerPool queue capacity must be greater than zero");
    }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    ~WorkerPool() { shutdown(); }

    bool try_submit(Task task) {
        if (!task) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || queue_.size() >= queue_capacity_) return false;
        start_locked();
        queue_.push_back(std::move(task));
        cv_.notify_one();
        return true;
    }

    bool started() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return started_;
    }

    void shutdown() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return;
            stopping_ = true;
            cv_.notify_all();
        }
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
        workers_.clear();
    }

private:
    void start_locked() {
        if (started_) return;
        started_ = true;
        workers_.reserve(thread_count_);
        for (std::size_t i = 0; i < thread_count_; ++i) {
            workers_.emplace_back([this] { worker_main(); });
        }
    }

    void worker_main() noexcept {
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                if (queue_.empty() && stopping_) return;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                task();
            } catch (...) {
                // Internal worker tasks must report failures through their completion path.
                // A worker task must never tear down the entire process.
            }
        }
    }

    const std::size_t thread_count_;
    const std::size_t queue_capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> queue_;
    std::vector<std::thread> workers_;
    bool started_{false};
    bool stopping_{false};
};

class Runtime {
public:
    Runtime(std::size_t worker_threads = 2,
            std::size_t event_queue_capacity = 4096,
            std::size_t worker_queue_capacity = 256)
        : loop_(event_queue_capacity), workers_(worker_threads, worker_queue_capacity) {}

    bool try_post(Task task) { return loop_.try_post(std::move(task)); }
    void run() { loop_.run(); }
    void stop() noexcept { loop_.stop(); }
    bool is_loop_thread() const noexcept { return loop_.is_loop_thread(); }
    bool workers_started() const noexcept { return workers_.started(); }

    template <class Work, class Done>
    bool try_offload(Work&& work, Done&& done) {
        using Result = std::invoke_result_t<Work>;
        if constexpr (std::is_void_v<Result>) {
            return workers_.try_submit(
                [this, work = std::forward<Work>(work), done = std::forward<Done>(done)]() mutable {
                    work();
                    (void)loop_.post([done = std::move(done)]() mutable { done(); });
                });
        } else {
            return workers_.try_submit(
                [this, work = std::forward<Work>(work), done = std::forward<Done>(done)]() mutable {
                    Result result = work();
                    (void)loop_.post(
                        [done = std::move(done), result = std::move(result)]() mutable {
                            done(std::move(result));
                        });
                });
        }
    }

private:
    EventLoop loop_;
    WorkerPool workers_;
};

} // namespace cito::detail
