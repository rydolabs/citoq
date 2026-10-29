# Three-way public API 256B benchmark, 2026-10-29

## Decision

**Completed same-workload comparison:** CitoQ, Zenoh, and Cyclone DDS on a pinned Ubuntu 24.04 GitHub Actions runner, two separate Linux network namespaces linked by veth, with exactly the same 256-byte *application* payload and target offered rates, actual successful-send timestamp pacing checks, strict valid messages/CPU/RSS measurement, and identical configured netem profile per experiment.

**Not a 3-way transport-security comparison.** CitoQ uses MsQuic QUIC+mTLS and trusted client/server certificates; Zenoh 1.10.1 uses QUIC with mandatory mTLS, no TCP fallback, shared memory disabled. The official **Cyclone DDS 11.0.1 PyPI binary bundle does not include DDS Security**, and this run uses **unencrypted, unicast DDSI-RTPS/UDP** with RELIABLE QoS and KEEP_LAST(1024), and no shared memory. This distinction must remain visible in every chart/claim. Cyclone DDS data is *not* encrypted or mutually TLS-authenticated. https://pypi.org/project/cyclonedds/

**Independent restart gate** passed first: [corrected receiver + Coordinator SIGKILL baseline](https://github.com/rydolabs/citoq/actions/runs/40802864635), 6/6 valid scenarios (3× each of 1% and 2% loss), 1200/1200 post-restart messages, confirmed old netns PIDs absent after SIGKILL. The earlier 0/200 result was invalid fault injection (the old receiver process survived).

## Final measurements with native Cyclone DDS event receive

[CI #40807917636](https://github.com/rydolabs/citoq/actions/runs/40807917636) **success**, strict [raw artifact](https://github.com/rydolabs/citoq/actions/runs/40807917636/artifacts/12869506313), 30/30 accepted across all cohorts; every valid attempt had zero missing, duplicates or invalid messages, no abnormal process exits, correct payload length, sender actual rate within 5% of offered, no catch-up burst, explicit Cyclone DDS writer ACK and receiver WaitSet evidence.

| Workload | Repeats per engine | CitoQ p50/p95/p99 ms | Zenoh QUIC+mTLS p50/p95/p99 ms | Cyclone DDS RELIABLE unencrypted UDP p50/p95/p99 ms |
| --- | --- | --- | --- | --- |
| 256B 100Hz normal | 3×1000 | 0.151716 / **0.226110** / 0.298811 | 0.176633 / 0.230935 / 0.264661 | 0.122390 / 0.338331 / 0.453710 |
| 256B 1000Hz normal | 3×1000 | 0.104879 / 0.129519 / 0.160227 | 0.133517 / 0.163408 / 0.192953 | 0.069914 / **0.084271** / 0.097355 |
| 256B 100Hz netem delay 10ms±2ms, loss 1% | 2×200 | 10.164198 / **11.961872** / 22.601706 | 10.188851 / 12.067790 / 26.525090 | 10.368995 / 12.225336 / 36.396439 |
| 256B 100Hz netem delay 10ms±2ms, loss 2% | 2×200 | 10.066984 / **11.938620** / 23.423808 | 10.376533 / 15.963969 / 39.806549 | 10.279732 / 17.726166 / 80.736910 |

All three engines actually sent at 100.00Hz (normal) / 99.99Hz (loss) and 999.82–999.89Hz (1000Hz); no compressed sub-half-period gaps. 100Hz normal CitoQ p95 **0.226110ms** in this runner passes the numeric p95 0.250ms goal, but **a single CI run is not a production stability guarantee**, and a previous independent [three-way run](https://github.com/rydolabs/citoq/actions/runs/40807883928) measured CitoQ 100Hz p95 **0.294886ms**. Thus *reliable reproducibility of p95 ≤0.250ms is not yet established*.

**CPU seconds (sender/receiver), same runner process elapsed workload, not normalized hardware or scheduler overhead:**

| Workload | CitoQ | Zenoh QUIC+mTLS | Cyclone DDS UDP |
| --- | --- | --- | --- |
| 100Hz | 0.28 / 0.28 | **0.16 / 0.22** | 0.28 / 0.29 |
| 1000Hz | 0.23 / 0.23 | **0.12 / 0.15** | 0.23 / 0.24 |
| loss 1% | 0.16 / 0.16 | **0.065 / 0.09** | 0.20 / 0.22 |
| loss 2% | 0.155 / 0.16 | **0.06 / 0.09** | 0.20 / 0.215 |

CPU is whole-process user+system time and includes runtime startup, discovery and drain; there is no identical framing/QoS/crypto overhead guarantee. On-wire bytes are higher than application 256B and differ across middleware.

## Why native Cyclone DDS receive matters

The original Cyclone DDS Python benchmark receiver used `DataReader.take(N=128)` then `time.sleep(.0005)`, which introduced an application receiver scheduling floor unrelated to the DDS/UDP transmission path. The first complete 3-way CI [#40807234037](https://github.com/rydolabs/citoq/actions/runs/40807234037) used this **500µs sleep polling** and recorded Cyclone DDS p95 0.680493ms at 100Hz and 0.613973ms at 1000Hz.

The revised receiver uses `ReadCondition` + native `WaitSet`, drains the data reader until empty, and applies the same 80ms post-completion duplicate-detection grace as the CitoQ and Zenoh paths. The strict analyzer checks the receiver log for `CYCLONEDDS_RX_MODE=waitset`, and checks the sender log for `CYCLONEDDS_WRITER_ACKED=1`. A second complete independent event-driven [run #40807883928](https://github.com/rydolabs/citoq/actions/runs/40807883928) found Cyclone p95 0.248918ms (100Hz) and 0.129743ms (1000Hz), while the final run above found 0.338331ms and 0.084271ms respectively. These **are different CI runners, not a synchronized causal A/B**; the improvement is consistent with removing the 500µs sleep, but cannot be presented as a precise implementation speedup factor.

## Unresolved independent failure / risk

One further independent event-driven [run #40807890714](https://github.com/rydolabs/citoq/actions/runs/40807890714) was **correctly marked failure**: in 2% loss profile Zenoh QUIC/mTLS repeat 1 delivered **198/200**, missing **2** messages, despite sender exit and receiver exit both zero. This failure was **not retried into acceptance**, and the strict aggregate errors include its entire failed trial. The final 30/30 success does not disprove intermittent 2%-loss delivery failure. A longer repeated mixed-load/loss matrix and actual hardware hosts remain a separate reliability gate.

## Reproduction

- Branch: `bench/cyclonedds-threeway-parity-20261029`, based on [corrected restart probe PR #38](https://github.com/rydolabs/citoq/pull/38); no CitoQ production source changes.
- Workflow: `.github/workflows/benchmark-threeway-cyclonedds.yml`; analyzer: `bench/matrix/analyze_post_recovery_threeway.py`.
- Data probe: `benchmark/bench_driver.py`; static unicast DDS network config: `benchmark/cyclonedds_netns.xml`.
- Python versions pinned: `eclipse-zenoh==1.10.1`, `cyclonedds==11.0.1`; MsQuic 2.6.1; no fallback to TCP or SHM for Zenoh, no multicast for Cyclone DDS.
- Successful run contains 30 trial-level JSONs, resource measurements, sender/receiver logs, actual pacing and qdisc profile evidence.
- Each p50/p95/p99 in the table is the **median of trial-local percentile estimates** rather than a single pooled distribution; 2 loss trials per engine are inadequate for population tail claims.
- Payload validates sequence, timestamp, length, duplication and ordering-independent uniqueness; no independent CRC. Sender API invocation is not a receiver delivery acknowledgment; Cyclone DDS writer's built-in ACK does not replace application-level validation.
