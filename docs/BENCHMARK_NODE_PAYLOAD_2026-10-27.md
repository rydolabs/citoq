# CitoQ benchmark: payload size and discovery at multiple nodes (2026-10-27)

## Evidence / identity

- **Pinned CitoQ product code:** [08188eba73284dd1069016174ad216f5e02d2cfa](https://github.com/rydolabs/citoq/commit/08188eba73284dd1069016174ad216f5e02d2cfa). Tested harness includes the explicit **post-publish `NativeRuntime.poll()`** fix; production CitoQ binaries otherwise identical to pin.
- **Corrected public API payload-size matrix:** [Actions run #40437891430](https://github.com/rydolabs/citoq/actions/runs/40437891430), [raw artifact #12705411179](https://github.com/rydolabs/citoq/actions/runs/40437891430/artifacts/12705411179). **40/40 accepted**, every trial received **200/200** unique samples, 0 invalid and duplicate samples.
- **Public CitoQ multi-node QUIC:** [Actions run #40437891335](https://github.com/rydolabs/citoq/actions/runs/40437891335), artifacts [3-node #12705051764](https://github.com/rydolabs/citoq/actions/runs/40437891335/artifacts/12705051764), [10-node #12704763591](https://github.com/rydolabs/citoq/actions/runs/40437891335/artifacts/12704763591), [30-node #12703974815](https://github.com/rydolabs/citoq/actions/runs/40437891335/artifacts/12703974815): **3/3 jobs PASS**.
- **Internal UDP research-adapter node matrix:** first full [3/10/30 run #40437015227](https://github.com/rydolabs/citoq/actions/runs/40437015227) **24/24 accepted**. Corrected Zenoh-SHM-disabled [run #40437891797](https://github.com/rydolabs/citoq/actions/runs/40437891797): 10-node and 30-node jobs each **8/8 accepted**. Any still-pending 3-node repeat should not be described as passed.
- **Mainline regression on the matrix branch:** [source/CTest](https://github.com/rydolabs/citoq/actions/runs/40437897858), [G1 native mTLS](https://github.com/rydolabs/citoq/actions/runs/40437897872), [G4 Python mTLS](https://github.com/rydolabs/citoq/actions/runs/40437898125), all passed.

All data were gathered on Ubuntu 24.04 GitHub Actions runners with separate Linux network namespaces joined by veth + Linux bridge, sharing one kernel CLOCK_MONOTONIC. Processes are **not independent physical hosts**, and the tests do not simulate a real RF medium.

## A. Shipping public API payload size: corrected CitoQ mTLS QUIC

Two repetitions per engine/size, each 200 application messages at 100 Hz, baseline with no deliberate netem loss/delay. Message content embeds sequence and timestamp; one-way **application-to-application** p95 and p99 include language, serialization and event-loop overhead. CitoQ drives `NativeRuntime.poll()` immediately **after each publish** to avoid previous 10 ms benchmark queueing error.

| Application bytes | CitoQ mTLS/QUIC p95 ms | Cyclone DDS RELIABLE UDP p95 ms | Zenoh peer TCP p95 ms | Fast DDS RELIABLE UDPv4 p95 ms |
| ---: | ---: | ---: | ---: | ---: |
| 64 | 0.708 | 0.677 | 0.246 | 0.598 |
| 256 | 0.702 | 0.659 | 0.232 | 0.592 |
| 1,024 | 0.694 | 0.691 | 0.231 | 0.607 |
| 4,096 | 0.722 | 0.697 | 0.191 | 0.551 |
| 16,384 | 0.782 | 1.150 | 0.221 | 0.601 |

These are the **median of two per-trial p95 values**, not pooled p95 and not a mean over all application messages.

| Application bytes | CitoQ bridge ingress bytes (median) | Cyclone DDS | Zenoh SHM-disabled | Fast DDS |
| ---: | ---: | ---: | ---: | ---: |
| 64 | 110,778 | 47,385 | 36,493 | 68,707 |
| 256 | 149,588 | 85,884 | 75,191 | 106,939 |
| 1,024 | 302,346 | 240,486 | 228,800 | 260,767 |
| 4,096 | 951,455 | 871,969 | 849,650 | 889,757 |
| 16,384 | 3,475,322 | 3,431,842 | 3,307,954 | 3,413,051 |

**Interpretation.** Across this tested fixed-rate workload, the CitoQ **shipping Context+mTLS/QUIC** p95 ranged ~0.69–0.78 ms, not the older ~10.65ms caused by poll-before-publish. Measured ingress **includes discovery + all control + QUIC handshake + payload**, so it is not "application-only payload overhead." CitoQ sends more bytes than the peers in this short fixed-message run, despite the separate internal UDP selective-interest prototype's efficiency results. This is not a protocol ranking: CitoQ encrypts its public connection, while other peers use different reliability, transport and language bindings. Two repetitions and 200 samples per trial are not robust p99/production evidence.

**Not a maximum-throughput test.** The publisher is deliberately paced at 100 Hz: nominal app payload rates were 0.0512, 0.2048, 0.8192, 3.2768 and 13.1072 Mbps for 64B, 256B, 1KiB, 4KiB and 16KiB respectively. These rates were *offered*, not discovered by saturating a transport until overload.

### Invalid first attempt and SHM correction

The **first** 40/40 delivered payload sweep, [run #40437015103](https://github.com/rydolabs/citoq/actions/runs/40437015103), left Zenoh's automatic same-host shared-memory optimization enabled. In that run, 4KiB/16KiB Zenoh bridge ingress inexplicably showed **33,769 / 59,155 bytes**, below the 200× application payload bytes. This is consistent with Zenoh 1.10's automatic SHM promotion at a 3,072-byte threshold. Linux **network namespaces do not isolate host IPC**, so this invalidated network-only traffic comparisons at those payload sizes. The harness now calls `cfg.insert_json5("transport/shared_memory/enabled", "false")` for Zenoh (and Fast DDS already forced UDPv4-only), yielding **849,650 / 3,307,954 bytes** at 4KiB/16KiB on the passing corrected run. References: [Zenoh 1.10 release note](https://zenoh.io/blog/2026-08-17-zenoh-1.10/) and [Zenoh Config](https://docs.rs/zenoh/latest/zenoh/struct.Config.html). Do not use the earlier off-veth measurements as proof of Zenoh's on-wire efficiency.

## B. Real CitoQ public mTLS QUIC: N-node publisher readiness

One *already-running* set of N-2 interested subscribers and one noninterested subscriber, each with its own network namespace, listening QUIC server, discovery announcements and ephemeral test certificate. Start one public-API publisher afterwards and record elapsed time from its advertise/loop start to `Context.publish_raw()` reporting all **N-2** interested destinations. Publisher then sends eight unique messages to each interested reader; noninterested receiver should receive zero. All identities are mutually authenticated with the benchmark's ephemeral test CA.

| Total nodes | Interested readers | Sender readiness (ms, single trial) | Actual / expected deliveries | Uninterested received | Full bridge ingress bytes |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 3 | 1 | 150.8 | 8/8 | 0 | 66,708 |
| 10 | 8 | 126.8 | 64/64 | 0 | 330,576 |
| 30 | 28 | 129.8 | 224/224 | 0 | 1,075,581 |

Readiness is measured **after the sender's initial native runtime construction** and **after receivers started**, not simultaneous whole-system cold start. It includes sending probing publications to assess the remote-destination count; the bytes observed before readiness therefore include **probe data, announcements, TLS handshakes and control**. These timings are **not directly comparable** to SPDP/SEDP readiness from other drivers. **One repetition per N** is a functional validation/pilot, not a robust statistical convergence study.

## C. N-node selective-interest research UDP adapters — different CitoQ stack!

One publisher, N-2 matching subscribers and one unrelated subscriber; four topics, two subscribed (50%), 256B application messages, four samples per topic at 10Hz, two repetitions per implementation. CitoQ is **`citoq::detail::UdpRuntimeAdapter`**, **not** shipping `Context` / native QUIC or mTLS. Cyclone and Fast DDS use BEST_EFFORT UDP; Zenoh peer TCP is not equivalent QoS.

**Median host-veth RX bytes accumulated before the publisher prints its local "ready" line** (approximate 50ms sampling; includes the subscriber setup period, announcements and other prepublication traffic):

| Nodes | CitoQ internal UDP | Cyclone DDS | Zenoh TCP (SHM off) | Fast DDS |
| ---: | ---: | ---: | ---: | ---: |
| 3 | 1,897* | 16,413* | 7,417* | 56,061* |
| 10 | 9,532 | 275,813 | 202,878 | 1,289,679 |
| 30 | 33,887 | 4,054,755 | 4,140,397 | 23,000,965 |

*The 3-node values are from the initial 24/24 successful run where Zenoh's implicit SHM setting was not yet overridden. Messages are 256B, below Zenoh's 3,072B implicit promotion threshold; keep provenance distinct rather than merging unverified results into the corrected cohort.*

**Median publisher-local readiness (ms), from process start, after receivers were started**:

| Nodes | CitoQ UDP prototype | Cyclone DDS | Zenoh peer | Fast DDS |
| ---: | ---: | ---: | ---: | ---: |
| 3 | 81.5* | 2.0* | 2.5* | 38.5* |
| 10 | 61.5 | 3.5 | 2.0 | 85.5 |
| 30 | 151.5 | 37.5 | 9.5 | 1,611.5 |

**Do not rank "fastest Discovery" from this table:** DDS endpoints, Zenoh `matching_status`, and CitoQ interest routing mark readiness differently; startup segments are not normalized. For 30 nodes, **all four engines delivered 448/448 across their two runs**, and the unrelated subscriber received zero application messages.

## Recommended peer group and next acceptance tests

- **Direct Pub/Sub:** Cyclone DDS and Fast DDS (SPDP/SEDP comparison), plus Zenoh peer (gossip/multicast scouting and interest-routing). They illuminate two distinct discovery architectures rather than duplicating only one.
- **Separate RPC streaming baseline:** gRPC + Protobuf with a clearly specified DNS or xDS backend, TLS, streaming topology and connection warmup. gRPC+Protobuf is not inherently a decentralized peer Pub/Sub discovery stack; **no comparable gRPC measurements were fabricated**.
- **IoT broker-mode optional comparator:** MQTT broker, only with separate broker availability, security and hop-count accounting.

To support a strong performance/Discovery product claim, add cold-start coordination and standardized "last interested reader receives first probe" convergence; report genuine metadata-only capture, never confuse control with data. Add network-loss + churn/reconnect, per-topic partial subscriptions at 10/30/100+ nodes using the **public CitoQ API**, comparable encryption/reliability/QoS, at least 5 repetitions and realistic payload entropy; run throughput-to-saturation sweeps rather than calling 100Hz a "throughput benchmark."
