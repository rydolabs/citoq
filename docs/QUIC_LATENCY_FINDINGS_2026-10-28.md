# CitoQ native QUIC vs Zenoh latency investigation — 2026-10-28

## Verdict

**The original p95 ≤0.250ms product objective is NOT consistently demonstrated.** A verified opt-in single-owner event-driven receive path improves the benchmark, but further transport prototypes have not passed all p95, p99, CPU, packet-loss, high-rate, and wire-compatibility gates. No transport prototype in this report is safe to enable by default or merge as a release-ready optimization.

The pinned `main` SHA during the investigation was `08188eba73284dd1069016174ad216f5e02d2cfa`. This report does not modify native code on main.

## Reproducibility and comparison boundaries

- Each reported public SDK probe uses two Linux network namespaces linked by veth/bridge on the same Ubuntu 24.04 GitHub runner. The namespaces share CLOCK_MONOTONIC; these are **not** physical hosts.
- CitoQ: `Context.publish_raw()` through C ABI + `NativeRuntime` + authenticated native mTLS/MsQuic. Zenoh: Python Publisher.put / callback, TCP peer, explicit SHM disabled, **without equivalent TLS/mTLS**.
- p95/p99 are one-way *application send timestamp → Python callback* latencies, **not** transport-only latencies. Cohort summaries are medians of trial-level percentiles, not pooled percentiles.
- Nominal 100Hz/1000Hz is an offered paced rate, **not** maximum sustainable throughput. Each trial verifies 200/200 unique messages with zero missing, duplicates or invalid data. Trials with delivery failures are not accepted.
- Timing varies substantially between runner attempts; do not compare two absolute p95s from different runners as if they were paired measurements.

## 1. Verified improvement: remove fixed receiver sleep

