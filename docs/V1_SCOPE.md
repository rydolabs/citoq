# Cito v1 Scope

## Product target

Cito v1 targets deployments up to **200 hosts** while preserving the simple application model:

```cpp
cito::Context ctx;
ctx.on<Position>(handler);
ctx.publish(position);
ctx.run();
```

The v1 product scope includes:

- C++ typed API
- OMG IDL subset and generated direct codecs
- demand-indexed discovery
- same-host SHM
- LAN UDP BestEffort and bounded reliability
- stable C ABI
- Python binding
- QUIC secure remote transport
- Linux and Windows implementation compatibility

## Scale target

The architecture is designed around 200 hosts, not 200 Linux namespaces in CI.

Deterministic tests maintain at least 200 coordinator summaries and validate exact DemandKey-to-candidate-host lookup, replacement, and removal. Real namespace tests use smaller topologies for network correctness. Absolute p99/throughput claims require a later physical multi-machine lab.

## Language boundary

The stable cross-language boundary is the C ABI. C++, Python, and future language bindings must share the same Context semantics, type identity, discovery, wire format, and transports rather than implementing separate runtimes.

Current foundation:

- C ABI v1 opaque Context/Subscription handles
- explicit ownership and status codes
- raw `type_id + resource + bytes` publish/subscribe
- Python `ctypes` binding over that ABI

v1 completion requires the C ABI/Python Context to use the same distributed runtime underneath as C++ and a generated typed Python IDL path that produces/consumes the canonical Cito wire.

## QUIC policy

Transport choice stays internal.

```text
same process                 -> in-process
same host                    -> SHM
remote ordinary LAN          -> UDP
secure remote BestEffort     -> QUIC DATAGRAM
secure remote Reliable       -> QUIC stream
```

MsQuic is the first backend. A real Linux namespace smoke already validates one Cito packet over both a reliable stream and a QUIC DATAGRAM.

MsQuic callbacks are transport-completion callbacks only. Cito's mutable discovery/routing/runtime state remains owned by the Cito event loop.

v1 completion requires this backend to be selected through the normal distributed Context transport path rather than only through the standalone integration smoke.

## Definition of v1 complete

The release is not declared complete merely because the component prototypes exist. All of the following must be green:

- 200-host demand/index structural scale regression
- C++ local/SHM/UDP distributed Context flow
- C ABI using that same runtime
- Python using that same runtime
- generated Python IDL type round-trip compatible with generated C++
- BestEffort no-interest zero-work and encode-once invariants
- bounded reliable UDP recovery/failure tests
- QUIC DATAGRAM and stream selected through the runtime
- coordinator restart/lease/stale-route recovery
- Linux regression, sanitizer, SHM and namespace integration
- Windows build/runtime compatibility for C ABI and transport backends
- documentation of stable public API, ABI and wire-version policy

Performance claims additionally require measured p50/p95/p99 benchmarks. Structural tests alone are not marketed as production latency results.

## Current milestone

The current v1 integration baseline is green:

- 200-host structural demand-index scale gate
- canonical platform-independent TypeId/SchemaHash numeric hashing
- C++ and Python IDL generation from the same schema model
- byte-for-byte generated C++ <-> generated Python CTO1 wire compatibility
- stable C ABI v1 opaque Context/Subscription boundary
- simple typed Python Context API over the C ABI
- no-interest typed Python publish skips encoding
- shared HostRuntimeCore with exact local/remote DemandKey routing
- transport-independent DiscoveryControlCore
- portable UdpRuntimeAdapter with encode-once, non-owning data fan-out
- real three-node namespace selective delivery through the reusable UDP runtime
- coordinator lease expiry cleans remote routes and UDP endpoint cache
- multiple C/Python Contexts in one process aggregate on one weak-lifetime embedded host runtime
- reentrant Context/app destruction covered under ASan/UBSan
- same-host SHM multi-process regressions
- actual MsQuic stream and DATAGRAM namespace smoke

The next integration sequence is:

