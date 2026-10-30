# CitoQ native QUIC stabilization: diagnostic gate

## Scope and baseline
This branch is **diagnostic-only** for the native QUIC/mTLS data-plane.
The CitoQ+ optional gossip feature is explicitly disabled in the build.
The pre-existing default C ABI lease was `700 ms` and the announce period
was `100 ms` in `src/native_runtime_c.cpp`; changing
`PosixUdpControlManager` defaults did not change that native configuration.

The existing `citoq_native_options_t` ABI stays unchanged.
Runtime environment options:
- `CITOQ_NATIVE_LEASE_MS`: integer 100..60000, default 700
- `CITOQ_NATIVE_ANNOUNCE_MS`: integer 20..10000, default 100
- `CITOQ_NATIVE_DIAGNOSTICS=1`: emit diagnostic logs

Reject a lease shorter than two announcement periods. Logs include
`CITOQ_DIAG_CONFIG` to prove the requested value reached the native process.

## Screening test (on GitHub-hosted Ubuntu 24.04)
Workflow: `.github/workflows/benchmark-citoq-stabilization.yml`.

- 100 and 200 Linux network namespaces, veth interfaces and one Linux bridge.
- Each namespace egress gets 10 +/- 2 ms `tc netem` delay, plus 0%, 1%
  or 2% random loss. This is **not** 100/200 independent machines.
- Lease: 700, 1500, 3000 ms; announce interval: fixed 100 ms.
- Two independent trials per matrix cell (screening, **not** statistical
  proof): 2 sizes x 3 loss rates x 3 leases x 2 repeats = 36 trials.
- 1 publisher, N-2 positive-interest subscribers, 1 negative-interest
  subscriber. Only N-2 nodes are expected to receive data.
- Public `Context` API and native QUIC/mTLS, 8 x 256-byte data payloads
  per positive subscriber, plus repeated discovery probe messages.
- Success requires matched exactly N-2 subscribers; all N-2 recipients
  receive their 8 unique data sequences without duplicates or invalid frames;
  negative-interest subscriber receives zero; all worker exits successful.
- 200-node timeout is long; a process timeout is an **end-to-end FAIL**, not
  proof that Discovery failed. Publisher readiness is collected separately.
- Evidence includes per-node logs, netem qdisc confirmation, runner stats,
  CITOQ_DIAG events and trial JSON with timeout diagnostics.

## Hypotheses to test
H1: transient CPU scheduling or control packet loss causes coordinator lease
expiration and route withdrawal. Compare lease expiration counts and route
stability vs. lease length; check detection latency tradeoff.

H2: discovery returns N-2 matches, but a QUIC connection has closed between
data-plane queue acceptance and actual `send_packet()`. Look for
`ASYNC_SEND_DROPPED_NOT_CONNECTED` and `TRANSPORT_FAILED` events.

H3: reconnect storms or event queue pressure affect 200-node outcomes.
Look for repeated CONNECT_BEGIN/CLOSED cycles and event queue high-water alerts.

These are hypotheses only; the prior failures do not identify the root cause.

## Important benchmark confound: discovery probes are actual data sends
The current `public_discovery_node.py` checks the number of routes by calling
`ctx.publish_raw` with a probe payload every 25 ms. This is **not** a passive
Discovery query: each probe may create QUIC stream and delivery load on every
already-matched subscriber, particularly for late convergence at 200 nodes.
A long discovery window can generate very many probe sends, and is not directly
comparable to A+'s membership-only discovery timer. Treat current failures as
end-to-end results **under probe stress**, not proof of a pure Discovery defect.

Before interpreting performance rankings, add a non-sending matched-destination
count query or run a separate low-frequency-probe cohort. Log the total probe
count and compare 25ms vs a sparse interval at identical network conditions,
especially 200 nodes at 1-2% loss.

## Next safety gates (after screening)
1. Isolate lease effect by preserving the same binary, announce interval and
   netem across cohort conditions. Report passes, discovery ready latency,
   delivery completeness, CPU and wire traffic. Do not compare A+ bridge TX
   with CitoQ veth RX numerically.
2. Inject an *actual* network interruption (link down/up or `tc` 100% loss),
   and a node restart while preserving a live DemandKey. Measure reconnect
   attempts, time to N-2 matching, and time to first delivered data after repair.
3. Distinguish queue acceptance, MsQuic stream send completion and *receiver*
   application delivery. QUIC `SEND_COMPLETE` does **not** prove the recipient
   application processed the message.
4. Only after diagnosis, implement bounded reconnect backoff/handshake limit,
   bounded send queue, and (if reliable delivery is required) delivery ACK +
   bounded retransmission + deduplication, behind an explicit QoS mode.
5. Repeat at least 10 independent trials per condition, report median,
   95th-percentile and worst-case outcomes, and add reconnect/soak/leak tests.

## Limitations
The A+ multicast/NACK prototype and the CitoQ demand-indexed QUIC system
have different discovery and delivery semantics. Do not infer a protocol
ranking from isolated PASS counts. First-stage diagnosis does not introduce
application-level guaranteed delivery. Do not merge this experiment without
regression, security, and fault tests.
