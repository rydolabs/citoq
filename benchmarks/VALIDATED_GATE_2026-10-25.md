# Pinned CitoQ comparative validation — first complete five-variant run

- **Product source pinned SHA:** `08188eba73284dd1069016174ad216f5e02d2cfa`
- **First complete CI:** [GitHub Actions #40162062348](https://github.com/rydolabs/citoq/actions/runs/40162062348)
- **Evidence:** attached Actions artifact includes all per-process logs and `results.json` / `summary.csv`, including unsuccessful preliminary attempts.
- **OS:** Ubuntu 24.04 hosted Linux runner; isolated Linux network namespace per process, bridge and veth pair; bridge multicast snooping disabled.
- **Workload:** 10 nodes, one publisher, eight positive subscribers, one unrelated subscriber; 8 topics, approximately 10% topic interest, payload 256B, 8 samples per topic, 10Hz per topic. **Single repetition**, not a performance confidence interval.
- **Impairment:** 10Mbps per-interface netem cap, 20ms modeled one-way delay, 1% modeled path packet loss. Not equivalent to a shared radio channel.

## First successful run (one repetition each)

| Variant | Normal delivery | Normal host veth RX bytes | Normal max subscriber p95 (µs) | Impaired delivery | Impaired host veth RX bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| CitoQ experimental UDP adapter | 64/64 | 45,134 | 879 | 64/64 | 47,800 |
| Cyclone DDS 0.10.4 BEST_EFFORT | 64/64 | 367,832 | 1,400 | 64/64 | 429,548 |
| Fast DDS 2.11.2 BEST_EFFORT UDP-only | 64/64 | 806,444 | 1,207 | 64/64 | 989,896 |
| Zenoh 1.7.1 Peer via Python SDK | 64/64 | 55,887 | 896 | 64/64 | 59,733 |
| Zenoh 1.7.1 embedded Router/Client via Python SDK | 64/64 | 55,686 | 955 | 64/64 | 55,627 |

**Interpretation:** this one-run small workload demonstrates that all five setups can communicate through isolated namespaces and the instrumentation collects data. It does **not** prove a global ranking or production readiness. The different transport, discovery readiness semantics, sample encoding, QoS and binding language mean byte/CPU/RSS/latency values are not normalized for an isolated protocol overhead comparison.

- CitoQ: pre-release `UdpRuntimeAdapter` direct-path benchmark, **not the public Context MsQuic/mTLS transport**.
- Cyclone DDS / Fast DDS: C/C++ BEST_EFFORT + KEEP_LAST; Fast DDS shared-memory transport explicitly disabled so traffic goes through veth. Different RTPS implementations and discovery defaults.
- Zenoh Peer: explicit peer-to-hub TCP connections without multicast scouting. Router/Client: router session embedded in publishing process, not a separately measured `zenohd` daemon. Python interpreter cost is included; not directly comparable with C++-only implementations.
- Host-side veth RX and TX include discovery/control/data and Linux multicast/broadcast replication. CPU is summed process user/system time; RSS is sum of per-process peak RSS, not PSS.
- Discovery readiness is verified differently by each driver (route count, DDS matching count, or Zenoh subscriber readiness announcements) and must not be ranked by a single milliseconds column.
- Exactly 64 messages per trial is too few to establish reliability probability in 1% loss. No DDS RELIABLE vs QUIC stream matched-QoS evaluation here.
- Results are constrained to GitHub Actions virtualization; no RF/shared-airtime, 100-node all-matched DDS tuning, or long-run fault recovery claim.

Historical early runs #40160995427, #40161288451, #40161367750, and #40161717263 were **instrumentation failures or invalid trials** used to repair benchmark code, not evidence of competitor limitations. The complete run above is the first five-variant functional gate.
