#!/usr/bin/env python3
"""Strict public mTLS Pub/Sub reliability gate, no trace instrumentation needed."""
import argparse,json,pathlib,statistics,sys
def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--root",type=pathlib.Path,required=True)
    ap.add_argument("--profile",choices=["netem_10ms_1pct","netem_10ms_2pct"],required=True)
    ap.add_argument("--repeats",type=int,default=30)
    ap.add_argument("--count",type=int,default=200)
    args=ap.parse_args()
    errors=[]
    path=args.root/"summary.json"
    rows=json.loads(path.read_text()) if path.is_file() else []
    rows=[r for r in rows if r.get("engine")=="citoq" and r.get("profile")==args.profile]
    if len(rows)!=args.repeats or {r.get("repetition") for r in rows}!=set(range(1,args.repeats+1)):
        errors.append(f"Missing trials: {len(rows)}/{args.repeats}")
    accepted=[]
    failures=[]
    for r in rows:
        ok=(r.get("accepted") is True and r.get("received_unique")==args.count and
            r.get("expected")==args.count and r.get("missing")==0 and
            r.get("duplicates")==0 and r.get("invalid")==0 and
            r.get("sender_exit")==0 and r.get("receiver_exit")==0 and
            r.get("payload_bytes")==256 and r.get("citoq_rx_mode")=="event")
        if ok:
            accepted.append(r)
        else:
            failures.append(r)
    def median(key,divide=1):
        vals=[r[key] for r in accepted if isinstance(r.get(key),(int,float))]
        return round(statistics.median(vals)/divide,6) if len(vals)==len(accepted) and vals else None
    report={"profile":args.profile,"requested_trials":args.repeats,
            "attempted_trials":len(rows),"accepted_trials":len(accepted),
            "messages_per_trial":args.count,"delivered_valid":len(accepted)*args.count,
            "p95_ms_median_of_accepted":median("latency_us_p95",1000),
            "p99_ms_median_of_accepted":median("latency_us_p99",1000),
            "receiver_cpu_s_median":median("receiver_cpu_s"),
            "sender_cpu_s_median":median("sender_cpu_s"),
            "failures":[{"repetition":r.get("repetition"),"received_unique":r.get("received_unique"),
                         "missing":r.get("missing"),"sender_exit":r.get("sender_exit"),
                         "receiver_exit":r.get("receiver_exit"),"error":r.get("error")} for r in failures],
            "limitations":[
                "Single shared GitHub Actions VM per profile, 30 distinct process pairs, not 30 hosts.",
                "100Hz is a paced offered workload, not maximum throughput.",
                "The public publish return value is not remote delivery acknowledgement.",
                "A single missing sample, duplicate, corruption or nonzero exit fails the gate."
            ],"errors":errors}
    report["passed"]=(not failures and not errors and len(accepted)==args.repeats)
    args.root.mkdir(parents=True,exist_ok=True)
    (args.root/"loss-reliability-report.json").write_text(json.dumps(report,indent=2)+"\n")
    print("CITOQ_LOSS_RELIABILITY_REPORT="+json.dumps(report,sort_keys=True),flush=True)
    for e in errors:print("CITOQ_LOSS_RELIABILITY_ERROR="+e,file=sys.stderr)
    return int(not report["passed"])
if __name__=="__main__":
    sys.exit(main())
