# Pinned CitoQ / CycloneDDS / Zenoh / Fast DDS comparison

- Pinned CitoQ source: \`08188eba73284dd1069016174ad216f5e02d2cfa\`; harness commits are separate.
- Ubuntu 24.04 Actions runner, two Linux network namespaces, veth, Linux Bridge, optional bidirectional \`tc netem delay 10ms 2ms loss 1%\`.
- Protocols are not equivalent: CitoQ native mTLS/QUIC, Zenoh peer TCP, CycloneDDS RELIABLE RTPS/UDP, Fast DDS RELIABLE RTPS/UDP. No native-engine ranking without controlling serialization, threads and security.
- 256-byte application payload, 100 messages/second, 200 sequenced unique messages, 2 replicates/profile. One-way *application* latency uses CLOCK_MONOTONIC shared across namespace processes on the same kernel. Discovery/warmup bytes are included in host-veth byte counters.
- Run artifacts include per-process exit status, expected/received/missing/duplicate, p50/p95/p99 and bridge RX/TX. A failed adapter is not recorded as a measured zero.
- Fast DDS native driver will be added and separately validated; the first workflow targets CitoQ, CycloneDDS and Zenoh.

This is a functional measurement under an emulated network, not proof of real wireless performance or production maturity.

## CitoQ public API send-loop latency A/B (2026-10-27)

At 100 Hz the original harness called `transport.poll()` **before** `ctx.publish_raw()`, then slept until the next message. MsQuic packet submission is queued onto the poll owner, so the pending send was serviced in the *next* 10 ms iteration rather than immediately. The original ~10.65 ms baseline p95 must **not** be ascribed solely to QUIC or encryption.

The harness now services the queue **immediately after a successful publish** (default `--citoq-poll-mode immediate`). `--citoq-poll-mode legacy` preserves the old loop for A/B without changing the pinned product source. The A/B regression gate targets the **unshaped normal-network profile** with three repetitions per mode (200 payloads at 100 Hz each); it checks zero missing/duplicate/invalid packets and enforces a baseline p95 regression guard. The subsequent full four-engine comparison separately tests both unshaped and 10 ms/1% loss profiles. An initial loss-profile A/B trial exposed a separate intermittent startup/matching failure (0/200, sender exit 2); that failure is preserved in [run #40425342142](https://github.com/rydolabs/citoq/actions/runs/40425342142), not included in the successful baseline latency claim. Raw outputs: `benchmark-evidence/{legacy,immediate}/` and `poll-fix-report.json`. A full four-engine comparison then uses the corrected default.

**Scope:** This changes the benchmark application's event-loop scheduling, not the CitoQ engine's QUIC transport or mTLS. Real integrations that drive `NativeRuntime.poll()` infrequently can still introduce queueing delay; call it promptly after publishes, or drive a continuous owner-thread event loop. The Python Context and transport runtime currently require explicit polling.

### Measured same-build normal-network A/B

First GitHub Actions A/B [run #40425342142](https://github.com/rydolabs/citoq/actions/runs/40425342142) (source unchanged, the new harness scheduling alone differs), 3 repetitions per mode, all **200/200** unique deliveries:

| Baseline p95 application latency | Rep 1 | Rep 2 | Rep 3 | Median |
| --- | ---: | ---: | ---: | ---: |
| Legacy poll-before-publish | 10.572 ms | 10.589 ms | 10.579 ms | 10.579 ms |
| Immediate post-publish poll | 0.593 ms | 0.601 ms | 0.587 ms | 0.593 ms |

**Reduction: 9.986 ms (94.4%)** on these runs. Payload and native mTLS/QUIC stack are identical. This identifies a **benchmark sender-loop scheduling artifact**, not an underlying 10 ms QUIC cipher/network delay. The impaired network experiment was not completely successful (11/12 full deliveries) because one trial could not discover an eligible subscriber; this is a separate reliability investigation, not an omitted successful latency datapoint.
