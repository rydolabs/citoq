#include <cito/cito.hpp>
#include <cito/detail/embedded_host_runtime.hpp>

#include <cassert>
#include <memory>

namespace {

class FakeDriver final
    : public cito::detail::RuntimeDriver {
public:
    void run() override {
        ++runs;
    }

    void stop() noexcept override {
        ++stops;
    }

    int runs{0};
    int stops{0};
};

} // namespace

int main() {
    auto runtime =
        cito::detail::
            acquire_embedded_host_runtime();

    cito::Context ctx(
        "driver-test");

    // No installed distributed driver keeps the existing local no-op run.
    ctx.run();

    auto driver =
        std::make_shared<
            FakeDriver>();

    assert(
        runtime->install_driver(
            driver));
    assert(
        !runtime->install_driver(
            std::make_shared<
                FakeDriver>()));

    ctx.run();
    assert(driver->runs == 1);

    ctx.stop();
    assert(driver->stops == 1);

    assert(
        runtime->clear_driver(
            driver.get()));
    assert(driver->stops == 2);
    assert(
        !runtime->has_driver());

    ctx.run();
    assert(driver->runs == 1);

    return 0;
}
