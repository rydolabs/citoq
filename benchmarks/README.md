# CitoQ comparative benchmark (experimental)

Pinned tested production rename baseline: `08188eba73284dd1069016174ad216f5e02d2cfa`. Benchmark code lives on a separate branch and **must not change the release source**.

Linux Ubuntu 24.04 privileged CI uses per-process network namespaces, veth links, one Linux bridge, and optional symmetric `tc netem`. Metrics: host veth RX/TX, prepublication veth traffic, RSS sum, CPU, discovery-ready, sent/received, bad/duplicate, latency p95. Results and individual logs uploaded as artifacts. Treat failures/timeouts as invalid, not a performance ranking.

**Scope limitation:** the CitoQ driver compiled against this commit uses an internal UDP discovery adapter, not the deployed MsQuic/mTLS public Context transport. The results therefore compare a *specific CitoQ UDP adapter* with competitor profiles, not comprehensive production middleware performance. Baseline and limited profiles have different reliability semantics.

Raw measurement statistics include startup and data packets, and host-side virtual Ethernet counters count replicated multicast/broadcast traffic. Different wire serializations cannot be interpreted as equal protocol overhead. No inference of real RF airtime. Independent process/namespace resource figures are peak RSS sums (not PSS).
