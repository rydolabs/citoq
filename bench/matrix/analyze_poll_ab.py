#!/usr/bin/env python3
"""Strict CitoQ receiver poll-cadence A/B summary; diagnostic, not a vendor ranking.

Run groups with equal payload/rate/transport/cert setup. Verify every trial,
preserve raw failures, and report CPU tradeoffs alongside latency statistics.
"""
import argparse
import json
import pathlib
import statistics
import sys

GROUPS = ("citoq-500", "citoq-0", "citoq-100", "citoq-event", "citoq-500-repeat", "zenoh")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, required=True)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    errors = []
    result = {"conditions": {"payload_bytes": 256, "rate_hz": 100,
                             "receiver_sleep_us": [500, 0, 100, "event-driven", 500],
                             "citoq_transport": "authenticated QUIC with explicit poll",
                             "zenoh_transport": "TCP peer, SHM disabled"},
              "groups": {}}
    for name in GROUPS:
        file = args.root / name / "summary.json"
        if not file.is_file():
            errors.append(f"missing {file}")
            continue
        records = json.loads(file.read_text())
        engine = "zenoh" if name == "zenoh" else "citoq"
        expected_sleep = None if engine == "zenoh" else (500 if name in ("citoq-event", "citoq-500", "citoq-500-repeat") else int(name.split("-")[1]))
        expected_mode = "event" if name == "citoq-event" else "sleep"
        trials = [r for r in records if r.get("engine") == engine and r.get("profile") == "baseline"]
        if len(trials) != args.repeats:
            errors.append(f"{name}: {len(trials)} != {args.repeats} trials")
        if {r.get("repetition") for r in trials} != set(range(1, args.repeats + 1)):
            errors.append(f"{name}: missing/duplicate repetitions")
        valid = []
        for r in trials:
            if (not r.get("accepted") or r.get("received_unique") != 200 or
                    r.get("missing") != 0 or r.get("duplicates") != 0 or r.get("invalid") != 0 or
                    r.get("sender_exit") != 0 or r.get("receiver_exit") != 0 or
                    r.get("payload_bytes") != 256 or
                    (engine == "citoq" and (r.get("citoq_rx_sleep_us") != expected_sleep or
                                            r.get("citoq_rx_mode") != expected_mode))):
                errors.append(f"{name} rep {r.get('repetition')} failed strict validation: {r}")
            else:
                valid.append(r)
        def med(key):
            values = [r.get(key) for r in valid]
            if len(values) != args.repeats or not all(isinstance(v, (int, float)) for v in values):
                return None
            return round(statistics.median(values), 6)
        result["groups"][name] = {
            "accepted": len(valid), "total": args.repeats, "rx_sleep_us": expected_sleep,
            "rx_mode": expected_mode if engine == "citoq" else None,
            "p50_ms": (x / 1000 if (x := med("latency_us_p50")) is not None else None),
            "p95_ms": (x / 1000 if (x := med("latency_us_p95")) is not None else None),
            "p99_ms": (x / 1000 if (x := med("latency_us_p99")) is not None else None),
            "receiver_cpu_s": med("receiver_cpu_s"),
            "sender_cpu_s": med("sender_cpu_s"),
            "receiver_max_rss_kb": med("receiver_max_rss_kb"),
        }
    result["limitations"] = [
        "Only 100Hz, 256B, 200 messages, same-host veth namespaces.",
        "CitoQ uses mTLS QUIC, Zenoh uses unencrypted TCP; not equivalent security.",
        "Unbounded busy polling is a diagnostic and can consume an entire CPU core.",
        "Event-driven CitoQ callback polling retains single-thread ownership and keeps mTLS/QUIC enabled.",
        "The median of 3 trial-level percentiles is not a pooled latency percentile.",
        "The 500us repetition checks for temporal drift; no gain claim without evidence.",
    ]
    result["errors"] = errors
    args.root.mkdir(parents=True, exist_ok=True)
    (args.root / "poll-ab-report.json").write_text(json.dumps(result, indent=2) + "\n")
    print("POLL_AB_REPORT=" + json.dumps(result, sort_keys=True), flush=True)
    for error in errors:
        print("POLL_AB_ERROR=" + error, file=sys.stderr)
    return 0 if not errors else 1

if __name__ == "__main__":
    sys.exit(main())
