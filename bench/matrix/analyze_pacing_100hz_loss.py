#!/usr/bin/env python3
"""Strict baseline and loss-matrix smoke for default rebased pacing."""
import argparse
import json
import pathlib
import statistics
import sys

CASES={"normal": "baseline","loss":"netem_10ms_1pct"}
COHORTS={
    "citoq-legacy":("citoq","legacy"),
    "citoq-default":("citoq","rebase"),
    "zenoh-quic-default":("zenoh","rebase"),
}
def median(rows,field,divisor=1):
    vals=[r.get(field) for r in rows]
    if not vals or not all(isinstance(x,(int,float)) for x in vals):
        return None
    return round(statistics.median(vals)/divisor,6)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--root",type=pathlib.Path,required=True)
    ap.add_argument("--repeats",type=int,default=2)
    args=ap.parse_args()
    errors=[]
    report={"config":{"payload_bytes":256,"rate_hz":100,"samples_per_trial":200,
       "network":"2 Linux namespaces / mTLS QUIC, 10ms±2ms with 1% loss profile when specified"},
       "scenarios":{},
       "limits":["The historical legacy scheduler is retained only for A/B.",
                 "No production mTLS/QUIC or message framing was changed.",
                 "Shared runner variation can dominate small p95 differences."]}
    for scenario,profile in CASES.items():
        report["scenarios"][scenario]={}
        for cohort,(engine,mode) in COHORTS.items():
            root=args.root/scenario/cohort
            p=root/"summary.json"
            if not p.exists():
                errors.append(f"missing {p}")
                continue
            rows=[x for x in json.loads(p.read_text()) if x.get("engine")==engine and x.get("profile")==profile]
            if len(rows)!=args.repeats or {x.get("repetition") for x in rows}!=set(range(1,args.repeats+1)):
                errors.append(f"{scenario}/{cohort} incomplete trials")
            pace=[]
            for row in rows:
                if not (row.get("accepted") is True and row.get("received_unique")==200 and
                        row.get("missing")==0 and row.get("duplicates")==0 and row.get("invalid")==0 and
                        row.get("sender_exit")==0 and row.get("receiver_exit")==0 and
                        row.get("payload_bytes")==256 and
                        (engine!="citoq" or row.get("citoq_rx_mode")=="event")):
                    errors.append(f"{scenario}/{cohort} invalid delivery: {row}")
                log=root/f"{profile}-{engine}-r{row.get('repetition')}-sender.log"
                if not log.exists():
                    errors.append(f"missing sender log: {log}")
                    continue
                traces=[x.split("PACING_REPORT=",1)[1] for x in log.read_text(errors="replace").splitlines()
                        if "PACING_REPORT=" in x]
                if len(traces)!=1:
                    errors.append(f"trace record count {log}: {len(traces)}")
                    continue
                record=json.loads(traces[0])
                if (record.get("mode")!=mode or record.get("count")!=200 or record.get("offered_hz")!=100):
                    errors.append(f"{scenario}/{cohort} wrong pacing mode/report {record}")
                if mode=="rebase" and record.get("short_gap_count",0)>2:
                    errors.append(f"{scenario}/{cohort} burst detected: {record}")
                pace.append(record)
            report["scenarios"][scenario][cohort]={
                "accepted_trials":sum(1 for x in rows if x.get("accepted") is True),
                "p95_ms":median(rows,"latency_us_p95",1000),
                "p99_ms":median(rows,"latency_us_p99",1000),
                "receiver_cpu_s":median(rows,"receiver_cpu_s"),
                "sender_cpu_s":median(rows,"sender_cpu_s"),
                "sender_observed_hz":median(pace,"observed_hz"),
                "short_gap_count":median(pace,"short_gap_count")}
    report["errors"]=errors
    args.root.mkdir(parents=True,exist_ok=True)
    (args.root/"pacing-100hz-loss-report.json").write_text(json.dumps(report,indent=2)+"\n")
    print("PACING_100HZ_LOSS_REPORT="+json.dumps(report,sort_keys=True),flush=True)
    for error in errors:print("PACING_100HZ_LOSS_ERROR="+error,file=sys.stderr)
    return int(bool(errors))
if __name__=="__main__":
    sys.exit(main())
