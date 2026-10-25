# CitoQ

CitoQ is a typed communication middleware built around demand-oriented discovery, Publish/Subscribe and Request/Reply.

> **Release preparation — NOT RELEASE READY.** This repository is a curated source migration from Cito. It is not a production release or a claim of completed G1–G5 acceptance gates.

## Build (Linux)

Requirements: CMake 3.20+, C++20 compiler, Python 3, Threads and optionally Ninja.

```sh
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON -DCITO_ENABLE_MSQUIC=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$PWD/stage"
```

In a separate CMake project: `find_package(CitoQ CONFIG REQUIRED)` and `target_link_libraries(app PRIVATE CitoQ::Core)` with `CMAKE_PREFIX_PATH` set to the installed prefix. C consumers may use `CitoQ::C`.

## CitoQ API transition

The repository and CMake project are named CitoQ. The first import **intentionally preserves** the original `cito::` C++ namespace, `cito_*` C ABI symbols, `libcito.so`, on-wire identifiers and Python native internals. This avoids silently breaking compatibility while moving sources. A C++ transitional header `<citoq/citoq.hpp>` exposes the `citoq::` namespace alias. C consumers may include `<citoq/citoq_c.h>` (existing `cito_*` symbols). The `python/citoq` module reexports the original Python binding; native runtime installation or `CITO_LIBRARY` is required.

`CITO_ENABLE_MSQUIC=ON` requires separately installed MsQuic headers/libraries. QUIC/mTLS has partial integration evidence in the development repository but is **not production certified**. Experimental ROS 2 bridge code is deliberately excluded from this initial distribution snapshot.

## Project information

- [Release status and blockers](docs/RELEASE_STATUS.md)
- [Source provenance](docs/SOURCE_PROVENANCE.md)
- [Public API](docs/PUBLIC_API.md)
- [Architecture rules](docs/ARCHITECTURE_CONSTITUTION.md)
- [Testing](docs/TESTING.md)

[Original development PR #25](https://github.com/nssang00/chatgpt-write-test/pull/25). No final license has been selected; public access alone does not grant reuse rights.