Receiver loop originally did `NativeRuntime.poll(); time.sleep(0.0005)`. PR [#10](https://github.com/rydolabs/citoq/pull/10) adds an opt-in blocking, wake-driven `poll_wait()` on the same owner thread while preserving existing wire/mTLS/C++ Context behavior.

| Paired CI run | Existing CitoQ 500µs sleep p95 | New event-driven p95 | Receiver CPU seconds (old→new) |
| --- | ---: | ---: | ---: |
| [First](https://github.com/rydolabs/citoq/actions/runs/40502884791) | 0.6980ms | 0.2965ms | 0.29→0.20 |
| [Same-commit rerun](https://github.com/rydolabs/citoq/actions/runs/40502884791/attempts/2) | 0.6910ms | 0.3422ms | 0.28→0.20 |
| [Final source](https://github.com/rydolabs/citoq/actions/runs/40503802897) | 0.7267ms | 0.3326ms | 0.29→0.19 |

All 18/18 trial groups succeeded per each complete final run (200/200 messages per trial); native C++ CTest 67/67 plus authenticated G1/G4 CI succeeded for PR #10. Busy-polling achieved a lower delay in some runs but consumed about 4.63 receiver CPU seconds per trial, so it was **not** selected as the production design.

## 2. Measured asynchronous and API stage costs

Compiler-disabled-by-default timing traces, [PR #16](https://github.com/rydolabs/citoq/pull/16) and [PR #18](https://github.com/rydolabs/citoq/pull/18):

| Stage | Observed median interval | Evidence |
| --- | ---: | --- |
| MsQuic StreamOpen+StreamStart synchronous API | 7.6–8.6µs | [stage CI](https://github.com/rydolabs/citoq/actions/runs/40597409266) |
| MsQuic StreamSend synchronous API | 0.6–0.8µs | same |
| Sender event queue enqueue→drain | 6–8µs | [queue CI](https://github.com/rydolabs/citoq/actions/runs/40598294080) |
| Receiver MsQuic callback→single-owner queue drain | 25–28µs | same |

These are **individual synchronous API/queue timing observations**, instrumented with per-message trace printing, not a complete causal decomposition of end-to-end p95. Do not add p95 percentiles to estimate an end-to-end p95.

## 3. Experiments rejected or not promoted

### StreamSend(START|FIN) instead of synchronous StreamStart

The opt-in START experiment was valid per MsQuic documentation but **did not improve application p95**. Same-runner baseline mean of two cohort-level p95s: 0.312230ms; candidate 0.323595ms (**3.64% worse**). All 12/12 CitoQ source trials accepted. [CI](https://github.com/rydolabs/citoq/actions/runs/40597987194) / [closed PR #17](https://github.com/rydolabs/citoq/pull/17). Default behavior unchanged.

### One persistent, framed CTD1 data QUIC stream

[PR #19](https://github.com/rydolabs/citoq/pull/19) is research-only. It implements bounded CQDP+LE32 data framing and authenticated CQPN/CQPA negotiation before use. Request/Reply stays on the original QUIC streams. The mixed-version verification [passed](https://github.com/rydolabs/citoq/actions/runs/40599882600): new→old, old→new, new→new each 3x200 accepted, with traces proving negotiated persistent data only for new→new.

Different accepted paired runner attempts gave contradictory 100Hz/256B effects:
- Original experiment [attempt 1](https://github.com/rydolabs/citoq/actions/runs/40598910054) baseline 0.122508ms vs candidate 0.116209ms (5.14% better); [attempt 2](https://github.com/rydolabs/citoq/actions/runs/40598910054/attempts/2) 0.314716ms vs 0.289403ms (8.04% better, **still over the 0.250ms objective**).
- With negotiated protocol [CI](https://github.com/rydolabs/citoq/actions/runs/40599798785), baseline 0.229960ms vs candidate 0.239994ms (**4.36% worse**).
- Under bidirectional 10ms±2ms/1% packet loss, [CI](https://github.com/rydolabs/citoq/actions/runs/40599798995): baseline p95 **12.0703ms**, candidate **16.2181ms**; baseline p99 **12.1776ms**, candidate **36.0245ms**. This is consistent with stream head-of-line blocking.
- Another [stress run](https://github.com/rydolabs/citoq/actions/runs/40599264882) found p95 12.1738ms vs 21.0653ms and p99 12.3613ms vs 30.5741ms in a loss scenario.

**Decision:** NOT production-ready. Do not merge or enable by default.

### Eight persistent QUIC stream shards

[PR #20](https://github.com/rydolabs/citoq/pull/20) (closed and unmerged) round-robins authenticated CTD1 over eight streams under an opt-in compiler flag; same source builds for baseline / single / eight-shard.

[Multi-condition strict CI](https://github.com/rydolabs/citoq/actions/runs/40600151638), two accepted 200/200 trials per condition and variant:

| Condition | Existing per-message p95 | Single persistent p95 | Eight-shard p95 | Observation |
| --- | ---: | ---: | ---: | --- |
| 256B, 100Hz | 0.271388ms | 0.270475ms | 0.269948ms | **All miss 0.250ms goal** |
| 64B, 100Hz | 0.259934ms | 0.256524ms | 0.255993ms | Small gain |
| 16KiB, 100Hz | 0.380003ms | 0.379784ms | 0.397479ms | Eight shards worse |
| 256B, 1000Hz | **0.350899ms** | 1.346670ms | 0.659117ms | Eight shards **87.84% worse** |
| 256B, 100Hz, 1% loss | 11.989325ms | 12.171379ms | 11.996895ms | Eight shards restore p95 loss behavior |

At 1000Hz, p99 was legacy 0.487183ms vs eight-shard 0.961606ms (**97.39% worse**). At 1% loss, p99 was legacy 12.155423ms vs eight-shard 12.238750ms, nearly equal. While sharding mitigates one-stream HOL, the high-rate regression and 100Hz target miss prohibit adoption.

## 4. CI and release cautions

- All GitHub Actions artifacts/logs and negative outcomes are kept. No missing or failed trial was silently converted to a zero latency.
- Default-source CTest and G4 authenticated native/Python runs passed on experimental branches. Some G1/G3 **2% netem packet-loss** subgates intermittently failed even without candidate compiler flags; for [PR #20's G1 run](https://github.com/rydolabs/citoq/actions/runs/40600175392) the initial job failed and a retry passed. Preserve the initial failure; do not count a single retry as reliable loss-tolerance proof.
- Compiler-enabled CQDP is still a protocol experiment: mixed-version handshake smoke does not guarantee security against malformed inputs, memory bound across streams, backpressure, graceful reconnect/restart, cancellation, resource caps, or preservation of all Pub/Sub ordering/QoS semantics.
- The primary `main` code was not changed or merged during these experiments, and CitoQ is not established as production release-ready.

## Engineering decision / next actionable target

1. Keep verified opt-in single-owner `poll_wait()` [PR #10](https://github.com/rydolabs/citoq/pull/10) as the only demonstrated substantial latency improvement, while preserving Context ownership and mTLS.
2. Do **not** ship StreamSend START, single persistent CQDP, or eight-shard CQDP as performance defaults.
3. Instrument the remaining unmeasured boundary: native MsQuic worker scheduling and receive callback through C++ dispatch/FFI/Python callback. Quantify using trace IDs with a bounded/ring-buffered diagnostic trace (not per-message stderr on product builds).
4. Re-run fair security-normalized Zenoh (TLS/mTLS) versus CitoQ configurations, plus multi-run stable hardware measurements. Require ≥3 independent CI runners or pinned physical hosts, p50/p95/p99 intervals, CPU/RSS, 64B–16KiB, 100–1000Hz, 1% loss, reconnect, and multi-node scaling.
5. Only call the `p95 <=0.250ms` objective complete when it consistently passes **without regressions** under every agreed safety/performance gate; one fast runner is insufficient.
