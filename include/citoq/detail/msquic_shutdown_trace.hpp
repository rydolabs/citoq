#pragma once
// Opt-in teardown-phase timestamps. No changes to MsQuic control flow.
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace citoq::detail::diagnostics {
inline void shutdown_trace(
    const char* owner,
    const char* phase,
    std::size_t connections_out = 0,
    std::size_t connections_in = 0,
    std::size_t outstanding = 0) noexcept {
    static const bool enabled = [] {
        const auto* p = std::getenv("CITOQ_TRACE_MSQUIC_SHUTDOWN");
        return p && p[0] == '1' && p[1] == '\0';
    }();
    if (!enabled) return;
    const auto usec = std::chrono::duration_cast<
        std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::fprintf(stderr,
        "CITOQ_MSQUIC_SHUTDOWN owner=%s phase=%s monotonic_us=%lld "
        "outbound=%zu inbound=%zu outstanding=%zu\n",
        owner, phase, static_cast<long long>(usec),
        connections_out, connections_in, outstanding);
    std::fflush(stderr);
}
} // namespace citoq::detail::diagnostics
