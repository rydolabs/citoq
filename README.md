# CitoQ

**CitoQ** is a typed messaging middleware with demand-driven discovery, Publish/Subscribe and Request/Reply APIs.

> **Pre-release development source. Not production release ready.** The API and packaging were fully renamed from the former Cito project. No stable ABI or wire-protocol compatibility with the earlier development snapshots is promised.

## Benchmarks and discovery (experimental, 2026-10-26)

**Measured results, not an overall middleware ranking.** The runs below are from GitHub Actions on Ubuntu 24.04 with isolated Linux network namespaces, veth pairs, a Linux bridge, and `tc netem`. The CitoQ **product source is fixed at [`08188eba`](https://github.com/rydolabs/citoq/commit/08188eba73284dd1069016174ad216f5e02d2cfa)**; benchmark harness changes are separate.

### Public API: application-level transport measurements

Two independent runs per engine and network profile; 256-byte messages, 200 sequential samples per run at 100 Hz. The baseline is unshaped; the impaired profile applies **10 ms ± 2 ms delay and 1% packet loss in each direction**. All 16 tested runs across four engines received **200/200** unique messages.

| Tested application stack | Baseline p95 latency, ms (two runs) | Impaired p95 latency, ms (two runs) | Baseline bridge ingress, mean bytes/run |
| --- | ---: | ---: | ---: |
| **CitoQ** public Python `Context` + native **mTLS/QUIC** | 10.681 / 10.649 | 22.171 / 22.330 | 149,822 |
| **Cyclone DDS** Python, RELIABLE, UDP | 0.668 / 0.665 | 12.492 / 12.530 | 85,962 |
| **Zenoh** Python, peer mode, TCP | 0.186 / 0.174 | 12.142 / 12.038 | 75,104 |
| **Fast DDS** native C++, RELIABLE, UDPv4-only | 0.605 / 0.565 | 12.170 / **2,591.250** | 107,107 |
| **gRPC + Protocol Buffers** | **Not measured** | **Not measured** | **Not measured** |

The Fast DDS impaired-profile outlier (**2.59 s p95**) is retained, not discarded. These numbers are **not apples-to-apples**: encryption, reliability/transport, Python vs C++ bindings, serialization and internal polling differ. They must **not** be interpreted as intrinsic protocol latency or a vendor performance ranking. CitoQ's public mTLS/QUIC path in this experiment is substantially slower than the other tested stacks in the unshaped profile; its experimental internal UDP path is **not** a substitute for these product measurements. The two repetitions are insufficient for robust p99 or production performance claims.

**Reproducible evidence:** [successful public-API workflow](https://github.com/rydolabs/citoq/actions/runs/40242491625) · [raw per-process logs, CSV and JSON](https://github.com/rydolabs/citoq/actions/runs/40242491625/artifacts/12617496490) · [full methodology and limitations](https://github.com/rydolabs/citoq/blob/1d0a6d53fce13a50121ca35c7a377ebd3bbac309/docs/BENCHMARK_RESULTS_2026-10-26.md).

### Discovery approaches — architectural comparison, **not measured discovery speed**

| System | How peers/services are found | Status of comparable discovery timing and control-plane bytes |
| --- | --- | --- |
| **CitoQ** | Demand-driven/selective interest routing is the project design. Automatic same-host SHM routing is still an integration milestone. | **Not yet measured** on the pinned public SDK |
| **Cyclone DDS** | DDSI **SPDP** participant discovery + **SEDP** endpoint discovery; multicast and configured unicast peers | **Not yet measured** in a like-for-like discovery-only test |
| **Zenoh** | Peer/router **multicast and gossip scouting**; supports peer-to-peer and client/router topologies | **Not yet measured** in a like-for-like discovery-only test |
| **gRPC + Protobuf** | gRPC normally uses **DNS** name resolution, or configured resolvers such as xDS/custom implementations; Protobuf is a serialization format, not a discovery system | **Not measured**; discovery backend must be specified |

References: [Cyclone DDS discovery](https://cyclonedds.io/docs/cyclonedds/latest/about_dds/discovery_participants.html), [Zenoh discovery and deployment](https://zenoh.io/docs/getting-started/deployment/), [gRPC name resolution](https://grpc.io/docs/guides/custom-name-resolution/), [CitoQ SHM integration status](docs/SHM_V1.md).

<details>
<summary>Historical selective-subscription network traffic (Cito UDP research prototype vs Cyclone DDS — not the current CitoQ SDK)</summary>

An **earlier experimental Cito UDP runtime**, distinct from the pinned public CitoQ API, was tested at **30 nodes / 33 topics / 256-byte payload / five repetitions**, with full expected delivery for both systems. Mean total veth RX, including **discovery, control and user data**, was:

| Subscribed topics | Historical Cito prototype | Cyclone DDS BEST_EFFORT |
| --- | ---: | ---: |
| ~10% (4/33) | 0.378 MB | 6.920 MB |
| ~50% (17/33) | 1.421 MB | 15.502 MB |
| 100% (33/33) | 2.702 MB | 29.809 MB |

This does **not** isolate discovery traffic, is **not** a measured CitoQ SDK speedup, and contains no Zenoh/gRPC comparison. Historical runner evidence: [GitHub Actions #39858742770](https://github.com/nssang00/chatgpt-write-test/actions/runs/39858742770), [#39859684030](https://github.com/nssang00/chatgpt-write-test/actions/runs/39859684030).

</details>

**Next comparison requirements:** implement a genuine gRPC+Protobuf baseline (specify unary versus streaming, service discovery backend, payload and TLS), measure cold-start discovery convergence separately from data-plane bytes, normalize security and QoS, and repeat at 10/30/100+ nodes with enough samples for p99. Until then, no four-way discovery-efficiency ranking is supported.


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