1. move the embedded runtime registry into one compiled shared core so C++ executables and the C ABI shared library cannot accidentally own separate host runtimes across a DSO boundary;
2. attach IDL-generated C++ Contexts to that shared host core while preserving object-native same-Context delivery and no-interest zero-encode;
3. extend the host coordinator contract across processes using local IPC for control only, keeping normal data direct/SHM;
4. attach the reusable UDP adapter to the public Context event loop;
5. attach QUIC session routing to the same discovery/runtime state and select DATAGRAM vs stream internally;
6. integrate bounded reliable UDP recovery into the public delivery path;
7. add Windows C ABI/runtime/QUIC/SHM compatibility gates.

v1 is complete only when those public-runtime integration gates, Windows compatibility, and the definition above are all green.


## Public API convergence milestone

The public vocabulary is fixed around Publish/Subscribe
(`publish`/`subscribe`) and planned Request/Reply
(`request`/`handle`). The embedded host runtime registry now belongs in one
compiled shared core so C++, C ABI and Python cannot accidentally own separate
process runtimes across a DSO boundary. IDL-generated direct-codec C++ Contexts
are attached to that shared runtime while preserving same-Context object-native
delivery and zero serialization for self-only demand.


## Request/Reply public baseline

The type-first Request/Reply baseline now includes:

- `request RequestType returns ReplyType;` generated relation metadata;
- C++ `ctx.request()` / `ctx.handle()` on the shared process runtime;
- stable C ABI request/handler/reply/cancel primitives;
- Python `await ctx.request()` / `ctx.handle()`;
- exact scope/resource/request-type matching;
- deterministic first-registered local handler selection;
- no-handler typed C++/Python request fast path with zero serialization;
- RequestId correlation, cancellation, late-reply rejection and handler-app
  failure cleanup.

This does not yet claim remote Request/Reply. Same-host SHM and remote QUIC
routing must be attached to the same RequestReplyCore and pass real namespace
gates before that claim is made.


### Request handler discovery identity

Request/Reply handlers now publish a deterministic internal handler-capability
DemandKey through the same HostRuntimeCore summaries used by Publish/Subscribe.
The capability TypeId is distinct from the raw request TypeId, so a normal
subscriber to a request message is not treated as an RPC handler. Unit
regression covers selective remote candidate/route discovery for this
capability; remote request payload/reply transport is still a later gate.


## Remote Request/Reply transport baseline

The transport-independent remote Request/Reply layer now uses a versioned
`CTX1` request/reply/cancel envelope. Correlation is protected by expected
outbound DestinationId and inbound peer/session + RequestId identity, so two
clients may safely use the same local RequestId.

A real two-node MsQuic smoke sends a CTX1 request on a bidirectional QUIC
stream, dispatches it through RequestReplyCore on the server loop, and returns
the typed-byte reply on the same stream. MsQuic receive callbacks post bytes
to the loop rather than owning request/discovery state.

This is a transport baseline, not yet a claim that public Context performs
remote Request/Reply automatically. The remaining public-path work is to attach
discovery route readiness, QUIC session management, deadlines, and Context
request lifecycle to the same loop/runtime.


## Public C++ remote Request/Reply bridge

The C++ Context request path now prefers an exact same-process handler and,
when none exists, can use a discovered request-handler route through
RequestNetworkAdapter. The adapter selects a ready reliable session
deterministically, keeps no-ready-session requests at encode zero, and routes
CTX1 replies back into the same RequestReplyCore correlation state.

Cumulative regression covers cancellation, Context destruction, session loss,
peer loss, late replies, and local-handler priority. Concrete automatic MsQuic
session creation is not yet attached to the public Context; the current
two-node MsQuic CTX1 smoke and public in-memory bridge are separate validated
layers.


## C ABI remote Request/Reply correlation

The stable C ABI now shares the RequestNetworkAdapter path used by C++
Request/Reply. `cito_context_has_handler` reports an exact local handler or a
ready discovered remote handler session; `cito_context_request` is
local-first and otherwise uses CTX1 remote correlation.

