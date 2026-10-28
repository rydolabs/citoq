#!/usr/bin/env python3
"""Strict source-identical A/B for MsQuic MsQuic priority work at normal/high rate and loss."""
import argparse,json,pathlib,statistics,sys

SCENARIOS={
    "rate100":(256,"baseline"),
    "rate1000":(256,"baseline"),
    "size16384":(16384,"baseline"),
    "loss":(256,"netem_10ms_1pct"),
}
def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--root",type=pathlib.Path,required=True)
    ap.add_argument("--repeats",type=int,default=3)
    args=ap.parse_args()
    report={"scenarios":{},"limitations":[
        "Normal/candidate binaries differ only by compile-time CITOQ_QUIC_PRIORITY_WORK.",
        "One GitHub runner, veth namespaces, native mTLS, CitoQ public Context+Python callback.",
        "200/200 deliverability is strictly required; 1000Hz offered not saturation throughput.",
        "Failed loss/handshake trial remains failure, not removed from metrics.",
        "Do not enable by default without repeatable p95/p99 and loss/no-starvation assurance."
    ]}
    errors=[]
    for scenario,(size,profile) in SCENARIOS.items():
        report["scenarios"][scenario]={}
        for variant in ("baseline","candidate"):
            file=args.root/scenario/variant/"summary.json"
            if not file.is_file():
                errors.append(f"{scenario}/{variant}: missing summary")
                continue
            rows=[r for r in json.loads(file.read_text())
                  if r.get("engine")=="citoq" and r.get("profile")==profile]
            if len(rows)!=args.repeats or {r.get("repetition") for r in rows}!=set(range(1,args.repeats+1)):
                errors.append(f"{scenario}/{variant}: incomplete or duplicate trial IDs")
            good=[]
            for r in rows:
                if (r.get("accepted") is not True or r.get("received_unique")!=200 or
                    r.get("missing")!=0 or r.get("duplicates")!=0 or
                    r.get("invalid")!=0 or r.get("sender_exit")!=0 or
                    r.get("receiver_exit")!=0 or r.get("payload_bytes")!=size or
                    r.get("citoq_rx_mode")!="event"):
                    errors.append(f"{scenario}/{variant}: invalid trial {r}")
                else:good.append(r)
            def med(key,scale=1):
                vals=[r.get(key) for r in good]
                return round(statistics.median(vals)/scale,6) if len(vals)==args.repeats and all(isinstance(x,(int,float)) for x in vals) else None
            report["scenarios"][scenario][variant]={
                "accepted":len(good),"expected":args.repeats,
                "p95_ms":med("latency_us_p95",1000),
                "p99_ms":med("latency_us_p99",1000),
                "receiver_cpu_s":med("receiver_cpu_s"),
                "sender_cpu_s":med("sender_cpu_s"),
                "receiver_rss_kb":med("receiver_max_rss_kb"),
            }
        x=report["scenarios"][scenario].get("baseline",{}).get("p95_ms")
        y=report["scenarios"][scenario].get("candidate",{}).get("p95_ms")
        if x is not None and y is not None and x>0:
            report["scenarios"][scenario]["candidate_vs_baseline_p95_change_pct"]=round((y/x-1)*100,3)
    report["errors"]=errors
    args.root.mkdir(parents=True,exist_ok=True)
    (args.root/"quic-priority-report.json").write_text(json.dumps(report,indent=2)+"\n")
    print("QUIC_PRIORITY_REPORT="+json.dumps(report,sort_keys=True),flush=True)
    for err in errors: print("QUIC_PRIORITY_ERROR="+err,file=sys.stderr)
    return int(bool(errors))
if __name__=="__main__":
    sys.exit(main())
