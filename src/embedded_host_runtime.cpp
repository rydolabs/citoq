#include <cito/detail/embedded_host_runtime.hpp>

#include <mutex>

namespace cito::detail {

std::shared_ptr<EmbeddedHostRuntime>
acquire_embedded_host_runtime() {
    static std::mutex registry_mutex;
    static std::weak_ptr<EmbeddedHostRuntime> weak;

    std::lock_guard<std::mutex> lock(registry_mutex);

    auto runtime = weak.lock();
    if (!runtime) {
        runtime = std::make_shared<EmbeddedHostRuntime>();
        weak = runtime;
    }
    return runtime;
}

} // namespace cito::detail