Remote inbound requests allocate a process-local handler RequestId rather than
exposing the peer's wire RequestId. This allows C and Python handlers to call
`cito_context_reply` safely when multiple peers reuse the same wire
correlation value. Cancel, peer loss, handler-app teardown, explicit request
destruction, and raw C Context destruction all clean up the corresponding
local/transport state. Raw C Context destruction preserves the existing rule
that requester callbacks are suppressed during teardown.


## CTR5 transport-aware route endpoints

Route discovery now has a backward-readable CTR5 endpoint format that
advertises separate UDP data and QUIC ports while retaining CTR4 decoding.
The endpoint footprint stays 12 bytes. UdpRuntimeAdapter caches both transport
endpoints and removes both through the existing route/lease lifecycle.

RequestSessionLifecycleCore consumes exact request-handler routes and QUIC
endpoint metadata to emit loop-owned Connect/Disconnect actions, including
endpoint changes, connect retry, handler withdrawal, and route expiry. Actual
MsQuic session objects remain the next concrete attachment layer.


## Concrete MsQuic Request/Reply session attachment

The optional `cito_msquic` transport now contains a reusable
`MsQuicRequestSessionManager`. It consumes RequestSessionLifecycleCore
Connect/Disconnect actions, owns concrete MsQuic connections/streams, and
attaches ready DestinationIds to RequestNetworkAdapter.

MsQuic callbacks queue bytes/completions; HostRuntimeCore, RequestReplyCore,
discovery, and session attachment state are mutated only from the manager's
loop-owner `poll()/reconcile()` path.

The QUIC workflow now includes a real two-process public Context smoke where
the client learns a CTR5 QUIC route, the manager creates the session, and
`Context::request()` reaches a remote `Context::handle()` and receives the
typed reply.

The public C++ remote path now also supports a pending-first-request model:
when the concrete session manager is installed, the first typed request can
arrive before discovery/session readiness and remains unencoded until an exact
session is ready. `RequestOptions.timeout` defaults to 5 seconds and covers
both readiness waiting and remote reply waiting. Deadline-before-readiness is
encode-zero; deadline-after-send completes locally as `DeadlineExceeded` and
emits CTX1 Cancel best-effort.

The remaining integration is event-loop ownership in normal `Context::run()`
plus the real UDP discovery/control socket path, so applications no longer
drive `poll()/reconcile()` or route learning explicitly.


## Real UDP discovery to QUIC Request/Reply

RequestSessionLifecycleCore now expresses handler-route demand through
UdpRuntimeAdapter as soon as a request key becomes desired. Route desire may
therefore precede coordinator discovery; a later matching snapshot
automatically emits CTR3 without a second application-side ensure call.

The POSIX runtime has a reusable nonblocking UDP control manager that pumps
CTS2/CTQ1/CTB1/CTR3/CTR5 messages and lease expiry from the loop owner. The
public QUIC namespace smoke now uses that real UDP control exchange instead of
in-process route seeding, then creates the MsQuic session and completes the
original pre-discovery `Context::request()`.

The remaining integration is to make `Context::run()` own the UDP control and
QUIC poll/reconcile cycle automatically, and to provide the corresponding
Windows control-socket backend.


## Context-owned Linux Request/Reply loop

The embedded host runtime now accepts one internal RuntimeDriver.
`Context::run()` delegates to it and `Context::stop()` wakes/stops it.
Without a driver, local Context run remains a no-op for compatibility.

The Linux Request/Reply driver composes the real POSIX UDP control manager,
RequestSessionLifecycleCore, MsQuic session manager, and request deadline
progression. It blocks on the UDP fd plus an MsQuic wake pipe and computes its
timeout from the next request/announcement deadline, with only a bounded lease
maintenance ceiling.

The real two-node public QUIC smoke now uses `Context::run()` rather than
manually calling control/quic poll or reconcile functions. The remaining work
is automatic platform bootstrap/configuration and the equivalent Windows
driver/backend.
