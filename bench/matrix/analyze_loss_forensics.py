#!/usr/bin/env python3
"""Strict 30-repetition authenticated CitoQ Pub/Sub loss forensics."""
import argparse
import json
import pathlib
import re
import statistics
import sys

TRACE = re.compile(r"CITOQ_LOSS_TRACE ([^\r\n]+)")
VALUE = re.compile(r"([a-z0-9_]+)=(\d+)")

def trace_from(path, errors):
    if not path.is_file():
        errors.append(f"Missing trace log {path}")
        return {}
    records = TRACE.findall(path.read_text(errors="replace"))
    if len(records) != 1:
        errors.append(f"Expected one native teardown snapshot in {path}, found {len(records)}")
        return {}
    return {k:int(v) for k,v in VALUE.findall(records[0])}

def classify(sender, receiver):
    if sender.get("client_connected",0)==0:
        return "client session never attached after mTLS handshake"
    if sender.get("enqueued",0)==0:
        return "publish succeeded without outbound data queued"
    if sender.get("send_accepted",0)==0:
        return "outbound MsQuic StreamSend not accepted"
    if receiver.get("server_accepted",0)==0:
        return "receiver has no inbound QUIC connection callback"
    if receiver.get("server_connected",0)==0:
        return "receiver saw QUIC connection but no completed mTLS handshake"
    if receiver.get("server_streams",0)==0:
        return "receiver established QUIC but saw no incoming stream"
    if receiver.get("ctd1_dispatch",0)==0:
        return "receiver saw QUIC streams but dispatched no CTD1 data"
    return "native C++ CTD1 delivered, but Python subscriber did not receive"

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--root",type=pathlib.Path,required=True)
    parser.add_argument("--profile",choices=("netem_10ms_1pct","netem_10ms_2pct"),required=True)
    parser.add_argument("--repeats",type=int,default=30)
    parser.add_argument("--messages",type=int,default=200)
    args=parser.parse_args()
    errors=[]
    file=args.root/"summary.json"
    rows=json.loads(file.read_text()) if file.is_file() else []
    rows=[r for r in rows if r.get("engine")=="citoq" and r.get("profile")==args.profile]
    if len(rows)!=args.repeats or {r.get("repetition") for r in rows}!=set(range(1,args.repeats+1)):
        errors.append("30 independent trials/repetition IDs were not completed")
    report={"profile":args.profile,"requested_trials":args.repeats,"messages_per_trial":args.messages,
            "attempted_trials":len(rows),"accepted_trials":0,"failures":[],
            "trace_coverage":0,"p95_ms_of_accepted":[],"p99_ms_of_accepted":[],
            "notes":[
               "30 independent process pairs on same runner; not thirty independent machines",
               "Native counters are diagnostic build-only and may marginally influence timings",
               "A publish API success is not a network-delivery ACK",
               "A single invalid trial makes the reliability gate FAIL, never ignored"
            ]}
    for row in rows:
        rep=row.get("repetition")
        sender=trace_from(args.root/f"{args.profile}-citoq-r{rep}-sender.log",errors)
        receiver=trace_from(args.root/f"{args.profile}-citoq-r{rep}-receiver.log",errors)
        if sender and receiver: report["trace_coverage"]+=1
        good=(row.get("accepted") is True and row.get("expected")==args.messages
              and row.get("received_unique")==args.messages and row.get("missing")==0
              and row.get("duplicates")==0 and row.get("invalid")==0
              and row.get("sender_exit")==0 and row.get("receiver_exit")==0)
        if good:
            report["accepted_trials"]+=1
            report["p95_ms_of_accepted"].append(row["latency_us_p95"]/1000)
            report["p99_ms_of_accepted"].append(row["latency_us_p99"]/1000)
        else:
            report["failures"].append({
                "repetition":rep,"missing":row.get("missing"),
                "received_unique":row.get("received_unique"),
                "sender_exit":row.get("sender_exit"),
                "receiver_exit":row.get("receiver_exit"),
                "hypothesis":classify(sender,receiver),
                "sender_native":sender,"receiver_native":receiver,
            })
    for field in ("p95_ms_of_accepted","p99_ms_of_accepted"):
        data=report.pop(field)
        report[field+"_median"]=round(statistics.median(data),6) if data else None
        report[field+"_maximum"]=round(max(data),6) if data else None
    report["errors"]=errors
    report["reliability_pass"]=(not errors and not report["failures"] and
                                report["accepted_trials"]==args.repeats)
    args.root.mkdir(parents=True,exist_ok=True)
    (args.root/"loss-forensics-report.json").write_text(json.dumps(report,indent=2)+"\n")
    print("CITOQ_LOSS_FORENSICS_REPORT="+json.dumps(report,sort_keys=True),flush=True)
    for issue in errors:print("CITOQ_LOSS_FORENSICS_ERROR="+issue,file=sys.stderr)
    return int(not report["reliability_pass"])

if __name__=="__main__":
    sys.exit(main())
