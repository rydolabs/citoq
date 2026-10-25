#pragma once

#include <cito/detail/host_runtime_core.hpp>
#include <cito/detail/request_network_adapter.hpp>
#include <cito/detail/request_reply_core.hpp>

#include <atomic>
#include <memory>
#include <mutex>

#if defined(_WIN32)
#  if defined(CITO_CORE_BUILD)
#    define CITO_CORE_API __declspec(dllexport)
#  else
#    define CITO_CORE_API __declspec(dllimport)
#  endif
#else
#  define CITO_CORE_API __attribute__((visibility("default")))
#endif

namespace cito::detail {

class RuntimeDriver {
public:
    virtual ~RuntimeDriver() = default;
    virtual void run() = 0;
    virtual void stop() noexcept = 0;
};

class EmbeddedHostRuntime {
public:
    EmbeddedHostRuntime()
        : request_network_(
              host_,
              requests_) {}

    AppId allocate_app_id() noexcept {
        return next_app_id_.fetch_add(1, std::memory_order_relaxed);
    }

    HostRuntimeCore& host() noexcept { return host_; }
    const HostRuntimeCore& host() const noexcept { return host_; }

    RequestReplyCore& requests() noexcept { return requests_; }
    const RequestReplyCore& requests() const noexcept { return requests_; }

    RequestNetworkAdapter& request_network() noexcept {
        return request_network_;
    }

    const RequestNetworkAdapter& request_network() const noexcept {
        return request_network_;
    }

    bool install_driver(
        std::shared_ptr<RuntimeDriver> driver) {
        if (!driver) {
            return false;
        }

        std::lock_guard<std::mutex>
            lock(driver_mutex_);

        if (driver_) {
            return false;
        }

        driver_ =
            std::move(driver);
        return true;
    }

    bool clear_driver(
        const RuntimeDriver* expected = nullptr) {
        std::shared_ptr<RuntimeDriver>
            previous;

        {
            std::lock_guard<std::mutex>
                lock(driver_mutex_);

            if (
                !driver_ ||
                (expected &&
                 driver_.get() !=
                     expected)) {
                return false;
            }

            previous =
                std::move(driver_);
        }

        previous->stop();
        return true;
    }

    bool has_driver()
        const noexcept {
        std::lock_guard<std::mutex>
            lock(driver_mutex_);
        return
            static_cast<bool>(
                driver_);
    }

    void run() {
        std::shared_ptr<RuntimeDriver>
            driver;

        {
            std::lock_guard<std::mutex>
                lock(driver_mutex_);
            driver = driver_;
        }

        if (driver) {
            driver->run();
        }
    }

    void stop() noexcept {
        std::shared_ptr<RuntimeDriver>
            driver;

        {
            std::lock_guard<std::mutex>
                lock(driver_mutex_);
            driver = driver_;
        }

        if (driver) {
            driver->stop();
        }
    }

private:
    std::atomic<AppId> next_app_id_{1};
    HostRuntimeCore host_;
    RequestReplyCore requests_;
    RequestNetworkAdapter request_network_;

    mutable std::mutex driver_mutex_;
    std::shared_ptr<RuntimeDriver>
        driver_;
};

// Must be out-of-line so an executable and libcito share one registry.
CITO_CORE_API std::shared_ptr<EmbeddedHostRuntime>
acquire_embedded_host_runtime();

} // namespace cito::detail
