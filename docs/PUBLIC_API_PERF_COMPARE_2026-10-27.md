# Public API-only CitoQ optimization and comparative benchmarks (2026-10-27)

## Exactly what was compared

**No CitoQ internal UDP adapter is used in this report.** CitoQ sends with the shipping Python `Context.publish_raw()` C ABI, `NativeRuntime` and native **authenticated mTLS/MsQuic**. CycloneDDS uses its **C `dds_write`/`dds_take`** application API, FastDDS its **C++ `DataWriter::write`/`DataReader::take_next_sample`** API, and Zenoh its **Python `Publisher.put` and subscriber callback** API. Zenoh same-host SHM is disabled and FastDDS built-in transports are replaced with UDPv4-only to prevent bypassing veth.

All benchmarks run separate Linux network namespaces linked with veth/bridge on the *same Ubuntu 24.04 GitHub runner*. Application timestamps use the same host CLOCK_MONOTONIC; these are not independent physical hosts.

**Critical differences:** CitoQ data plane uses mTLS/QUIC; DDS peers in the node test use BEST_EFFORT UDP and Zenoh peer uses TCP. Authentication, reliability, language bindings and frame encodings are not security/QoS equivalents. Thus the table below describes **these tested application configurations only**, not an intrinsic middleware ranking.

## Performance change in the CitoQ product source

The original `MsQuicRequestSessionManager` packet path owned a vector in its event queue and copied it **again** into `SendContext`, which must own memory until `QUIC_STREAM_EVENT_SEND_COMPLETE`. The new path **moves** that owned vector into `SendContext`. On reception, the first aggregated `QUIC_STREAM_EVENT_RECEIVE` vector is moved into `StreamState::received` when no bytes have been buffered, rather than allocating another vector and appending it. Follow-on/chunked receive events continue appending normally.

**Preserved behavior:** CTD1 wire packet layout, request/reply stream semantics, FIN flags, mTLS trust checks, single owner-thread polling, oversized packet limits and the public C/Python method signatures. No new unnegotiated QUIC stream protocol or insecure fallback was introduced.

