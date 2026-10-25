# CitoQ

**CitoQ** is a typed messaging middleware with demand-driven discovery, Publish/Subscribe and Request/Reply APIs.

> **Pre-release development source. Not production release ready.** The API and packaging were fully renamed from the former Cito project. No stable ABI or wire-protocol compatibility with the earlier development snapshots is promised.

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
