#!/usr/bin/env python3
"""Strict A/B validation for MsQuic StreamSend(START|FIN), not a speedup assertion."""
import argparse
import json
import pathlib
import statistics
import sys

GROUPS = ("baseline-a", "candidate-a", "candidate-b", "baseline-b", "zenoh")

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--root",type=pathlib.Path,required=True)
    parser.add_argument("--repeats",type=int,default=3)
    args=parser.parse_args()
    failures=[]
    out={"groups":{}, "assumptions":[
        "Public Context+NativeRuntime mTLS/QUIC; Zenoh unencrypted TCP, SHM disabled",
        "200 payloads × 256 bytes at paced 100Hz, no imposed netem; per-cohort 3 trials",
        "CitoQ candidates are identical except compile-time START flag, and receiver event-driven poll",
        "Latency is timestamp-in-payload to callback, not a network-only metric",
        "Benchmark variance is expected; one successful CI run cannot prove speedup",
    ]}
    for group in GROUPS:
        file=args.root/group/"summary.json"
        if not file.is_file():
            failures.append(f"{group}: summary missing")
            continue
        engine="zenoh" if group=="zenoh" else "citoq"
        rows=[r for r in json.loads(file.read_text()) if r.get("engine")==engine and r.get("profile")=="baseline"]
        if len(rows)!=args.repeats or {r.get("repetition") for r in rows}!=set(range(1,args.repeats+1)):
            failures.append(f"{group}: invalid number/repetition IDs")
        good=[]
        for r in rows:
            if (r.get("accepted") is not True or r.get("received_unique")!=200 or
                    r.get("missing")!=0 or r.get("duplicates")!=0 or r.get("invalid")!=0 or
                    r.get("sender_exit")!=0 or r.get("receiver_exit")!=0 or
                    r.get("payload_bytes")!=256 or
                    (engine=="citoq" and r.get("citoq_rx_mode")!="event")):
                failures.append(f"{group}: invalid trial: {r}")
            else:
                good.append(r)
        def med(key,scale=1):
            arr=[x.get(key) for x in good]
            return round(statistics.median(arr)/scale,6) if len(arr)==args.repeats and all(isinstance(v,(int,float)) for v in arr) else None
        out["groups"][group]={
            "accepted_trials":len(good),"requested_trials":args.repeats,
            "p50_ms":med("latency_us_p50",1000),
            "p95_ms":med("latency_us_p95",1000),
            "p99_ms":med("latency_us_p99",1000),
            "receiver_cpu_s":med("receiver_cpu_s"),"sender_cpu_s":med("sender_cpu_s")
        }
    def mean_p95(prefix):
        vals=[out["groups"].get(prefix+x,{}).get("p95_ms") for x in ("-a", "-b")]
        return statistics.mean(vals) if all(isinstance(v,(int,float)) for v in vals) else None
    baseline,candidate=mean_p95("baseline"),mean_p95("candidate")
    if baseline and candidate:
        out["baseline_mean_cohort_p95_ms"]=round(baseline,6)
        out["candidate_mean_cohort_p95_ms"]=round(candidate,6)
        out["observed_p95_change_pct"]=round((candidate/baseline-1)*100,3)
        out["observed_candidate_under_0_250_in_both_cohorts"]=all(
            out["groups"]["candidate-"+x]["p95_ms"]<=0.250 for x in "ab"
        )
    out["errors"]=failures
    args.root.mkdir(parents=True,exist_ok=True)
    (args.root/"msquic-start-ab-report.json").write_text(json.dumps(out,indent=2)+"\n")
    print("MSQUIC_START_AB_REPORT="+json.dumps(out,sort_keys=True),flush=True)
    for e in failures: print("MSQUIC_START_AB_ERROR="+e,file=sys.stderr)
    return 1 if failures else 0

if __name__=="__main__":
    sys.exit(main())
