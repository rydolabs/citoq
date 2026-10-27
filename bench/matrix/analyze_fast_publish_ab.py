#!/usr/bin/env python3
"""Strict same-source MsQuic fast owner-thread send experiment validation.

Does not select the winner based on a single p95; publishes regression evidence.
"""
import argparse
import json
import pathlib
import statistics
import sys

SCENARIOS = (
    ("256b-100hz", 256, 100, 200, 3),
    ("256b-1000hz", 256, 1000, 400, 3),
    ("16k-100hz", 16384, 100, 200, 3),
)

def median(rows, key):
    values = [r.get(key) for r in rows]
    if not values or not all(isinstance(x, (float, int)) for x in values):
        return None
    return round(statistics.median(values), 6)

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--root", type=pathlib.Path, required=True)
    a = p.parse_args()
    errors = []
    report = {"conditions": {"runtime": "CitoQ Python public Context + mTLS/MsQuic",
                             "experimental_change": "Python bytes-pointer ctypes cast vs per-publish from_buffer_copy",
                             "qos_security": "unchanged within same-source CitoQ pairs",
                             "repeat_method": "3 independent runs, same GitHub runner"}, "scenarios": {}}
    for label, size, rate, count, repeats in SCENARIOS:
        variants = ("classic", "fast")
        if label == "256b-100hz":
            variants += ("zenoh",)
        cohorts = {}
        for variant in variants:
            path = a.root / label / variant / "summary.json"
            if not path.is_file():
                errors.append(f"missing {path}")
                continue
            data = json.loads(path.read_text())
            name = "zenoh" if variant == "zenoh" else "citoq"
            rows = [r for r in data if r.get("engine") == name]
            if len(rows) != repeats:
                errors.append(f"{label}/{variant} has {len(rows)} rows, wanted {repeats}")
            if {r.get("repetition") for r in rows} != set(range(1, repeats + 1)):
                errors.append(f"{label}/{variant}: missing or duplicate repetition")
            valid = []
            for r in rows:
                if (not r.get("accepted") or r.get("received_unique") != count or
                    r.get("payload_bytes") != size or r.get("expected") != count or
                    r.get("missing") != 0 or r.get("duplicates") != 0 or r.get("invalid") != 0 or
                    r.get("sender_exit") != 0 or r.get("receiver_exit") != 0 or
                    (name == "citoq" and r.get("citoq_rx_mode") != "event")):
                    errors.append(f"{label}/{variant}: failed trial: {r}")
                else:
                    valid.append(r)
            cohorts[variant] = {
                "accepted": len(valid), "expected": repeats,
                "p50_ms": (x / 1000 if (x:=median(valid,"latency_us_p50")) is not None else None),
                "p95_ms": (x / 1000 if (x:=median(valid,"latency_us_p95")) is not None else None),
                "p99_ms": (x / 1000 if (x:=median(valid,"latency_us_p99")) is not None else None),
                "rx_cpu_s": median(valid,"receiver_cpu_s"),
                "tx_cpu_s": median(valid,"sender_cpu_s"),
                "rx_rss_kb": median(valid,"receiver_max_rss_kb"),
                "tx_rss_kb": median(valid,"sender_max_rss_kb"),
            }
        if all(x in cohorts and cohorts[x]["p95_ms"] is not None for x in ("classic","fast")):
            baseline=cohorts["classic"]
            candidate=cohorts["fast"]
            cohorts["change"]={
                "p95_percent":round((candidate["p95_ms"]/baseline["p95_ms"]-1)*100,2),
                "p99_percent":round((candidate["p99_ms"]/baseline["p99_ms"]-1)*100,2),
                "cpu_rx_percent":round((candidate["rx_cpu_s"]/baseline["rx_cpu_s"]-1)*100,2)
                        if baseline["rx_cpu_s"] else None,
                "cpu_tx_percent":round((candidate["tx_cpu_s"]/baseline["tx_cpu_s"]-1)*100,2)
                        if baseline["tx_cpu_s"] else None,
                "candidate_met_p95_target":candidate["p95_ms"] <= 0.250 if label == "256b-100hz" else None,
            }
        report["scenarios"][label] = cohorts
    report["errors"] = errors
    report["limitations"] = [
        "Two veth-linked Linux network namespaces share kernel/CPU and monotonic clock.",
        "Zenoh TCP is not security/QoS equivalent to CitoQ mTLS/QUIC.",
        "Three trials per cohort are exploratory; no statistical equivalence claim.",
        "Maximum throughput, congestion control, loss recovery, and long-lived-stream behavior not established.",
    ]
    a.root.mkdir(parents=True,exist_ok=True)
    (a.root/"fast-send-ab-report.json").write_text(json.dumps(report,indent=2)+"\n")
    print("FAST_PUBLISH_AB_REPORT="+json.dumps(report,sort_keys=True),flush=True)
    for error in errors:print("FAST_PUBLISH_AB_ERROR="+error,file=sys.stderr,flush=True)
    return 0 if not errors else 1

if __name__=="__main__":
    sys.exit(main())
