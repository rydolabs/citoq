#!/usr/bin/env python3
"""Validate native MsQuic receive event -> loop and typed Context dispatch timing.

This is diagnostic event-stage profiling, NOT packet arrival or wire latency.
"""
import argparse
import json
import pathlib
import statistics
import sys

def median(xs):
    return round(statistics.median(xs),3) if xs else None

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--root",type=pathlib.Path,required=True)
    ap.add_argument("--repeats",type=int,default=3)
    a=ap.parse_args()
    errors=[]
    output={"scope":"Same-machine Linux netns, 256B/100Hz/200 messages, CitoQ mTLS/QUIC vs Zenoh TCP",
            "native_profile":"CitoQ receiver MsQuic callback -> owner-loop event processing, then C++ receive_data including Python callback",
            "groups":{}}
    for engine in ("citoq","zenoh"):
        p=a.root/engine/"summary.json"
        if not p.exists():
            errors.append("missing "+str(p));continue
        rows=[x for x in json.loads(p.read_text()) if x.get("engine")==engine]
        if len(rows)!=a.repeats:
            errors.append(f"{engine}: expected {a.repeats} rows")
        if {r.get("repetition") for r in rows}!=set(range(1,a.repeats+1)):
            errors.append(f"{engine}: invalid repetition index")
        keys=["latency_us_p50","latency_us_p95","latency_us_p99",
              "callback_decode_us_p95","callback_entry_us_p95","sender_api_us_p95",
              "sender_pre_api_us_p95","receiver_cpu_s","sender_cpu_s"]
        if engine=="citoq":
            keys+=["native_msquic_receive_to_owner_p50_us",
                   "native_msquic_receive_to_owner_p95_us",
                   "native_msquic_receive_to_owner_p99_us",
                   "native_data_dispatch_p50_us",
                   "native_data_dispatch_p95_us",
                   "native_data_dispatch_p99_us"]
        good=[]
        for x in rows:
            ok=(x.get("accepted")==True and x.get("expected")==200 and x.get("received_unique")==200 and
                x.get("missing")==0 and x.get("duplicates")==0 and x.get("invalid")==0 and
                x.get("sender_exit")==0 and x.get("receiver_exit")==0 and
                x.get("stage_successful_sends")==200 and
                all(isinstance(x.get(k),(float,int)) for k in keys))
            if engine=="citoq":
                ok=ok and x.get("native_msquic_receive_to_owner_samples",0)>=200 and x.get("native_data_dispatch_samples")==200
            if not ok:
                errors.append(f"{engine}: invalid trace or delivery in rep {x.get('repetition')}: {x}")
            else:good.append(x)
        output["groups"][engine]={
            "accepted":len(good),"expected":a.repeats,
            "median_of_trial_metrics":{k:median([x[k] for x in good]) if len(good)==a.repeats else None for k in keys}}
    output["limitations"]=[
        "Native callback-to-owner measures only the interval starting in MsQuic's RECEIVE callback after kernel/QUIC processing, not transmission or arrival time.",
        "Native dispatch covers HostRuntimeCore packet delivery and cross-ABI/Python callback before return; it is not just Python decoding.",
        "Native trace instrumentation changes timing and should never be enabled as product default.",
        "Only receiver registered interest; all measured QUIC data required mTLS client certificates.",
        "Stage p95 values are from different samples and cannot be added or subtracted as one message path.",
        "CitoQ authenticated mTLS/QUIC and Zenoh unencrypted TCP are not QoS/security equivalents.",
    ]
    output["errors"]=errors
    (a.root/"native-rx-stage-report.json").write_text(json.dumps(output,indent=2)+"\n")
    print("NATIVE_RX_STAGE_REPORT="+json.dumps(output,sort_keys=True),flush=True)
    for e in errors:print("NATIVE_RX_STAGE_ERROR="+e,file=sys.stderr,flush=True)
    return 0 if not errors else 1

if __name__=="__main__":sys.exit(main())
