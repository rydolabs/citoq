# Source provenance

The initial CitoQ source is extracted from `nssang00/chatgpt-write-test/cito/` at commit `037e860ab21ddbed9837e1c68262105dcc74d5c3` ([original PR #25](https://github.com/nssang00/chatgpt-write-test/pull/25), draft).

Kept: original runtime and public headers, C/Python binding sources, IDL code-generation tool, regression tests, and selected architecture/user-facing specifications.

Not imported: unrelated NativeWeb parent project, old handoff logs and reports, development-specific workflows that assume `cito/` under the repo root, experimental ROS 2 bridge, build outputs or certificates. The offline cumulative Round 7 source is **not** automatically overlaid onto this verified source snapshot.

Migration-only changes: CitoQ project name, CMake packaging/install/export targets, C++/C/Python compatibility facades, release-prep CI, and distribution-facing documentation. The Cito ABI, C++ implementation namespace, artifact names and wire protocol are deliberately not rewritten as part of this initial migration.
