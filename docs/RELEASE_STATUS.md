# CitoQ release readiness

**NOT RELEASE READY — CitoQ full-rename integration (2026-10-25).**

The project uses CitoQ public names for C++ namespace and installed headers, C ABI functions and structs, shared libraries, Python package, IDL generator, tests and CMake targets. This is an intentional pre-release breaking rename.

Base Linux CMake/CTest coverage is a gate for this branch, but *does not* establish production readiness.

## Remaining product release blockers

1. Revalidate the optional MsQuic/mTLS network driver and cross-language remote Pub/Sub/RPC with renamed native symbols.
2. Finish TLS certificate identity-to-node/ACL policy, provisioning/revocation, and negative security tests.
3. Test reconnect after process/network failure and QoS/fault injection over network.
4. Expand ABI/thread/DSO lifecycle, sanitizers, Python packaging, and portability checks.
5. Decide license, supported-platform policy, artifact signing, release version/tagging.

The old G1–G5 PR is a draft development artifact. ROS 2 bridge and unfinished `rmw_citoq` are not part of the current distribution. The M26 Cyclone DDS benchmark is a separate development snapshot, not a benchmark of this renamed product branch.
