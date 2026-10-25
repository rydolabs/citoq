# CitoQ / Cyclone DDS / Zenoh / Fast DDS comparison

## Pinned baseline
CitoQ source: `08188eba73284dd1069016174ad216f5e02d2cfa` (2026-10-25 main). The harness branch starts at this exact commit. No CitoQ benchmark results are claimed from a topology-only run.

## Network environment
GitHub Actions `ubuntu-24.04`, two Linux network namespaces, veth endpoints attached to a Linux Bridge, bidirectional `tc netem` delay 10ms ±2ms and loss 1%. The workflow archives namespace, bridge, and qdisc evidence.

## Comparison protocol (not yet implemented)
For **each** CitoQ, Eclipse Cyclone DDS, Eclipse Zenoh and eProsima Fast DDS, pin upstream release/source SHA and document runtime configuration. Run separate processes across namespaces on identical network, payloads (64B, 1KiB, 64KiB), message counts (warmup 1000 + measured 10000), QoS/reliability mode, publisher rate and sample size. Measure delivered count, end-to-end latency p50/p95/p99, sustained throughput, CPU and RSS; run 5 replicates with order randomized. Include unshaped baseline and netem delay/loss profiles, with raw CSV, process exit codes, topology and tool versions uploaded. Disclose transports and discovery behavior; exclude incompatible QoS modes from like-for-like rankings.

**Status:** topology infrastructure only. There is no defensible middleware performance comparison until all four adapters and the measurement harness execute successfully. Do not treat network ping as middleware throughput or latency.
