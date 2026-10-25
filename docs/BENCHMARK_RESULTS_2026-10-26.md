# CitoQ benchmarking: measured results and release limitations

**Product source pin:** `08188eba73284dd1069016174ad216f5e02d2cfa`.
All product source/header/CMake files are unchanged by these benchmark-branch commits.

## Updated audit — Fast DDS **UDPv4-only** corrected 40-case test

**Supersedes the earlier 40-case results table below.** The earlier run #40241514977 did not explicitly disallow Fast DDS built-in SHM; network namespaces on one Linux runner can share IPC despite separating network stacks, so its Fast DDS traffic and latency are potentially confounded.

[Full successful re-run #40242077430](https://github.com/rydolabs/citoq/actions/runs/40242077430) and [raw 40-trial artifact](https://github.com/rydolabs/citoq/actions/runs/40242077430/artifacts/12617745758). Fast DDS is now forced to UDPv4-only by disabling built-in transports and registering `UDPv4TransportDescriptor`. Five repetitions per implementation per profile, 3 nodes, 4 topics, nominal 256B, 8 samples/topic, 10Hz. `valid` still does **not** mean fully delivered.

| Engine | Network | Actual / expected deliveries | Mean veth RX bytes | Mean subscriber p95 us | Sum peak RSS KB |
|---|---|---:|---:|---:|---:|
| CitoQ internal UDP (not SDK/QUIC) | baseline | 80/80 | 13,278.4 | 87.0 | 12,752.8 |
| Cyclone DDS BEST_EFFORT | baseline | 80/80 | 31,645.6 | 156.4 | 23,062.4 |
| Zenoh peer TCP | baseline | 80/80 | 17,834.6 | 308.2 | 74,088.8 |
| Fast DDS BEST_EFFORT UDP-only | baseline | 80/80 | 57,129.6 | 868.0 | 47,204.0 |
| CitoQ internal UDP (not SDK/QUIC) | limited | 80/80 | 13,577.6 | 55,398.4 | 12,977.6 |
| Cyclone DDS BEST_EFFORT | limited | 80/80 | 50,570.4 | 56,207.4 | 23,032.8 |
| Zenoh peer TCP | limited | 80/80 | 20,431.8 | 82,677.6 | 74,389.6 |
| Fast DDS BEST_EFFORT UDP-only | limited | 78/80 | 72,723.6 | 58,062.4 | 47,202.4 |

**Interpretation:** CitoQ's experimental internal UDP driver consumes fewer measured host-side bridge ingress bytes within this specific small 3-node workload; this is **not a general performance claim about the pinned CitoQ shipping API**. Zenoh TCP transport is semantically different from BEST_EFFORT UDP. The 1Mbps/50ms/1%-loss simulation is not shared RF airtime, and the sample is too small for robust p99. Do not rank overall engine quality, reliability or discovery from these numbers.

## Verified Linux netns infrastructure

Ubuntu 24.04 GitHub Actions host, independent Linux network namespaces connected by veth + Linux bridge, traffic shaping using `tc netem`. [Topology CI success](https://github.com/rydolabs/citoq/actions/runs/40240361702) after fixing an invalid `ip -n ... tc` command.

## Four-engine M26 native adapter A/B — **40 of 40 valid runs**

[Completed unified benchmark Actions run #40241514977](https://github.com/rydolabs/citoq/actions/runs/40241514977); [raw evidence ZIP artifact](https://github.com/rydolabs/citoq/actions/runs/40241514977/artifacts/12617370647): 289 files, aggregate `comparison.csv`, `comparison.json`, all-trials CSV, individual per-node stdout/stderr, resource files and network measurements.

3 netns nodes (publisher, matched subscriber, unrelated subscriber), 4 topics, 50% subscribed, 256B nominal payload, 8 messages/topic, 10Hz/topic, **5 engine runs for each profile**. Profiles are unshaped baseline and 1 Mbps/per-interface + 50ms nominal one-way/end-to-end delay profile + 1% loss as defined by the original harness. Measurements are host-side veth RX including control and user data.

| Engine | Profile | Matched deliveries | Mean veth RX (bytes) | Mean subscriber p95 (us) | Sum process peak RSS mean (KB) |
|---|---|---:|---:|---:|---:|
| CitoQ **internal UDP adapter** | baseline | 80/80 | 13,330.8 | 98.8 | 12,736.8 |
| Cyclone DDS BEST_EFFORT | baseline | 80/80 | 31,804.0 | 145.4 | 23,052.8 |
| Zenoh Peer | baseline | 80/80 | 17,478.6 | 299.6 | 74,083.2 |
| Fast DDS BEST_EFFORT | baseline | 80/80 | 59,574.8 | 923.2 | 50,936.0 |
| CitoQ **internal UDP adapter** | limited | 79/80 | 14,181.2 | 55,519.8 | 12,866.4 |
| Cyclone DDS BEST_EFFORT | limited | 79/80 | 51,030.4 | 56,086.2 | 23,033.6 |
| Zenoh Peer | limited | 80/80 | 20,326.0 | 55,648.2 | 74,594.4 |
| Fast DDS BEST_EFFORT | limited | 78/80 | 74,656.0 | 58,139.0 | 50,974.4 |

`valid` means harness lifecycle/counters/negative-interest checks passed, **not loss-free**: limited-profile BEST_EFFORT dropped packets in some trials. Receiver p95 is aggregated from a small number of application samples, not a large statistically robust latency population.

**Fairness limitations:** CitoQ is `citoq::detail::UdpRuntimeAdapter` manually driven over raw UDP sockets, **not the CitoQ installed C++/C/Python public API or native mTLS/QUIC**. Zenoh Peer default transport/reliability differs from UDP BEST_EFFORT. Process implementations differ (Python vs C/C++), payload wire serialization is not identical; thus no general product performance ranking can be made. Publisher readiness timing includes different internal setup boundaries; don't rank discovery from it. Processes are Linux namespaces sharing a kernel clock, not independent RF network nodes. Peak RSS is not PSS.

## Separate shipping API / encrypted transport experiment

The independent `benchmark/` harness exercises the **CitoQ public Python Context API plus native mTLS/QUIC**, Cyclone DDS Python RELIABLE, Zenoh Peer/TCP, and Fast DDS native RELIABLE. Same Linux bridge/netns and shared monotonic clock but **different security, transport and QoS profiles**. 200 messages × 256B × 100Hz, two repetitions per profile.

[Actual public-API comparison run #40241924322](https://github.com/rydolabs/citoq/actions/runs/40241924322): 12/16 cases fully delivered and passed process/uniqueness checks. 8/8 baseline passed. In the bidirectional 10ms±2ms loss1% `netem` profile, CitoQ QUIC and Zenoh each passed 2/2 (200/200 each), while Cyclone DDS Python RELIABLE received 197/200 and 194/200; Fast DDS native RELIABLE received 100/200 and 28/200. **These 4 cases are failures and must not be reported as success or middleware product limits.** DDS reader lifecycle handling and ACKs were investigated; Fast DDS history resource-limits were adjusted in subsequent commits for further investigation.

Two baseline example p95 latencies from that run (application-level, **not fair equivalent-transports comparisons**):
- CitoQ mTLS/QUIC: 10.615 and 10.704 ms;
- Cyclone DDS Python RELIABLE: 0.717 and 0.740 ms;
- Zenoh Peer/TCP: 0.289 and 0.245 ms;
- Fast DDS native RELIABLE: 0.608 and 0.633 ms.

The separate public API CitoQ experiment is an important check: its latency is **not the same as the internal UDP adapter's 98.8us**. Any external marketing or architecture conclusion needs full workload/QoS/security normalization.

## Next acceptance criteria

- Fix any benchmark adapter lifecycle and DDS history limits **without hiding packet-loss cases**, rerun same-SHA reproducibility.
- Compare CitoQ QUIC with similarly secured alternatives (encrypted transports where available) and compare BEST_EFFORT to BEST_EFFORT separately.
- Re-run all engines at 10/30/100+ nodes with heterogenous subscriptions, repeated cold-starts, fan-out and RSS/PSS, negative matching and disconnect/reconnect, sufficient payloads for robust p99.
- Preserve repeat count and exact pinned package versions and user-level/bridge-level measurements in raw artifacts.
- Keep the benchmark work in a draft PR until core source/product compatibility and comparison-methodology review.
