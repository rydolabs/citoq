#!/usr/bin/env python3
"""Validate public API timestamp breakdown without claiming percentile additivity."""
import argparse
import json
import pathlib
import statistics
import sys

KEYS = ("latency_us_p50", "latency_us_p95", "latency_us_p99",
        "callback_entry_us_p95", "callback_decode_us_p95",
        "sender_pre_api_us_p95", "sender_api_us_p50", "sender_api_us_p95",
        "stage_successful_sends", "receiver_cpu_s", "sender_cpu_s")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=pathlib.Path, required=True)
    ap.add_argument("--repeats", type=int, default=3)
    args = ap.parse_args()
    report={"source":"Public application timestamps only; not packet-level QUIC profiling",
            "scope":"256B, 100Hz, 200 messages, mTLS/QUIC CitoQ vs non-TLS TCP Zenoh",
            "groups":{}}
    failures=[]
    for engine in ("citoq","zenoh"):
        path=args.root/engine/"summary.json"
        if not path.exists():
            failures.append("missing "+str(path));continue
        rows=json.loads(path.read_text())
        rs=[r for r in rows if r.get("engine")==engine]
        if len(rs)!=args.repeats or {r.get("repetition") for r in rs}!=set(range(1,args.repeats+1)):
            failures.append(f"{engine} wrong trial count")
        valid=[]
        for r in rs:
            bad=(not r.get("accepted") or r.get("received_unique")!=200 or
                 r.get("expected")!=200 or r.get("payload_bytes")!=256 or
                 r.get("missing")!=0 or r.get("duplicates")!=0 or r.get("invalid")!=0 or
                 r.get("sender_exit")!=0 or r.get("receiver_exit")!=0 or
                 r.get("stage_successful_sends")!=200 or
                 any(not isinstance(r.get(k),(int,float)) for k in KEYS))
            if bad:failures.append(f"{engine} invalid data: {r}")
            else:valid.append(r)
        report["groups"][engine]={
            "accepted":len(valid),"expected":args.repeats,
            "median_of_trial_metrics":{k:round(statistics.median(r[k] for r in valid),3)
                                      if len(valid)==args.repeats else None for k in KEYS}}
    report["cautions"]=[
       "p95 stage values cannot be subtracted from p95 application delay because tails may correspond to different messages.",
       "CitoQ sender_post_poll includes work between public API return and owner dispatch; Zenoh does not have explicit poll.",
       "Callback entry is first Python code in callback; callback decode is Python work until Collector.ingest.",
       "Synchronized monotonic clock only because both namespace processes share the same host kernel.",
       "Instrumentation overhead perturbs observed latency; same trace enabled on both engines.",
       "CitoQ mTLS/QUIC and Zenoh unencrypted TCP remain security and QoS different.",
    ]
    report["errors"]=failures
    (args.root/"app-stage-report.json").write_text(json.dumps(report,indent=2)+"\n")
    print("APP_STAGE_REPORT="+json.dumps(report,sort_keys=True),flush=True)
    for x in failures:print("APP_STAGE_ERROR="+x,file=sys.stderr,flush=True)
    return 0 if not failures else 1

if __name__=="__main__":
    sys.exit(main())
