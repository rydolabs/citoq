# CitoQ

**CitoQ** is a typed messaging middleware with demand-driven discovery, Publish/Subscribe and Request/Reply APIs.

> **Pre-release development source. Not production release ready.** The API and packaging were fully renamed from the former Cito project. No stable ABI or wire-protocol compatibility with the earlier development snapshots is promised.

## Public API transport and scaling benchmarks (experimental)

Benchmarks below use the **actual CitoQ `Context.publish_raw()` + native mTLS/QUIC** data path, *not* the internal UDP research adapter. Ubuntu GitHub Actions uses separate Linux network namespaces and veth/bridge; peers use their own public application APIs. All 30-node trials below delivered **224/224** expected messages (4 per each of 28 interested readers × 2 repetitions), with one separate uninterested node per run.

| 30 nodes, 256B | First data reaches all interested readers* | Total veth RX |
| --- | ---: | ---: |
| **CitoQ Context + mTLS/QUIC** | 756.3ms | **1.10MB** |
| Cyclone DDS C API | 538.9ms | 4.19MB |
| Zenoh Python peer TCP | 544.2ms | 4.76MB |
| Fast DDS C++ API | 3,590.3ms | 24.20MB |

At 30 nodes, this CitoQ test used about **73.7% less whole-trial veth RX than Cyclone DDS**, but took about **217ms longer** until the last interested reader received data. At 3/10 nodes the network comparison was less favorable to CitoQ; **do not generalize a product ranking** from one workload. *The first-message timing includes a deliberate 500ms post-readiness delay, process setup and CitoQ's mTLS handshake. Other engines have different QoS, serialization and security. Entire-run traffic also includes Discovery and connection maintenance.*

Five public-API message sizes (64B, 256B, 1KiB, 4KiB, 16KiB) at a *paced* 100Hz each passed **40/40** CitoQ/CycloneDDS/Zenoh/FastDDS trials; CitoQ p95 ranged **0.683–0.768ms**. Offered 100Hz is not measured maximum throughput. The CitoQ optimization removes redundant send/receive buffer copies, but six-pair high-rate A/B results are noisy and do not prove a robust latency speedup.

**Evidence:** [3/10/30-node 24/24 Actions](https://github.com/rydolabs/citoq/actions/runs/40462524812) · [5-size 40/40 Actions](https://github.com/rydolabs/citoq/actions/runs/40463065417) · [same-runner source A/B](https://github.com/rydolabs/citoq/actions/runs/40462840456) · [methods, exact source, limitations](docs/PUBLIC_API_PERF_COMPARE_2026-10-27.md).


## Build and test (Linux)

Prerequisites: CMake >= 3.20, C++20 compiler, Python 3, Threads, optional Ninja.

```sh
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON -DCITOQ_ENABLE_MSQUIC=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$PWD/stage"
```

## Use from C++

```cpp
#include <citoq/citoq.hpp>

int main() {
    citoq::Context context;
    return 0;
}
```

From an external CMake project, use `find_package(CitoQ CONFIG REQUIRED)` and `target_link_libraries(app PRIVATE CitoQ::Core)`. The native C API is `<citoq/citoq_c.h>` and its functions are named `citoq_*`. `libcitoq.so` provides the C ABI, with `libcitoq_core.so` as the runtime. The Python package uses `import citoq` and `CITOQ_LIBRARY` to locate the native C library. `citoq-idlc` is installed as the type generator.

## Optional networking

Native MsQuic integration is disabled by default and requires MsQuic headers and libraries when enabled (`CITOQ_ENABLE_MSQUIC=ON`). This profile still needs a separate network mTLS acceptance run and has not been certified for production.

## Project records

- [Release status](docs/RELEASE_STATUS.md)
- [API](docs/PUBLIC_API.md)
- [Architecture](docs/ARCHITECTURE_CONSTITUTION.md)
- [Source provenance](docs/SOURCE_PROVENANCE.md)
- [Testing](docs/TESTING.md)

The original engineering source remains preserved in [the Cito development PR](https://github.com/nssang00/chatgpt-write-test/pull/25). See provenance for the exact source commit. A redistribution license is not yet selected.
