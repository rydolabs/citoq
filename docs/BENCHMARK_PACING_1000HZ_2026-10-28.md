# Verified 1000Hz benchmark pacing correction — 2026-10-28

## Engineering verdict

The historical 256-byte/1000Hz CitoQ-versus-Zenoh p95/p99 comparison was confounded by **the benchmark's CitoQ first-message discovery wait**. Before this change, `paced_send()` initialized its absolute send deadline before `publish_raw()` could first succeed. If CitoQ took 30ms to discover a matching subscriber, the benchmark replayed ~30 expired deadlines without sleeping after matching, so 1000Hz became a short initial burst. Zenoh QUIC's first put normally did not have that wait, yielding asymmetric offered load.

This change **corrects benchmark measurement**, not a product runtime latency or on-wire protocol. CitoQ Coordinator/P2P, C ABI, MsQuic mTLS, existing Python Context and single-owner model remain unchanged.

## Implementation

The benchmark default is `--pacing-mode rebase`:

1. The first accepted publish starts pacing; discovery delays are **not** part of the offered rate schedule.
2. Future deadlines are absolute, maintaining actual ~1000Hz frequency rather than sleeping a full 1ms *after every call* (the latter achieved only ~924Hz in [intermediate diagnostic](https://github.com/rydolabs/citoq/actions/runs/40644689280)).
3. If sending falls more than one whole period behind, skip expired ticks instead of replaying a catch-up burst. No payloads are intentionally dropped.
4. The original bug remains reproducible with `--pacing-mode legacy`. Opt-in `--pacing-trace` emits `PACING_REPORT` including first accepted message wait, successful publish interval quantiles, short-gap count, first 100 span and observed offered Hz.
5. Deterministic fake-clock tests reproduce a 30ms discovery wait, a 4ms scheduling stall and 70µs sleep overshoot. Existing CitoQ send/poll ordering tests still run.

**Measurement boundary:** `PACING_REPORT.observed_hz` measures Python successful *publish API invocation timestamps* and is not physical NIC wire throughput. `latency_us_p95` remains payload timestamp to receiver Python callback on two netns sharing the kernel monotonic clock.

## Verified 256B / 1000Hz / 1000-message QUIC/mTLS A/B

Each GitHub run compares four source-cohorts, 3 strict 1000-message trials each: CitoQ legacy/fixed and Zenoh QUIC/mTLS legacy/fixed. All 12 trial results in each independent run were accepted with no invalid/duplicate/missing messages. Fixed CitoQ and fixed Zenoh both reported ~999.9Hz, with zero artificially short catch-up intervals.

| Workflow | CitoQ legacy p95 / p99 | CitoQ corrected p95 / p99 | Zenoh QUIC/mTLS corrected p95 |
| --- | ---: | ---: | ---: |
| [40644983045](https://github.com/rydolabs/citoq/actions/runs/40644983045) | 0.286558 / 1.642452ms | **0.185472 / 0.218540ms** | 0.224222ms |
| [40645024198](https://github.com/rydolabs/citoq/actions/runs/40645024198) | 0.085748 / 0.868950ms | **0.065459 / 0.144834ms** | 0.091432ms |
| [40645363413](https://github.com/rydolabs/citoq/actions/runs/40645363413) | 0.127137 / 1.005358ms | **0.117698 / 0.143772ms** | 0.128544ms |
| [40645355955](https://github.com/rydolabs/citoq/actions/runs/40645355955) | 0.246662 / 1.911496ms | **0.187982 / 0.242854ms** | 0.232605ms |

The high-rate analyzer enforces 200/200 or 1000/1000 (depending on workload) delivery, exact trace count and corrected mode 1000Hz observed rate within ±3%, with at most three under-0.5ms gaps. CitoQ corrected p95 stayed below 0.250ms on these runners and was lower than Zenoh QUIC/mTLS on each paired run. The absolute p95 varies strongly by runner and **does not constitute a physical-hardware/long-run guarantee or universal architectural superiority**. Zenoh CPU was generally lower.

### Direct proof of the mechanism

[CI 40645363413](https://github.com/rydolabs/citoq/actions/runs/40645363413) measured **30.69ms** first accepted publish delay in the CitoQ legacy cohort and **30.72ms** in corrected. Both waited for the same discovery. The legacy cohort then compressed first 100 publish invocations into **68.4ms**, including **31** sub-0.5ms gaps, and first-100 p95 **1.120873ms**. Corrected cohort covered **99.1ms**, had **zero** short gaps, and first-100 p95 **0.125024ms**. Zenoh QUIC/mTLS first accepted wait was approximately **0.039ms**. Thus the high-rate tail reported in some earlier comparisons largely reflected unequal bursts rather than normal steady-state 1000Hz load.

## 100Hz and lossy network regression

[Successful 100Hz CI](https://github.com/rydolabs/citoq/actions/runs/40645899789) used identical native mTLS/QUIC/CitoQ source; 200 messages × 2 repetitions per engine/cohort/scenario, 12/12 accepted. Baseline p95 CitoQ legacy/corrected/Zenoh QUIC: **0.315843 / 0.305021 / 0.309484ms**; with 10ms±2ms + 1% netem loss: **12.043108 / 12.027359 / 15.372258ms**. Corrected CitoQ actual successful-publish rate was ~99.99Hz with zero sub-half-period gaps. The high rate and normal/loss conditions had **zero loss, duplication or invalid data in accepted trials**.

**Important unresolved caveat:** prior [loss regression CI 40645433816](https://github.com/rydolabs/citoq/actions/runs/40645433816) passed all three normal 100Hz cohorts but one *legacy CitoQ* 1% loss trial published 200/200 while receiver observed **0/200**. The job failed (correctly), and [raw logs](https://github.com/rydolabs/citoq/actions/runs/40645433816/artifacts/12797975092) are retained. The failure was present with the old pacing comparator and is consistent with previously observed intermittent CitoQ loss/connection reliability failures in [PR #23](https://github.com/rydolabs/citoq/pull/23), but its specific native cause is **not yet established**. One succeeding repeat is not a reliability proof. A CitoQ transport loss/reconnection investigation is a separate open gate.

## Reproduction and release boundary

- Source: [PR #27](https://github.com/rydolabs/citoq/pull/27), based on [security-normalized Zenoh parity PR #23](https://github.com/rydolabs/citoq/pull/23).
- High-rate workflow: `.github/workflows/benchmark-highrate-pacing.yml`; analyzer: `bench/matrix/analyze_highrate_pacing.py`.
- Normal/loss workflow: `.github/workflows/benchmark-pacing-100hz-loss.yml`; analyzer: `bench/matrix/analyze_pacing_100hz_loss.py`.
- All raw artifacts retain bridge/netns details, commit identity, stdout/stderr, resource measurements and trial-level JSON. Main product code remains untouched until reviewed.
- C++ source build, native authenticated G1 Pub/Sub, Python G4 native mTLS and separate public mTLS QUIC comparison workflows passed on the benchmark source ([CTest](https://github.com/rydolabs/citoq/actions/runs/40645440675), [G1](https://github.com/rydolabs/citoq/actions/runs/40645440734), [G4](https://github.com/rydolabs/citoq/actions/runs/40645441011)).

**Scope of completion:** benchmark timing validity and representative 1000Hz parity verified. **Not completed:** physical-node validation, many-minute high-rate saturation, QoS matching, lower CitoQ CPU than Zenoh, or robust 1% loss/reconnect assurance.
