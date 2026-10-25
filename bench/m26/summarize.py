#!/usr/bin/env python3
"""Validate and summarize same-runner multi-engine M26 results; never impute missing runs."""
import argparse
import csv
import json
import statistics
from pathlib import Path

ENGINES = ("cito", "dds", "zenoh", "fastdds")
PROFILES = ("baseline", "limited")
SOURCE_PIN = "08188eba73284dd1069016174ad216f5e02d2cfa"


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--root", type=Path, required=True)
    p.add_argument("--repeats", type=int, default=5)
    p.add_argument("--nodes", type=int, default=3)
    args = p.parse_args()
    rows = []
    summaries = []
    for profile in PROFILES:
        path = args.root / profile / "results.json"
        if not path.is_file():
            raise RuntimeError(f"Missing experiment output {path}")
        current = json.loads(path.read_text())
        if len(current) != len(ENGINES)*args.repeats:
            raise RuntimeError(f"Expected {len(ENGINES)*args.repeats} rows in {path}; got {len(current)}")
        for r in current:
            if r.get("engine") not in ENGINES or r.get("profile") != profile:
                raise RuntimeError(f"Unexpected engine/profile in {path}: {r}")
            if not r.get("valid") or r.get("nodes") != args.nodes:
                raise RuntimeError(f"Invalid benchmark run: {r}")
            if r.get("publisher_sent") != r.get("topics") * r.get("samples"):
                raise RuntimeError(f"Unsent user data: {r}")
            if r.get("received", 0) > r.get("expected", -1):
                raise RuntimeError(f"Overcounted deliveries: {r}")
            if r.get("negative_received") != 0:
                raise RuntimeError(f"Unrelated subscription received data: {r}")
            rows.append(r)
        for engine in ENGINES:
            selected = [r for r in current if r["engine"] == engine]
            reps = {r["repeat"] for r in selected}
            if reps != set(range(1, args.repeats+1)):
                raise RuntimeError(f"Wrong repetition set: {engine} {profile} {sorted(reps)}")
            def average(key):
                return round(statistics.mean(r[key] for r in selected), 2)
            total_received = sum(r["received"] for r in selected)
            total_expected = sum(r["expected"] for r in selected)
            summaries.append({
                "engine": engine, "profile": profile, "nodes": args.nodes,
                "repeats": args.repeats, "total_received": total_received,
                "total_expected": total_expected,
                "delivered_fraction": round(total_received/total_expected, 6),
                "veth_rx_bytes_mean": average("veth_rx_bytes"),
                "veth_tx_bytes_mean": average("veth_tx_bytes"),
                "sum_max_rss_kb_mean": average("sum_max_rss_kb"),
                "subscriber_p95_max_us_mean": average("subscriber_p95_max_us"),
                "cpu_total_seconds_mean": round(statistics.mean(
                    r["cpu_user_s"]+r["cpu_system_s"] for r in selected), 4),
                "publisher_readiness_ms_mean": average("discovery_ready_ms")
            })
    args.root.mkdir(parents=True, exist_ok=True)
    with (args.root/"comparison.csv").open("w", newline="") as f:
        w=csv.DictWriter(f, fieldnames=list(summaries[0]))
        w.writeheader()
        w.writerows(summaries)
    with (args.root/"all-trials.csv").open("w", newline="") as f:
        keys=sorted(set().union(*(r.keys() for r in rows)))
        w=csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        w.writerows(rows)
    (args.root/"comparison.json").write_text(json.dumps({
        "source_pin": SOURCE_PIN,
        "engines": ENGINES, "profiles": PROFILES,
        "summary": summaries,
        "cautions": [
          "CitoQ adapter is citoq::detail::UdpRuntimeAdapter, not the public SDK/QUIC driver.",
          "Zenoh peer uses different transport reliability than BEST_EFFORT UDP peers.",
          "Nominal application payload bytes are the same, but on-wire serialization varies.",
          "RSS is summed process maximum RSS, not physical PSS.",
          "Publisher readiness is driver-scoped and includes different internal setup stages.",
          "This is a 3-node exploratory test; no general throughput or production ranking."
        ]},indent=2)+"\n")
    lines=["# CitoQ four-engine exploratory A/B results", "",f"Source CitoQ pin: \`{SOURCE_PIN}\`", "",
           "3 Linux namespaces on one Ubuntu Actions runner; same bridge/veth setup. Five repetitions per engine per profile.",
           "The baseline and 1Mbps/50ms/1%-loss profiles are different workloads. Engine QoS is not equivalent.",
           "", "| Engine | Profile | Received | veth RX mean bytes | p95 mean us | Sum peak RSS mean KB |",
           "|---|---|---:|---:|---:|---:|"]
    for r in summaries:
        lines.append(f"| {r['engine']} | {r['profile']} | {r['total_received']}/{r['total_expected']} | {r['veth_rx_bytes_mean']} | {r['subscriber_p95_max_us_mean']} | {r['sum_max_rss_kb_mean']} |")
    lines += ["", "## Important restrictions",
              "- CitoQ source path is an internal UDP adapter, not release public API or mTLS QUIC.",
              "- Zenoh default peer behavior uses a different transport/reliability profile than DDS/CitoQ UDP BEST_EFFORT.",
              "- Publisher readiness is not a normalized discovery convergence benchmark; do not rank from it.",
              "- Application payloads are nominally comparable but wire representation, runtimes and serialization differ.",
              "- Two namespace endpoints are not distinct physical hosts. No real RF/shared-airtime simulation.",
              "- Preserve all per-node logs and CSV artifacts for audit.", ""]
    (args.root/"COMPARISON.md").write_text("\n".join(lines))
    print(json.dumps({"source_pin":SOURCE_PIN,"summary":summaries},sort_keys=True),flush=True)


if __name__=="__main__":
    main()
