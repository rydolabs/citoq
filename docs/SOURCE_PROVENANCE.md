# Source provenance

The initial CitoQ source is extracted from `nssang00/chatgpt-write-test/cito/` at commit `037e860ab21ddbed9837e1c68262105dcc74d5c3` ([original PR #25](https://github.com/nssang00/chatgpt-write-test/pull/25), draft).

Kept: original runtime and public headers, C/Python binding sources, IDL code-generation tool, regression tests, and selected architecture/user-facing specifications.

Not imported: unrelated NativeWeb parent project, old handoff logs and reports, development-specific workflows that assume `cito/` under the repo root, experimental ROS 2 bridge, build outputs or certificates. The offline cumulative Round 7 source is **not** automatically overlaid onto this verified source snapshot.

Migration-only changes: CitoQ project name, CMake packaging/install/export targets, C++/C/Python compatibility facades, release-prep CI, and distribution-facing documentation. The Cito ABI, C++ implementation namespace, artifact names and wire protocol are deliberately not rewritten as part of this initial migration.

## Full CitoQ rename (2026-10-25)

After the initial curated import, the pre-release public API was renamed from the old `cito` identifier to `citoq` throughout headers, implementation, CMake targets, C ABI, Python module and native library names, tests and IDL generator. Compatibility wrappers were removed to avoid two supported entry-point names. See [RENAMING.md](RENAMING.md).

**Historical source references above intentionally retain the original Cito path and commit SHA.** This is not a claim that the renamed ABI can load binaries or Python wheels built against the former snapshot. Packet framing and numeric wire-format identifiers were not deliberately version-bumped by the naming change; binary/wire interoperability with the older deployment still needs explicit tests.
