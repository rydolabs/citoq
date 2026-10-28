#!/usr/bin/env python3
"""A/B check for 1000 real public API messages at 1000 Hz per independent trial."""
import argparse
import json
import pathlib
import statistics
import sys

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--root",type=pathlib.Path,required=True)
    ap.add_argument("--repeats",type=int,default=3)
    args=ap.parse_args()
    errors=[]
    report={"groups":{},"requirements":{
        "payload_bytes":256,"messages":1000,"rate_hz":1000,
        "transport":"native authenticated mTLS MsQuic, public Python Context",
        "runner":"single Ubuntu 24.04, two veth linked Linux netns",
    },"limitations":[
        "Offered 1000 Hz not saturation throughput; send rate pacing and scheduling can vary.",
        "Mean of two cohort medians does not establish statistical significance.",
        "Requires separate 100Hz/p99/loss smoke validation before enabling in production.",
    ]}
    for cohort in ("baseline-a","candidate-a","candidate-b","baseline-b"):
        p=args.root/cohort/"summary.json"
        if not p.is_file():
            errors.append(cohort+": missing summary")
            continue
        rows=[r for r in json.loads(p.read_text()) if r.get("engine")=="citoq" and r.get("profile")=="baseline"]
        if len(rows)!=args.repeats or {r.get("repetition") for r in rows} != set(range(1,args.repeats+1)):
            errors.append(cohort+": incorrect repetition coverage")
        good=[]
        for r in rows:
            if not (r.get("accepted") is True and r.get("received_unique")==1000 and
                    r.get("missing")==0 and r.get("duplicates")==0 and r.get("invalid")==0 and
                    r.get("sender_exit")==0 and r.get("receiver_exit")==0 and
                    r.get("payload_bytes")==256 and r.get("citoq_rx_mode")=="event"):
                errors.append(cohort+": delivery/security/metadata invalid: "+str(r))
            else:good.append(r)
        def med(k,div=1):
            vals=[x.get(k) for x in good]
            return round(statistics.median(vals)/div,6) if len(vals)==args.repeats and all(isinstance(v,(int,float)) for v in vals) else None
        report["groups"][cohort]={
            "accepted":len(good),"expected":args.repeats,
            "p95_ms":med("latency_us_p95",1000),
            "p99_ms":med("latency_us_p99",1000),
            "receiver_cpu_s":med("receiver_cpu_s"),
            "sender_cpu_s":med("sender_cpu_s"),
            "receiver_rss_kb":med("receiver_max_rss_kb"),
        }
    for variant in ("baseline","candidate"):
        a=report["groups"].get(variant+"-a",{}).get("p95_ms")
        b=report["groups"].get(variant+"-b",{}).get("p95_ms")
        if isinstance(a,(int,float)) and isinstance(b,(int,float)):
            report[variant+"_mean_of_cohort_median_p95_ms"]=round(statistics.mean((a,b)),6)
    x=report.get("baseline_mean_of_cohort_median_p95_ms")
    y=report.get("candidate_mean_of_cohort_median_p95_ms")
    if x and y:
        report["candidate_p95_change_pct"]=round((y/x-1)*100,3)
    report["errors"]=errors
    args.root.mkdir(parents=True,exist_ok=True)
    (args.root/"longrate-report.json").write_text(json.dumps(report,indent=2)+"\n")
    print("LONG_RATE_REPORT="+json.dumps(report,sort_keys=True),flush=True)
    for e in errors:print("LONG_RATE_ERROR="+e,file=sys.stderr)
    return int(bool(errors))
if __name__=="__main__":
    sys.exit(main())
