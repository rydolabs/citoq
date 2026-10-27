#!/usr/bin/env python3
"""Strict security-normalized QUIC/mTLS payload/rate/loss matrix."""
import argparse
import json
import pathlib
import statistics
import sys

SCENARIOS = {
    "64B-100Hz": (64,100,"baseline"),
    "256B-100Hz": (256,100,"baseline"),
    "4096B-100Hz": (4096,100,"baseline"),
    "16384B-100Hz": (16384,100,"baseline"),
    "256B-1000Hz": (256,1000,"baseline"),
    "256B-loss": (256,100,"netem_10ms_1pct"),
}

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--root",type=pathlib.Path,required=True)
    ap.add_argument("--repeats",type=int,default=2)
    args=ap.parse_args()
    errors=[]
    result={
        "conditions": {
            "CitoQ": "native authenticated MsQuic mTLS/QUIC Context+Python poll_wait",
            "Zenoh": "Python API, peer QUIC TLS client certificate mandatory, SHM disabled",
            "common": "Ubuntu 24.04 veth netns, 200 sends, receiver delivered to Python callback",
            "netem": "both namespaces 10ms ±2ms + 1% packet loss for loss scenario",
        },
        "scenarios":{},
        "limitations":[
            "Same encryption and transport family, but not same QUIC implementation, QoS/framing/callback/queue policy or total control traffic.",
            "200 samples per repetition are not statistical throughput saturation evidence.",
            "No multiple independent hardware hosts; runner variability remains.",
            "Median of trial-local percentiles is not pooled p95/p99.",
        ]}
    for group,(payload,rate,profile) in SCENARIOS.items():
        result["scenarios"][group]={}
        for cohort,(engine,transport) in {
            "citoq":("citoq",None),"zenoh-quic-mtls":("zenoh","quic-mtls")
        }.items():
            path=args.root/group/cohort/"summary.json"
            if not path.is_file():
                errors.append(f"{group}/{cohort}: missing results")
                continue
            rows=[x for x in json.loads(path.read_text())
                  if x.get("engine")==engine and x.get("profile")==profile]
            if len(rows)!=args.repeats or {r.get("repetition") for r in rows}!=set(range(1,args.repeats+1)):
                errors.append(f"{group}/{cohort}: invalid repetitions")
            good=[]
            for r in rows:
                if (r.get("accepted") is not True or r.get("received_unique")!=200 or
                    r.get("missing")!=0 or r.get("duplicates")!=0 or
                    r.get("invalid")!=0 or r.get("sender_exit")!=0 or
                    r.get("receiver_exit")!=0 or r.get("payload_bytes")!=payload or
                    (engine=="zenoh" and (r.get("zenoh_transport")!=transport or
                                          r.get("zenoh_auth_control")!="legitimate")) or
                    (engine=="citoq" and r.get("citoq_rx_mode")!="event")):
                    errors.append(f"{group}/{cohort}: failure/invalid identity: {r}")
                else:
                    good.append(r)
            def med(k,div=1):
                values=[r.get(k) for r in good]
                if len(values)!=args.repeats or not all(isinstance(x,(int,float)) for x in values):
                    return None
                return round(statistics.median(values)/div,6)
            result["scenarios"][group][cohort]={
                "accepted":len(good),"trials":args.repeats,
                "p50_ms":med("latency_us_p50",1000),
                "p95_ms":med("latency_us_p95",1000),
                "p99_ms":med("latency_us_p99",1000),
                "sender_cpu_s":med("sender_cpu_s"),
                "receiver_cpu_s":med("receiver_cpu_s"),
                "sender_rss_kb":med("sender_max_rss_kb"),
                "receiver_rss_kb":med("receiver_max_rss_kb"),
            }
        x=result["scenarios"][group].get("citoq",{}).get("p95_ms")
        y=result["scenarios"][group].get("zenoh-quic-mtls",{}).get("p95_ms")
        if x is not None and y is not None and y>0:
            result["scenarios"][group]["citoq_to_zenoh_quic_p95_ratio"]=round(x/y,5)
    result["errors"]=errors
    args.root.mkdir(parents=True,exist_ok=True)
    (args.root/"quic-mtls-extended-report.json").write_text(json.dumps(result,indent=2)+"\n")
    print("QUIC_MTLS_EXTENDED_REPORT="+json.dumps(result,sort_keys=True),flush=True)
    for e in errors:print("QUIC_MTLS_EXTENDED_ERROR="+e,file=sys.stderr)
    return int(bool(errors))
if __name__=="__main__":
    sys.exit(main())
