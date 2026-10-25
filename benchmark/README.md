# Pinned CitoQ / CycloneDDS / Zenoh / Fast DDS comparison

- Pinned CitoQ source: \`08188eba73284dd1069016174ad216f5e02d2cfa\`; harness commits are separate.
- Ubuntu 24.04 Actions runner, two Linux network namespaces, veth, Linux Bridge, optional bidirectional \`tc netem delay 10ms 2ms loss 1%\`.
- Protocols are not equivalent: CitoQ native mTLS/QUIC, Zenoh peer TCP, CycloneDDS RELIABLE RTPS/UDP, Fast DDS RELIABLE RTPS/UDP. No native-engine ranking without controlling serialization, threads and security.
- 256-byte application payload, 100 messages/second, 200 sequenced unique messages, 2 replicates/profile. One-way *application* latency uses CLOCK_MONOTONIC shared across namespace processes on the same kernel. Discovery/warmup bytes are included in host-veth byte counters.
- Run artifacts include per-process exit status, expected/received/missing/duplicate, p50/p95/p99 and bridge RX/TX. A failed adapter is not recorded as a measured zero.
- Fast DDS native driver will be added and separately validated; the first workflow targets CitoQ, CycloneDDS and Zenoh.

This is a functional measurement under an emulated network, not proof of real wireless performance or production maturity.
