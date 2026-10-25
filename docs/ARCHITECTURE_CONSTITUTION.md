# Cito Architecture Constitution v1.0 (Draft)

## Product goal

Cito is a lightweight typed messaging runtime that lets applications publish and receive data without requiring users to manage peers, ports, transport selection, discovery machinery, or serialization calls.

## Three core product pillars

1. **Easy API** — the normal user should think in `Context`, `on`, `publish`, and `run`, not participants, endpoints, sockets, discovery protocols, or serializers.
2. **Demand-indexed discovery and delivery** — conceptually like using a GIS index instead of checking every road or destination: publishing asks the demand index for only the destinations that need `Scope + Resource + Type`, then sends only there.
3. **Single event-loop ownership + bounded worker pool** — like libuv/Chromium-style runtime architecture: one loop thread owns I/O and mutable runtime state; CPU-heavy or blocking work is offloaded only when needed, and completion returns to the loop.

The GIS comparison is an architectural analogy, not a geographic data model. Cito resources remain opaque identifiers.

## Non-negotiable principles

1. **Simple first. Powerful when needed. Complexity stays optional.**
2. The default mental model is `Context + publish + on + run`.
3. Advanced features extend the same model; they do not create a second programming model.
4. Scale changes runtime internals, not application API.
5. Discover demand, not every node or endpoint.
6. Do no unnecessary work: no interested destination means no serialization, buffer allocation, or network send.
7. When destinations exist, encode each required representation once and reuse it across matching destinations.
8. Users do not pay runtime cost for features they do not use.
9. Runtime mutable network/discovery state is single-owner on one event-loop thread by default.
10. Worker threads are bounded, lazy, and reserved for work that should not block the event loop.
11. All queues, histories, retries, and shared resources are bounded.
12. Transport semantics are uniform while mechanisms remain free to specialize: local/SHM, UDP, QUIC, or future transports.
13. A failed or slow application must not stop unrelated applications.
14. C++ is the initial core implementation language; cross-language boundaries use a stable C ABI.
15. Gateway/integration layers (DDS, ROS 2, MAVLink, legacy protocols) stay outside the native core.
16. Current-session/local verification comes first. GitHub Actions is used when independent network nodes, OS-specific behavior, or unavailable environments are required.
17. Every development phase requires unit tests, cumulative regression tests, and a minimal smoke test before it is considered complete.
18. The Host Coordinator is a control-plane aggregation/discovery component, not a mandatory data-plane broker.
19. Discovery mechanisms are replaceable backends; Cito's stable model is the DemandKey and demand index, not SWIM/multicast/rendezvous themselves.
20. Structural latency invariants are regression-tested; production static messages must eventually use a direct generated codec rather than DynamicData on the hot path.

## Public API baseline

```cpp
cito::Context ctx;
ctx.on<Position>(handler);
ctx.publish(Position{...});
ctx.run();
```

Optional isolation:

```cpp
cito::Context ctx("acme.robotics");
```

Optional opaque resource identifier:

```cpp
ctx.on<Position>("drone_17", handler);
ctx.publish("drone_17", Position{...});
```

`Resource` is an opaque UTF-8 identifier in v1. Cito assigns no hierarchy or REST-style semantics to its contents.

## Runtime execution invariant

The intended runtime flow is:

```text
socket / timer / SHM event
        |
        v
single event-loop thread
        |
        +-- discovery / interest state
        +-- routing decision
        +-- small fast-path work
        |
        +-- heavy or blocking work? --> bounded worker pool
                                      |
                                      v
                               completion posted
                                      |
                                      v
                              event-loop thread
```

The worker pool is not a second owner of discovery or transport state. It performs isolated work and posts results back.

## Control plane vs data plane

The intended host-level topology is:

```text
local applications
      |
      | local registrations / interest counts
      v
Host Coordinator
      |
      | versioned DemandKey summary / discovery control
      v
remote coordinators

normal data path:
publisher --------------------------> subscriber

same-host data path:
publisher ------------ SHM --------> subscriber

The SHM prototype is publisher-owned, one-writer/many-readers, bounded, and reader-nonblocking. Reader progress is not stored as shared writer-owned flow-control state; a slow reader drops overwritten samples rather than stalling the publisher.

Transport selection is internal and locality-first: same-process delivery uses direct callbacks, same-host cross-process delivery prefers SHM, and remote LAN delivery uses UDP. This choice must never require a different user-facing publish API.
```

The coordinator may later provide an optional host-ingress fan-out optimization when many local subscribers would otherwise cause redundant remote unicast. That optimization must be selected internally and must not become a mandatory hop.

A host demand summary changes externally only when a unique `Scope + Resource + Type` key appears for the first time or disappears after its last local subscriber.

Remote discovery is intentionally two-stage: versioned host summaries maintain `DemandKey -> candidate host` state, and direct endpoint details are exchanged only for matching demand. Stale summary versions and recently retired coordinator incarnations are ignored to prevent control-plane pull storms.

Same-process delivery follows the same demand-index principle: `Context` indexes callbacks by exact type and opaque resource rather than scanning unrelated handlers.

See `LATENCY_GUARDS.md` for the concrete hot-path invariants and `SHM_V1.md` for the same-host ring contract.

## Type direction

- Primary definition path: small OMG IDL data-type subset.
- Stable field IDs and compatible evolution are required design properties.
- Generated/static types are the normal fast path.
- `TypeBuilder` and `DynamicData` are optional expert/tooling/simulation APIs over the same canonical type model and wire semantics.
- Built-in text/bytes messages remain available for simple tools and simulation.
- Wire format must support evolution without transmitting field names on every message.

## Testing hierarchy

1. Current session: algorithms, codec/type logic, unit tests, regression tests, same-process smoke tests, simulations, sanitizers, and benchmarks where possible.
2. Same-host SHM: multiple real OS processes on one host/runner.
3. Independent Linux network nodes: Linux network namespaces + veth + bridge on one GitHub Actions Ubuntu runner.
4. Network fault testing: namespace-local `tc/netem` for loss, delay, reordering, link down/up, and recovery.
5. Windows Actions only for Windows-specific implementation and cross-platform compatibility.
6. Physical multi-machine tests are reserved for later real-network performance validation.

Detailed test gates are defined in `TESTING.md`.
