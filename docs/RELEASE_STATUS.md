# Release readiness

**NOT RELEASE READY — migration staging (2026-10-25).**

Curated source includes: C++20 runtime, C shared API and Python sources, IDL generator, selected specifications, and CMake/CTest regression cases. `CITO_ENABLE_MSQUIC=OFF` is the minimal source-build profile.

Release blockers:
1. Pass the new repository's clean Linux CI including out-of-tree installed consumer test.
2. Validate MsQuic/mTLS identity-to-node/ACL policy, certificate lifecycle and negative security cases.
3. Validate reconnect after node/network restart, QoS and stress/fault semantics.
4. Expand ABI, C/Python cross-process interoperability, sanitizer and lifecycle certification.
5. Complete CitoQ public ABI/Python/native library naming plan and compatibility checks.
6. Choose and publish an appropriate license; define supported platforms and package policy.

The former development G1–G5 PR is draft. Passing a few green isolated CI runs does not establish product readiness. This distribution is not the entire historical Cito or Round 7 development tree. Experimental ROS 2 bridge and incomplete `rmw_cito` are not part of the current distribution profile.
