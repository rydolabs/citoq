# Cito Runtime Model v1 — Single Loop + Bounded Workers

## Goal

Keep the common path fast and predictable without exposing threading concepts in the normal Cito API.

Applications continue to use:

```cpp
cito::Context ctx;
ctx.on<Position>(handler);
ctx.publish(position);
ctx.run();
```

They do not create event loops, worker pools, network threads, or discovery threads.

## Ownership model

One event-loop thread owns mutable runtime state such as:

- discovery state
- interest index updates
- destination/route state
- transport I/O state
- timers and lease state
- callback/completion sequencing

This minimizes locks on the main runtime path and makes ordering easier to reason about.

## Worker pool

The worker pool is:

- bounded
- lazily started on first submitted work
- not an owner of discovery/transport state
- used only for isolated CPU-heavy or blocking operations
- followed by completion posting back to the event loop

Candidate work includes sufficiently heavy serialization/compression, certificate/crypto operations, file-backed tooling, and adapters that would otherwise block the loop. Small messages should stay on the fast path when offload overhead would cost more than the work itself.

The event loop must also avoid hidden O(N) housekeeping. Timer work such as lease expiration must use indexed due-work structures rather than periodically scanning every known peer/interest.

## Backpressure

Both event and worker queues are bounded. A full queue is an explicit condition, not a reason to grow memory without limit or spawn more threads.

The exact public backpressure/error policy is not frozen yet.

## Current prototype validation

Unit tests verify:

- event queue capacity is bounded
- event-loop callbacks execute on one loop thread
- worker threads start lazily
- worker queue capacity is bounded
- offloaded work runs off the event-loop thread
- completion returns to the event-loop thread

Smoke tests verify the complete loop -> worker -> loop completion path.

The runtime prototype is still internal under `cito/detail`. It is intentionally not yet exposed through the public API while `Context::run()` semantics are being stabilized.


## BestEffort locality fast path

The internal BestEffort delivery prototype separates object-native local delivery from encoded transports:

```text
matching targets
     |
     +-- same process -> callback(object), no serialization
     |
     +-- same host ---\
     |                 +-> lazy encode exactly once -> SHM
     +-- remote ------/                         \----> UDP
```

SHM and UDP currently share the same canonical Cito wire representation, so a mixed fan-out does not need one serialization per transport or destination. If a future transport requires a genuinely different representation, each required representation may be produced at most once per publish.

This is internal runtime policy; it does not add transport parameters to `Context::publish()`.
