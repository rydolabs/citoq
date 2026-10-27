# CTX1 per-session RequestId replay guard (2026-10-27)

## Why this is needed

The middleware design proposal describes three distinct facts: request/reply correlation, cancellation and timeouts belong to the Core; a request timeout does **not** prove that the handler did not execute; and retrying side-effectful operations may execute them more than once. CitoQ already implements the former, but previously its `RequestTransportCore` rejected duplicate `peer + RequestId` only **while that request was in flight**. When the handler replied, that entry was erased, and repeating the exact frame on the same peer could dispatch a side-effecting handler a second time.

## Implemented in the current feature branch

- Added `detail::RecentRequestIds`: an owner-loop-only, bounded FIFO set of (transport peer, CTX1 RequestId), default window **1024**, no cache of secrets, payloads or responses.
- Integrated it into the existing `RequestTransportCore::on_request()` path. Replays during and after completion and after cancellation get the existing `RequestStatus::InvalidArgument` reply, with **no handler invocation**.
- Oldest records are evicted at capacity; records for a closed peer/session are removed by `drop_peer()`. Separate live sessions can use the same RequestId safely.
- No C++ public Context signature, wire layout, QUIC security policy, or Request/Reply correlation format changes. The default may cost up to 1024 lightweight tombstones per RequestTransportCore instance; the constructor accepts an override for embedders.
- Regression tests cover completed replay, different peer, 2-entry eviction, in-flight duplicate, cancelled replay, peer cleanup and zero-sized window.

## Security and exactly-once limits

**This is not an application idempotency implementation or end-to-end exactly-once delivery.** MsQuic assigns a fresh `RequestPeerId` to each accepted connection; after a reconnect, a repeated *business command* is a new peer session and cannot be recognized by this cache. IDs eventually age out of the bounded window; process restart also loses tombstones. `InvalidArgument` signals rejected duplicate and does **not** replay the original successful result. The replay cache does not authenticate clients or enforce resource permissions.

A future opt-in `IdempotencyKey` must be stable across client retries, scoped by an **authenticated principal and business resource**, with a carefully chosen TTL, in-progress state, durable-or-bounded outcome policy, and explicit non-idempotent operation semantics. Such behavior requires a versioned contract and rigorous failure-injection tests before shipping.

## Further design proposals intentionally not mixed into this patch

- **ACL**: production resource-level authorization cannot safely be provided by trusting the connection-local numeric peer ID. First bind validated certificate identity to the runtime principal and define default-deny rules for publish/subscribe/request/handle; include negative security and identity-rotation tests. No ACL is claimed here.
- **Gateway/codec**: keep native CTO1/CTX1; describe codec/schema identity only at adapters for DDS/ROS2/MAVLink/gRPC. These integrations remain outside native Core.
- **Router/hybrid, wildcard subscriptions, request_all**: defer until specific application need; current exact `Scope + Resource + Type` demand-index performance remains a core product design constraint.

Source reference: user-provided `middleware_core_design.md`, sections 3.2/3.3 and 5 (request correlation, cancellation, idempotency). CitoQ source: `include/citoq/detail/request_transport.hpp`, `include/citoq/detail/request_reply_core.hpp` and `src/msquic_request_session_manager.cpp`.

## API registration ergonomics from the reference design

A separate `Context::on_request<T>(resource, handler)` scoped/RAII alias was introduced. Its `ScopedHandlerRegistration` releases handler registration on scope exit and on replacement, while the existing `Context::handle<T>()` continues to belong to Context lifetime. No existing API behavior is changed. The scoped variant is especially useful for temporary gateways and service lifetimes: forgetting to keep the handle deliberately unregisters immediately. It is *not* the same as an application idempotency policy.
