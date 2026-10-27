#!/usr/bin/env python3
"""Validate public API latency and summarize opt-in MsQuic stream API timings.

Records are diagnostic wall-clock API-call durations, not wire latency.
"""
import argparse
import json
import pathlib
import re
import statistics
import sys

PAT = re.compile(r"CITOQ_QUIC_STAGE stream_open_start_ns=(\d+) stream_send_ns=(\d+) queued=(\d+)")

def percentile(values, pct):
    sorted_values = sorted(values)
    return sorted_values[min(len(sorted_values)-1, round((len(sorted_values)-1)*pct))]

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--root",type=pathlib.Path,required=True)
    ap.add_argument("--repeats",type=int,default=3)
    args=ap.parse_args()
    errors=[]
    report={"trials":[],"limitations":[
        "Measured synchronous MsQuic StreamOpen+Start and StreamSend wall time, not network transit or end-to-end breakdown.",
        "Tracing writes to stderr on every message and can change the benchmark p95.",
        "CitoQ mTLS/QUIC and Zenoh non-TLS TCP have unequal security/QoS.",
        "The p95s are trial-local and the reported summary is median of trial p95s."
    ]}
    for engine in ("citoq","zenoh"):
        directory=args.root/engine
        summary=directory/"summary.json"
        if not summary.exists():
            errors.append("missing summary: "+str(summary))
            continue
        rows=[r for r in json.loads(summary.read_text()) if r.get("engine")==engine]
        if len(rows)!=args.repeats or {r.get("repetition") for r in rows}!=set(range(1,args.repeats+1)):
            errors.append(engine+" incomplete repetitions")
        for row in rows:
            rep=row["repetition"]
            if not (row.get("accepted") is True and row.get("received_unique")==200 and
                    row.get("missing")==0 and row.get("invalid")==0 and row.get("duplicates")==0 and
                    row.get("sender_exit")==0 and row.get("receiver_exit")==0 and
                    row.get("payload_bytes")==256):
                errors.append(f"{engine} repetition {rep} delivery/acceptance failed: {row}")
            entry={"engine":engine,"repetition":rep,"p95_ms":row.get("latency_us_p95",0)/1000,
                   "p99_ms":row.get("latency_us_p99",0)/1000}
            if engine=="citoq":
                logfile=directory/f"baseline-citoq-r{rep}-sender.log"
                if not logfile.exists():
                    errors.append("missing trace log: "+str(logfile))
                else:
                    samples=[tuple(map(int,m.groups())) for m in PAT.finditer(logfile.read_text(errors="replace"))]
                    good=[s for s in samples if s[2]==1]
                    if len(good)<200 or len(good)!=len(samples):
                        errors.append(f"CitoQ trace samples rep {rep}: {len(good)} success, {len(samples)} total; expected >=200 all success")
                    if good:
                        entry["trace_samples"]=len(good)
                        for idx,key in ((0,"open_start"),(1,"send")):
                            ns=[s[idx] for s in good]
                            entry[key+"_median_us"]=statistics.median(ns)/1000
                            entry[key+"_p95_us"]=percentile(ns,0.95)/1000
            report["trials"].append(entry)
    for engine in ("citoq","zenoh"):
        subset=[r for r in report["trials"] if r["engine"]==engine]
        if len(subset)==args.repeats:
            report[engine+"_median_trial_p95_ms"]=statistics.median(r["p95_ms"] for r in subset)
    report["errors"]=errors
    args.root.mkdir(parents=True,exist_ok=True)
    (args.root/"quic-stage-report.json").write_text(json.dumps(report,indent=2)+"\n")
    print("QUIC_STAGE_REPORT="+json.dumps(report,sort_keys=True),flush=True)
    for e in errors: print("QUIC_STAGE_ERROR="+e,file=sys.stderr)
    return bool(errors)

if __name__=="__main__":
    sys.exit(main())
