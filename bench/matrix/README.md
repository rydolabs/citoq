# Node count and payload size comparison (2026-10-27)

Product source pinned to `08188eba73284dd1069016174ad216f5e02d2cfa`. The benchmark fixes the CitoQ explicit polling order (after successful `Context.publish_raw` call, immediately service `NativeRuntime.poll()`), to avoid the previous 10ms 100Hz send-queue artifact.

## Two strictly separated experiments

**A. Payload-size (shipping API / transport):** 64, 256, 1024, 4096, 16384 application bytes, 100Hz, 200 sequence+monotonic timestamp samples, 2 repetitions, no imposed packet loss/delay. Same Linux runner, two netns linked via veth/bridge. CitoQ Python Context + native mTLS QUIC; CycloneDDS Python RELIABLE UDP; Zenoh Python Peer TCP; FastDDS native C++ RELIABLE UDPv4-only (built-in SHM off). Outputs: p95/p99 one-way application latency, unique delivery/duplicate/invalid rates, veth bytes, **paced nominal payload bitrate**, which must *not* be called maximum throughput. QoS, security, language bindings differ; figures are not a fair vendor ranking. Test strictly marks missing cases failed. Evidence workflow: `.github/workflows/benchmark-payload-size-matrix.yml`.

**B. Node-count (experimental discovery-readiness proxy):** 3, 10, 30 namespaces/nodes; 1 publisher + N-2 interested subscribers + 1 irrelevant subscriber; 4 topics, 50% interested, 256-byte messages, 4 samples/topic, 10Hz, 2 repetitions, unshaped. CitoQ `detail::UdpRuntimeAdapter` internal prototype vs CycloneDDS C BEST_EFFORT UDP, Zenoh peer TCP, FastDDS native C++ BEST_EFFORT UDP-only. Metrics: publisher-local `BENCH_DISCOVERY_READY` elapsed time after *its process launch*, prepublish veth RX control/warmup bytes sampled when that line is observed, total delivery, full trial traffic. **NOT a fully normalized discovery convergence metric, and not a measurement of the CitoQ public mTLS/QUIC SDK**. Because peers start before the publisher and Zenoh matching semantics differ, do not proclaim intrinsic discovery superiority. Evidence workflow: `.github/workflows/benchmark-node-discovery-matrix.yml`.

## Choosing peer systems

- **Cyclone DDS** and **Fast DDS** are direct DDS Pub/Sub baselines for SPDP/SEDP discovery and UDP delivery.
- **Zenoh peer mode** measures selective-interest and gossip/scouting approaches that are conceptually closer to CitoQ.
- **gRPC + Protobuf** should be a *separate* RPC/streaming baseline with **explicit DNS or xDS** service discovery. Protobuf alone is just serialization. gRPC is not a native peer Pub/Sub discovery competitor. No invented gRPC numbers.
- Future experiment: CitoQ public API 3/10/30 node real QUIC and similarly encrypted peers, synchronized first-interest-to-first-discovery timestamps, matching-control-only bytes, cold restart and churn, 3+ repetitions and variance, rate sweep to measured saturation throughput.

## Reproducibility

All raw logs, per-process results, package versions and failed cases remain in Actions artifacts. Never replace a failed 200/200 delivery with a successful-looking latency statistic.