Source change: [`src/msquic_request_session_manager.cpp`](https://github.com/rydolabs/citoq/blob/perf/public-api-quic-copy-and-scaling-20261027/src/msquic_request_session_manager.cpp).

### Before/after measured *with both source versions built on one runner*

Pinned unoptimized baseline `08188eba73284dd1069016174ad216f5e02d2cfa`; modified product build at [PR #8](https://github.com/rydolabs/citoq/pull/8); identical public Context, Python binding, mTLS certificates and corrected post-publish `NativeRuntime.poll()`. **Evidence**: [GitHub Actions #40461793687](https://github.com/rydolabs/citoq/actions/runs/40461793687), **16/16 trials accepted**, 0 missing/invalid/duplicate.

| Offered pacing, payload | Before p95 median | After p95 median | Relative change |
| --- | ---: | ---: | ---: |
| 100Hz, 256B, 200 samples ×2 | 0.697ms | 0.690ms | −1.1% |
| 100Hz, 16KiB, 200 samples ×2 | 0.760ms | 0.731ms | −3.8% |
| 1000Hz, 256B, 400 samples ×2 | 1.036ms | 1.086ms | +4.9% (worse) |
| 1000Hz, 16KiB, 400 samples ×2 | 1.297ms | 1.920ms | +48.0% (worse) |

Because the first high-rate run suggested a potentially substantial regression, a second experiment **alternated order for six source-version pairs**, collected CPU and RSS and used more samples per run. **Evidence:** [GitHub Actions #40462840456](https://github.com/rydolabs/citoq/actions/runs/40462840456), **12/12 trials accepted**; 500×16KiB per trial at 1000Hz, 3000/3000 unique deliveries per version.

| Metric, median across six trials | Pinned baseline | Modified |
| --- | ---: | ---: |
| Application p95 | 1.351ms | 1.201ms |
| Application p99 | 1.948ms | 1.660ms |
| Sender+receiver process CPU time | 0.565s | 0.570s |

The corrected interleaved test has **11.1% lower median p95 but 0.9% higher CPU**, and individual pairs are variable. The observed improvement is **not robust enough for a blanket performance speedup claim**; nevertheless 1000Hz hard failure did not repeat, the copies removed are verifiable at source level and all delivery checks passed. Keep full original first-run negative result.

## Node-count comparison with *real public application APIs*

One publisher, N−2 interested subscribers and one irrelevant subscriber. 1 nominal 256-byte topic, 4 user messages per interested subscriber, 10Hz, 500ms deliberate fixed delay after each engine's own publisher-ready criterion, 2 repetitions per engine/N. Receivers start before publisher; all are separate Linux network-namespace processes.

The **standardized observable result** is from orchestration immediately before the publisher subprocess is launched to the last interested subscriber's first valid application message (global monotonic timestamp). This includes process creation, interest/endpoint matching, mTLS connection if configured, a 500ms fixed warmup, actual first send and receive. These steps are **not a normalized discovery-only convergence measurement**, and publisher-ready metrics cannot be compared directly due to different engine definitions.

[Actions run #40462524812](https://github.com/rydolabs/citoq/actions/runs/40462524812), verified **3-node and 10-node jobs PASS (8/8 per N)**:

| Engine public API | 3 nodes: all first useful data | 10 nodes: all first useful data | 3 nodes: full veth RX | 10 nodes: full veth RX |
| --- | ---: | ---: | ---: | ---: |
| CitoQ Python Context + mTLS/QUIC | **743.8ms** | **756.2ms** | 52,634 B | 322,147 B |
| Cyclone DDS C | 512.9ms | 526.8ms | 24,768 B | 326,134 B |
| Zenoh Python Peer TCP (SHM off) | 532.0ms | 542.2ms | 12,783 B | 263,568 B |
| Fast DDS C++ UDPv4 (SHM off) | 592.6ms | 681.2ms | 81,696 B | 1,450,436 B |

Time-to-all-first-data values are median of 2 per-trial measurements; full veth RX includes receiver startup, control, application data and lifetime heartbeat bytes. **CitoQ startup-to-first-data is slower on both verified node counts**. At 10 nodes its full veth RX is in the same approximate range as Cyclone DDS; the comparison does not establish superiority.

**30-node full four-engine job also passed: [run #40462524812](https://github.com/rydolabs/citoq/actions/runs/40462524812), [30-node raw artifact #12715760534](https://github.com/rydolabs/citoq/actions/runs/40462524812/artifacts/12715760534). All 3 node-matrix jobs passed; exactly 8/8 accepted application-API trials per node count, **24/24 total**, 0 invalid/duplicate/missing messages.**

| Engine public API | 3-node first-data-to-all (ms) | 10-node (ms) | 30-node (ms) | 3-node full veth RX bytes | 10-node | 30-node |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| **CitoQ Context mTLS/QUIC** | 743.8 | 756.2 | 756.3 | 52,634 | 322,147 | **1,100,897** |
| Cyclone DDS C | 512.9 | 526.8 | 538.9 | 24,768 | 326,134 | 4,191,209 |
| Zenoh peer Python TCP | 532.0 | 542.2 | 544.2 | 12,783 | 263,568 | 4,759,572 |
| Fast DDS C++ UDPv4 | 592.6 | 681.2 | 3,590.3 | 81,696 | 1,450,436 | 24,203,660 |

At **30 nodes**, CitoQ's whole-trial RX was **73.7% lower than Cyclone DDS** and **76.9% lower than Zenoh** on this particular workload, *despite using mTLS*. At **3 nodes**, CitoQ consumed **more** bytes than Cyclone/Zenoh, and at **10 nodes** it was approximately even with Cyclone but higher than Zenoh. This shows an interesting **observed scaling crossover** at 30 nodes, **not a proven general protocol advantage**. Runs are only twice repeated and full veth RX includes initial receivers, handshakes, control, maintenance and application data. Relative results may change with session lifetime, interest fraction and encryption parity.

CitoQ reached the last interested reader **~217ms after Cyclone** and **~212ms after Zenoh** at 30 nodes. Its security handshake and discovery readiness remain an important optimization target. The first-data-to-all measurement includes a fixed 500ms delay after publisher-specific readiness; it is neither a peer-invariant discovery-only time nor a simultaneous cold-start result.

**Same-runner before first data RX, excluding the pre-publisher receiver start:** CitoQ **610,024 bytes**, Cyclone **757,279**, Zenoh **457,233**, Fast DDS **4,603,613** at 30 nodes. This is an approximate **25/50ms-sampled** startup-to-first-data counter and includes probe/handshake/application bytes; it does not isolate discovery traffic.

**Candidate 40/40 public-API payload-size comparison also passed**: [run #40463065417](https://github.com/rydolabs/citoq/actions/runs/40463065417), [raw artifact #12715640893](https://github.com/rydolabs/citoq/actions/runs/40463065417/artifacts/12715640893). It uses the source with the MsQuic buffer-move improvement, not the earlier unmodified source.

| Nominal payload | CitoQ mTLS p95 median | Cyclone DDS | Zenoh peer TCP | Fast DDS |
| ---: | ---: | ---: | ---: | ---: |
| 64B | 0.683ms | 0.663ms | 0.200ms | 0.609ms |
| 256B | 0.695ms | 0.664ms | 0.206ms | 0.619ms |
| 1KiB | 0.705ms | 0.700ms | 0.190ms | 0.622ms |
| 4KiB | 0.706ms | 0.774ms | 0.216ms | 0.599ms |
| 16KiB | 0.768ms | 1.389ms | 0.210ms | 0.655ms |

All 40 trials delivered 200/200 payloads at **100Hz**; this is **not** a maximum-throughput test. The 16KiB CitoQ result is below 1ms while its public mTLS/QUIC and double application-runtime transitions remain in place. The peers have different cryptography and QoS.


## Ongoing improvement priorities backed by evidence

1. **Discovery + authenticated session startup:** 3/10-node CitoQ delays compared with unencrypted DDS/Zenoh are **notably larger**. Instrument announcements, snapshot acquisition, route acknowledgment, MsQuic TLS connect and first data separately, then optimize only bottlenecks actually observed; don't assume all delay is from QUIC crypto.
2. **QUIC publish at high rate:** retained per-message `StreamOpen/StreamStart/StreamSend(FIN)` preserves wire compatibility but may incur expensive per-message stream lifecycle. A persistent framed-stream experiment requires wire version negotiation, flow-control/backpressure and HOL regression tests before product default. It was **not** implemented as a backwards-incompatible change.
3. **Fixed-rate 100Hz ≠ maximum throughput:** run multiple offered rates to saturation with time-to-delivery tails, RSS and CPU; 16KiB×1000Hz is merely an offered 131.1Mbps, not proven sustainable physical throughput.
4. **Discovery control overhead:** separate control-only traffic from QUIC handshake and probes; use 10%/50%/100% interested topics, 30/100 nodes and churn. Do not use CitoQ's research-only UDP adapter efficiency as public API evidence.
5. **Automatic polling and SHM:** currently explicit `NativeRuntime.poll()` and no automatic same-host SHM Context wiring. Integration must preserve single owner thread and authenticated routing.

## Reproducibility and checks

- [PR #8](https://github.com/rydolabs/citoq/pull/8): changed source + exact workflow definitions and per-trial JSON/log artifact uploads.
- [AB 16/16](https://github.com/rydolabs/citoq/actions/runs/40461793687) and [interleaved stress 12/12](https://github.com/rydolabs/citoq/actions/runs/40462840456).
- Product source build and CTest, G1 authenticated native Pub/Sub, G4 Python native mTLS E2E, and CitoQ native latency regression gates also passed on the feature branch.
- [Prior public SDK payload-size matrix](https://github.com/rydolabs/citoq/actions/runs/40437891430) is from the unmodified pinned product and correct Zenoh SHM-off transport. A new four-engine matrix run for the candidate source is tracked separately.

No code was merged into `main`; PR #8 is stacked on benchmark PRs #7 → #6 → #4.
