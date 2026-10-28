# CitoQ vs Zenoh — authenticated QUIC parity and 1000Hz receive-path investigation

Date: 2026-10-28. Status: **benchmarks and diagnostics; no product main modification**.

## Engineering conclusion

1. The original CitoQ ~0.7ms vs Zenoh TCP ~0.2ms p95 ranking is **not a transport-normalized comparison**: CitoQ had authenticated mTLS/MsQuic QUIC and a receiver with an explicit 500µs polling sleep, while Zenoh ran non-TLS TCP with a background callback. CitoQ's earlier opt-in same-owner `poll_wait()` [PR #10](https://github.com/rydolabs/citoq/pull/10) removed most polling overhead without changing the Wire protocol.
2. On the same Ubuntu/veth runner with **both products using authenticated QUIC/mTLS**, CitoQ and Zenoh often show **overlapping 256B/100Hz application p95**. Neither is consistently faster across independent shared-runner executions. Zenoh TCP remains a different, generally lower-overhead, *not security-equivalent* baseline.
3. Some short 200-message, 1000Hz cohorts show CitoQ p95 ~1.0–1.4ms while Zenoh QUIC/mTLS is ~0.1ms. These are real accepted **short-run** measurements, but a matched **1000-message/1000Hz** CitoQ A/B measured baseline p95 ~0.16ms with no wake-coalescing gain. Consequently the claim that CitoQ is inherently an order of magnitude slower at sustainable 1000Hz is unsupported. Initial/burst vs steady-state measurement is required.
4. Existing default CitoQ P2P, Coordinator discovery, interest filtering, mTLS, Wire and Request/Reply design is **not shown to require replacement**. Do not enable single persistent stream, eight persistent shards, or priority flags based on the results here.

## Verified benchmark setup and limitations

