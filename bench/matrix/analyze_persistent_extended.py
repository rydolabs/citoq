#!/usr/bin/env python3
"""Strict multi-payload/high-rate/loss A/B for opt-in persistent CTD1 QUIC."""
import argparse
import json
import pathlib
import statistics
import sys

CASES=(
    ("size64",64,100,"baseline"),
    ("size16384",16384,100,"baseline"),
    ("rate1000",256,1000,"baseline"),
    ("loss",256,100,"netem_10ms_1pct"),
)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--root",type=pathlib.Path,required=True)
    ap.add_argument("--repeats",type=int,default=2)
    args=ap.parse_args()
    errors=[]
    result={"scenarios":{},"limits":[
        "256B/100Hz objective is measured independently in the core A/B workflow.",
        "All outputs use authenticated CitoQ public Context+MsQuic on veth netns.",
        "An offered rate is not proven sustained throughput.",
        "The persistent mode lacks mixed-version negotiation and HOL regression assurance.",
    ]}
    for name,size,rate,profile in CASES:
        result["scenarios"][name]={}
        for variant in ("baseline","candidate"):
            path=args.root/name/variant/"summary.json"
            if not path.is_file():
                errors.append(f"{name} {variant} missing summary")
                continue
            data=json.loads(path.read_text())
            rows=[r for r in data if r.get("engine")=="citoq" and r.get("profile")==profile]
            if len(rows)!=args.repeats or {r.get("repetition") for r in rows}!=set(range(1,args.repeats+1)):
                errors.append(f"{name}/{variant}: incomplete trials")
            for r in rows:
                if not (r.get("accepted") is True and r.get("received_unique")==200
                        and r.get("missing")==0 and r.get("duplicates")==0 and
                        r.get("invalid")==0 and r.get("payload_bytes")==size and
                        r.get("sender_exit")==0 and r.get("receiver_exit")==0 and
                        r.get("citoq_rx_mode")=="event"):
                    errors.append(f"{name}/{variant} invalid: {r}")
            def med(field,scale=1):
                values=[r.get(field) for r in rows]
                return round(statistics.median(values)/scale,6) if len(values)==args.repeats and all(isinstance(v,(int,float)) for v in values) else None
            result["scenarios"][name][variant]={
                "accepted_trials":sum(1 for r in rows if r.get("accepted") is True),
                "p95_ms":med("latency_us_p95",1000),
                "p99_ms":med("latency_us_p99",1000),
                "receiver_cpu_s":med("receiver_cpu_s"),
                "sender_cpu_s":med("sender_cpu_s"),
                "receiver_rss_kb":med("receiver_max_rss_kb"),
            }
    result["errors"]=errors
    args.root.mkdir(parents=True,exist_ok=True)
    (args.root/"extended-report.json").write_text(json.dumps(result,indent=2)+"\n")
    print("PERSISTENT_EXTENDED_REPORT="+json.dumps(result,sort_keys=True),flush=True)
    for e in errors:print("PERSISTENT_EXTENDED_ERROR="+e,file=sys.stderr)
    return int(bool(errors))
if __name__=="__main__":
    sys.exit(main())
