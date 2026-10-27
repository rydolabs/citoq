# Review of middleware_core_design.md for CitoQ

Date: 2026-10-27. Source is the user-supplied early design proposal
`middleware_core_design.md` (sections 1–9). It is an architectural reference,
not an official interoperable Zenoh or NATS protocol requirement.

## Decisions implemented now

| Proposal | CitoQ implementation | Invariant |
| --- | --- | --- |
| `on_request` is an explicit scoped handler registration (3.2) | New optional `Context::on_request<T>(resource, handler)` returns `ScopedHandlerRegistration`. It unregisters on destruction, move replacement and explicit close; the existing `Context::handle<T>()` is unchanged and Context-owned | No public API break; handler interest removed with registration; move source disarmed |
| A timed-out/retried request may execute twice (5) | `RequestTransportCore` now keeps up to 1024 (transport peer ID, CTX1 RequestId) tombstones and denies duplicate in-session replays even **after** a response or cancellation | No handler double-dispatch for an ID still in the window, bounded RAM; replay responds `InvalidArgument` |
| Node identity and authorization separated (5) | `AccessPolicy` exact principal/operation/DemandKey evaluator, default-deny, capacity-bound. Optional `RequestTransportCore::set_request_authorizer()` rejects inbound remote requests with `Unavailable` if the callback denies or throws | No callback = legacy behavior, not a security claim. The connection peer ID is **not** a verified principal. |
| Distinct operations `publish / subscribe / request / handle` (1, 5) | `AccessOperation` includes all four, but automatic enforcement is currently implemented **only for the optional inbound Request callback** | Do not claim complete resource ACL until identity/operation enforcement is wired to every ingress/egress path |

The replay guard is a bounded, per-session **duplicate prevention tool, not exactly-once**: no durable identity, reply-cache or cross-reconnect `IdempotencyKey`. An actual idempotency contract must define peer principal, command identity, TTL/capacity, concurrent retry policy, response cache semantics and crash recovery. Separate negative network tests must verify identity spoofing, certificate rotation, revocation and ACL bypass before announcing a security feature.

## Already implemented in main before this PR

- Shared CitoQ messaging runtime with Pub/Sub and Request/Reply, `Context::publish/subscribe/request/handle`, request correlation, timeouts and cancellation.
- Demand-indexed, exact `Scope + Resource + Type` routing; Host Coordinator serves control/discovery rather than mandatory data-plane brokering.
- C++/Python API over stable C ABI, IDL direct static codec and canonical CitoQ tagged wire.
- Native MsQuic mTLS remote transport and sample-to-public-API tests, but production release readiness is **not** established.

References: `docs/ARCHITECTURE_CONSTITUTION.md`, `docs/PUBLIC_API.md`, `docs/RELEASE_STATUS.md`, `docs/SHM_V1.md`, `docs/REQUEST_REPLAY_GUARD.md`.

## Deferred or deliberately NOT adopted

1. **Verified principal and complete operation ACL.** Must bind a validated mTLS certificate identity to the runtime peer/session before consuming `AccessPolicy` results, configure mandatory enforcement, and handle revocation/reconnect. Current optional request callback alone is not a production security boundary.
2. **End-to-end idempotency.** Requires explicit caller-provided stable key and retry semantics across reconnect; cannot silently infer from ephemeral CTX1 RequestId. Needs error/response contract and persistence decision.
3. **Router/P2P/Hybrid.** The proposal's Router-first path is not retroactively substituted for CitoQ's Coordinator + direct-data path. Add an optional Router as an independently versioned discovery/data bridge with cycle and duplication protection when topology warrants it.
4. **Wildcard subjects.** The proposal's `robot/*/position` matching is incompatible with the existing exact opaque Resource semantics as a default. If needed, keep it a separately indexed optional subscription layer.
5. **Bridge and codec metadata.** ROS 2/DDS, MAVLink, gRPC/Protobuf adapters must stay outside the native Core. Do not rename CTO1/CTX1 to Protobuf or assert external wire compatibility. Add versioned codec/schema metadata in each adapter only when exercised by real consumer tests.
6. **`request_all`, streaming, SHM automatic selection.** Track as separate bounded, tested features; SHM ring exists but automatic cross-process public Context routing is still deferred.

## Tests and safety gates

- `tests/request_api_test.cpp`: scoped `on_request` lifetime, request result, move/disarm, replacement cleanup, repeated close and dropped temporary.
- `tests/request_transport_test.cpp`: completed/cancelled/in-flight replay, separate sessions, bounded eviction, peer cleanup and an optional fail-closed request-only authorizer.
- `tests/access_policy_test.cpp`: deny-by-default, exact principal/operation/key checks, bounded grants, revoke and empty principal handling.
- Existing C ABI, request network, Python and native authenticated QUIC tests remain unchanged.

Source CI: [GitHub Actions run #40475869644](https://github.com/rydolabs/citoq/actions/runs/40475869644) — **68/68 CTest passed**; [G1 native mTLS](https://github.com/rydolabs/citoq/actions/runs/40475869633) and [G4 Python native mTLS](https://github.com/rydolabs/citoq/actions/runs/40475869634) also **passed**. The base `main` source at the start of this work was `08188eba73284dd1069016174ad216f5e02d2cfa`. All changes are in a separate PR and remain unmerged until reviewed.
