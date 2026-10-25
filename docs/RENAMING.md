# Cito to CitoQ: pre-release breaking rename

The 2026-10-25 full rename applies to code and distribution identities, not only the repository title:

| Former | Current |
| --- | --- |
| `#include <cito/cito.hpp>` | `#include <citoq/citoq.hpp>` |
| `cito::Context` | `citoq::Context` |
| `#include <cito/cito_c.h>` | `#include <citoq/citoq_c.h>` |
| `cito_context_create`, `CITO_C_ABI_VERSION` | `citoq_context_create`, `CITOQ_C_ABI_VERSION` |
| `libcito.so`, `libcito_core.so` | `libcitoq.so`, `libcitoq_core.so` |
| `import cito`, `CITO_LIBRARY` | `import citoq`, `CITOQ_LIBRARY` |
| `tools/cito_idlc.py` | `tools/citoq_idlc.py` (installed as `citoq-idlc`) |
| `CITO_ENABLE_MSQUIC` | `CITOQ_ENABLE_MSQUIC` |
| `cito_*` CMake internals | `citoq_*` (exported `CitoQ::*` targets unchanged) |

**Compatibility:** old C symbol names and installed header paths are no longer exported by default. Rebuild consumers and bindings. This is not an ABI-preserving patch or a final semantic-versioned release. The existing wire-format numeric constants, magic-byte values and packet layouts were not explicitly migrated as part of identifier renaming; inter-version wire compatibility must be tested before mixing deployments.

**Validation gates:** full CTest on Linux, installed C/C++ CMake consumers, Python import and native ABI access, IDL generation, and separately configured MsQuic network/cross-language tests. Passing the default MsQuic-off CI is necessary but not sufficient for a release.