- Same GitHub Actions Ubuntu 24.04 runner and two veth-linked Linux network namespaces; timestamps use a shared monotonic clock. **Not separate physical machines.**
- CitoQ `Context.publish_raw()`, common C ABI, native MsQuic authenticated QUIC, and owner-thread `NativeRuntime.poll_wait()` receive. Zenoh Python peer mode, `quic/` endpoint with client certificate, server certificate IP SAN verified against root CA, shared memory disabled, no TCP fallback.
- Security proof: after a valid client delivered 200/200, an independently generated client certificate signed by an untrusted CA was refused **0/20 delivered**, even though Zenoh `Publisher.put()` returned successfully without a connected subscriber. The negative-control CI correctly tests delivered data rather than sender process exit. [Passing CI #40605882550](https://github.com/rydolabs/citoq/actions/runs/40605882550). This does not constitute a complete TLS/mTLS security certification.
- All accepted 200-message cohorts require every unique sequence, no missing/duplicate/invalid payload, no sender/receiver process failure. Median of trial-local p95s is **not** pooled p95; 100/1000Hz are offered rates, not saturation throughput.
- Whole-process CPU includes initialization, handshake, producer and postreceive wait. Prior CitoQ harness waited up to ~1 extra second after completing 200 messages while Zenoh waited 80ms; the revised [benchmark PR #23](https://github.com/rydolabs/citoq/pull/23) normalizes CitoQ's duplicate grace to 80ms, so avoid direct old/new CPU comparisons without controlling for this fix.

## Same security protocol, matched QUIC/mTLS 256B/100Hz

| Source | CitoQ p95 ms | Zenoh QUIC/mTLS p95 ms | Zenoh non-TLS TCP p95 ms |
| --- | ---: | ---: | ---: |
| [CI #40605064240, original attempt](https://github.com/rydolabs/citoq/actions/runs/40605064240) | 0.161621 | 0.237596 | 0.152501 |
| [CI #40605064240, retry attempt 2](https://github.com/rydolabs/citoq/actions/runs/40605064240/attempts/2) | 0.117968 | 0.114237 | 0.080902 |
| [CI #40605373664](https://github.com/rydolabs/citoq/actions/runs/40605373664) | 0.136807 | 0.140301 | 0.086005 |
| [CI #40605395229](https://github.com/rydolabs/citoq/actions/runs/40605395229) | 0.259998 | 0.270266 | 0.150042 |
| [Latest positive+rogue-control CI #40605882550](https://github.com/rydolabs/citoq/actions/runs/40605882550) | 0.279881 | 0.278492 | 0.179617 |

Every row is a different or retried GitHub runner environment, not a pooled estimate. The last security-normalized run showed essentially **p95 parity**, but CitoQ process receiver CPU was ~0.17s versus Zenoh QUIC/mTLS ~0.10s for the 200-message test; this is whole-process CPU, not a per-message engine CPU metric.

### Security-normalized payload/rate/loss matrix

[Accepted full matrix #40605882566](https://github.com/rydolabs/citoq/actions/runs/40605882566): each engine/scenario 2 trials ×200 fully valid messages.

| Scenario | CitoQ p95 ms | Zenoh QUIC/mTLS p95 ms | Interpretation |
| --- | ---: | ---: | --- |
| 64B, 100Hz | 0.253014 | 0.249620 | Comparable |
| 256B, 100Hz | 0.223688 | 0.474505 | Runner variation; no universal winner |
| 4KiB, 100Hz | 0.247384 | 0.262092 | Comparable |
| 16KiB, 100Hz | 0.278155 | 0.268569 | Comparable p95; tails need checking |
| 256B, 1000Hz, 200 messages | **1.352939** | **0.124712** | Short-burst CitoQ tail regression |
| 256B 100Hz, 10ms±2ms & 1% loss | 11.927238 | 17.663331 | Accepted run only; not reliability proof |

A separate earlier [matrix run #40605613608](https://github.com/rydolabs/citoq/actions/runs/40605613608) had a CitoQ 1% loss trial that failed to establish a matched subscriber before sending; do not treat that failure as a successful latency result. A passing later 1% loss run does not remove the reconnect/discovery reliability concern.

## Native and Python stage timing

- [CI #40697409266](https://github.com/rydolabs/citoq/actions/runs/40597409266): synchronous MsQuic `StreamOpen+StreamStart` median **~7.6–8.6µs**, `StreamSend` **~0.6–0.8µs** at 100Hz.
- [Queue CI #40598294080](https://github.com/rydolabs/citoq/actions/runs/40598294080): sender owner-event queue median **~6–8µs**, receiver queue median **~25–28µs** at 100Hz.
- [1000Hz queue CI #40606414339](https://github.com/rydolabs/citoq/actions/runs/40606414339): 1000Hz receive queue p95 varied **~62.5–487.3µs**, while synchronous StreamStart/send remained a few µs.
- [FFI callback CI #40604918633](https://github.com/rydolabs/citoq/actions/runs/40604918633), [PR #22](https://github.com/rydolabs/citoq/pull/22): C ABI→ctypes callback median **~6.8–7.3µs**. Python byte copy **~1.7–2.0µs** and user handler **~2.6–2.8µs** are *nested* inside the ABI interval. All 12/12 accepted 200-message trials. Python FFI is measurable but not established as the dominant 1000Hz tail cause.

Per-message stderr in the MsQuic queue experiments may perturb the timing. Do not sum independent p95s into e2e p95.

## Attempted worker/queue optimizations

### Coalesce owner wake signals — potentially useful for short bursts, not yet release-ready

[PR #24](https://github.com/rydolabs/citoq/pull/24): optional compile flag `CITOQ_QUIC_WAKE_COALESCE=1` changes a `post()` wake from every item to empty→nonempty transitions, under the existing queue mutex. Default behavior and Wire remain unchanged.

Two independently executed [strict same-source A/B runs](https://github.com/rydolabs/citoq/actions/runs/40606254900), all 24/24 ×200-message trials accepted each:

| Condition | Baseline p95 attempt 1 | Candidate p95 attempt 1 | Baseline attempt 2 | Candidate attempt 2 |
| --- | ---: | ---: | ---: | ---: |
| 256B 100Hz | 0.258117 | 0.258788 | 0.355014 | 0.357499 |
| 256B 1000Hz | **1.170738** | **0.473912** | **2.075979** | **1.600186** |
| 16KiB 100Hz | 0.270627 | 0.289595 | 0.403214 | 0.395512 |
| 256B 100Hz 1% loss | 11.959990 | 11.990848 | 12.045457 | 12.047384 |

High-rate improvement was observed in both *short-run* repeats, but p99 was mixed (16KiB: attempt 1 p99 0.448ms→1.460ms worse; attempt 2 2.126ms→1.172ms better).

A longer 1000-message ABBA [CI #40607227553](https://github.com/rydolabs/citoq/actions/runs/40607227553), all 12/12 accepted, reported **baseline mean cohort p95 0.161616ms, coalesced 0.165087ms (+2.15% worse)** at offered 1000Hz. This negates any claim of *sustained* throughput/latency gain; investigation of first 100 vs later 800 messages is ongoing via segregated-window measurement. Coalescing is **not recommended as a default** from these results.

### Prioritize MsQuic worker tasks — rejected

[CI #40606960639](https://github.com/rydolabs/citoq/actions/runs/40606960639) and rejected closed [PR #25](https://github.com/rydolabs/citoq/pull/25): opt-in `QUIC_STREAM_START_FLAG_PRIORITY_WORK` and `QUIC_SEND_FLAG_PRIORITY_WORK` gave 1000Hz 200-message p95 2.156→1.998ms (−7.35%), but 100Hz p95 **0.390→0.425ms (+8.95%)**, and 1% loss p99 **12.194→33.150ms**. All 24/24 delivery accepted; performance regression forbids adoption.

## Status and next acceptance gates

- GitHub main remains unchanged and protected; the only completed candidate with large repeatable latency improvement is earlier opt-in single-owner `poll_wait()` PR #10.
- PR #23 is a benchmark-only, QUIC/mTLS/security-normalized comparison and has 9/9 positive delivery + rogue-CA rejection. PR #22 is diagnostic only.
- No proof CitoQ is universally slower than Zenoh QUIC/mTLS at 100Hz, and no stable ≤0.250ms p95 claim can be made across all shared runner environments. CitoQ CPU efficiency and p99 merit further engineering.
- Investigate initial/burst 1000Hz percentiles separately from steady-state; investigate intermittent discovery/match failure with 1% loss.
- For release, require pinned independent physical hardware, repeated per-payload/per-rate p50/p95/p99 with confidence bounds, CPU/RSS, loss/reconnect/churn, 100–200 nodes, principal-bound ACL verification. Do not confuse lab C ABI transport mTLS with full production authorization.

## Evidence integrity

All originally failed runs remain visible, and each accepted trial checks every unique sequence. The study does not merge PR #10, #22, #23 or #24 to main; no risky persistent QUIC mode is enabled by default.
