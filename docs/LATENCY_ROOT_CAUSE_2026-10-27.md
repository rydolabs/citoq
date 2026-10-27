# CitoQ 10ms p95 root-cause investigation (2026-10-27)

## Confirmed primary cause

The original public-API benchmark's 100 Hz sender ran `transport.poll()` **before** `ctx.publish_raw()` and then slept until the next tick. `publish_raw()` enqueues outbound MsQuic work; `src/msquic_request_session_manager.cpp` `queue_packet()` posts work onto the loop owner, and `poll()` drains that queue. The packet therefore waited about **10 ms in the application queue before transmission**. The original 10.65ms p95 was not pure QUIC or mTLS network latency.

**Correction:** perform a second `transport.poll()` immediately after every successful `publish_raw()`, while preserving pre-publish polls for discovery/match readiness. The same pinned CitoQ production source `08188eba73284dd1069016174ad216f5e02d2cfa` and same native mTLS/QUIC implementation are used for both modes.

## Reproduced A/B experiments

All normal-network cases below delivered 200/200 unique 256-byte application payloads at 100Hz, zero duplicates, invalids or missing messages. Two network namespaces on the same Ubuntu 24.04 runner share CLOCK_MONOTONIC. p95 below is application send timestamp to Python subscriber callback; the comparison does not isolate QUIC wire latency.

| Evidence | Legacy baseline p95 (3 repetitions) | Immediate baseline p95 (3 repetitions) | Median reduction |
| --- | --- | --- | --- |
| [First A/B run #40425342142](https://github.com/rydolabs/citoq/actions/runs/40425342142) | 10.572, 10.589, 10.579 ms | 0.593, 0.601, 0.587 ms | 9.986 ms / 94.4% |
| [Second A/B run #40425968000](https://github.com/rydolabs/citoq/actions/runs/40425968000) | 10.661, 10.673, 10.659 ms | 0.763, 0.711, 0.713 ms | 9.948 ms / 93.3% |

In the second run the corrected **four-engine comparison** included CitoQ baseline p95 **0.713/0.707 ms** and bidirectional 10ms ±2ms, 1% loss p95 **12.170/12.404 ms**, each 200/200 delivered. Against the original [pre-fix comparison run #40242491625](https://github.com/rydolabs/citoq/actions/runs/40242491625) that reported CitoQ baseline p95 10.681/10.649ms, the 10ms increase was a benchmark scheduling artifact. Other engines, QoS profiles, and security settings are *not* normalized; this is not a vendor ranking.

## Failures are not hidden

- [First run #40425342142](https://github.com/rydolabs/citoq/actions/runs/40425342142): one of six corrected loss-profile A/B cases failed to establish an eligible subscriber (0/200, sender exit 2). Original per-process stack trace and evidence ZIP are retained: [artifact #12698214365](https://github.com/rydolabs/citoq/actions/runs/40425342142/artifacts/12698214365). This is a separate intermittent startup/handshake/discovery issue, **not** evidence of low transport latency.
- [Second run #40425968000](https://github.com/rydolabs/citoq/actions/runs/40425968000): overall four-engine comparison was **15/16** due to Zenoh TCP loss-profile receiving **141/200** on one trial. CitoQ four-engine samples were **4/4 complete**. This invalidates any claim of 16/16 acceptance for that run. Raw logs remain in that run's artifact.
- On the second fix commit, source build/CTest, G1 native mTLS Pub/Sub, G4 Python native mTLS E2E passed: [source regression](https://github.com/rydolabs/citoq/actions/runs/40425973526), [G1](https://github.com/rydolabs/citoq/actions/runs/40425973520), [G4](https://github.com/rydolabs/citoq/actions/runs/40425973591).

## Automated prevention

The focused `.github/workflows/benchmark-citoq-latency.yml` enforces a 3+3 A/B baseline gate, 200/200 delivery, >5ms median p95 improvement and <5ms absolute corrected median p95 (a generous regression threshold, not a claim that 5ms is typical). The independent full four-engine workflow retains **strict** all-engine acceptance and may fail on non-CitoQ transport issues; neither check suppresses failed samples.

## Correct application integration

The `NativeRuntime` Python wrapper is an explicitly-polled owner-thread transport. Applications must service it regularly, especially **after queueing publishes**. Sleeping until the next publish without polling adds up to the publication period in queued-send latency. A dedicated runtime thread/event wakeup would require explicit thread-ownership and Context callback safety guarantees; this fix deliberately avoids unverified concurrent polling changes.
