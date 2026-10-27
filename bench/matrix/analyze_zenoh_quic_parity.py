#!/usr/bin/env python3
"""Strict same-veth public API comparison including Zenoh authenticated QUIC.

Never call this an equivalent-implementation or QoS/CPU-saturation comparison:
it merely normalizes the QUIC encryption/authentication and application payload.
"""
import argparse
import json
import pathlib
import statistics
import sys

COHORTS = {
    "citoq-quic-mtls": ("citoq", None),
    "zenoh-tcp": ("zenoh", "tcp"),
    "zenoh-quic-mtls": ("zenoh", "quic-mtls"),
}

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--root", type=pathlib.Path, required=True)
    ap.add_argument("--repeats",type=int,default=3)
    ap.add_argument("--payload",type=int,default=256)
    ap.add_argument("--rate",type=float,default=100)
    args=ap.parse_args()
    report={
        "controls":{
            "citoq_transport":"authenticated native MsQuic/QUIC, owner event-driven poll_wait",
            "zenoh_quic_transport":"Zenoh QUIC peer, CA validated, client certificate mandatory",
            "zenoh_tcp_transport":"Zenoh non-TLS TCP peer diagnostic, not security-normalized",
            "zenoh_shm":"disabled",
            "payload_bytes":args.payload,
            "offered_rate_hz":args.rate,
            "messages_per_trial":200,
            "single_runner_netns":True,
        },
        "cohorts":{},
        "limitations":[
            "Common QUIC+mTLS authentication does not normalize QUIC stack, frame format, scheduling, QoS, code path, or callback behavior.",
            "Shared Linux kernel and monotonic time across network namespaces; not independent hosts.",
            "Trial-level median of percentile estimates is not a pooled percentile.",
            "100Hz/200 samples per trial does not demonstrate saturation throughput.",
            "No statistical superiority claim is possible from only three repeated trials on one runner.",
        ],
    }
    errors=[]
    for cohort,(engine,transport) in COHORTS.items():
        f=args.root/cohort/"summary.json"
        if not f.exists():
            errors.append(f"{cohort}: missing {f}")
            continue
        rows=[r for r in json.loads(f.read_text())
              if r.get("engine")==engine and r.get("profile")=="baseline"]
        if len(rows)!=args.repeats or {r.get("repetition") for r in rows} != set(range(1,args.repeats+1)):
            errors.append(f"{cohort}: incomplete/duplicate trial IDs")
        valid=[]
        for r in rows:
            if (r.get("accepted") is not True or r.get("received_unique")!=200 or
                r.get("duplicates")!=0 or r.get("missing")!=0 or r.get("invalid")!=0 or
                r.get("sender_exit")!=0 or r.get("receiver_exit")!=0 or
                r.get("payload_bytes")!=args.payload or
                (engine=="zenoh" and (r.get("zenoh_transport")!=transport or
                                      r.get("zenoh_auth_control")!="legitimate")) or
                (engine=="citoq" and r.get("citoq_rx_mode")!="event")):
                errors.append(f"{cohort} rep={r.get('repetition')}: invalid or mislabeled: {r}")
            else:
                valid.append(r)
        if engine=="zenoh":
            for r in rows:
                n=r.get("repetition")
                for role in ("sender","receiver"):
                    logfile=args.root/cohort / f"baseline-{engine}-r{n}-{role}.log"
                    if not logfile.exists() or ("ZENOH_TRANSPORT="+transport) not in logfile.read_text(errors="replace"):
                        errors.append(f"{cohort} rep={n} {role}: missing explicit transport mode proof")
        def med(field,scale=1):
            xs=[v.get(field) for v in valid]
            if len(xs)!=args.repeats or any(not isinstance(v,(int,float)) for v in xs):
                return None
            return round(statistics.median(xs)/scale,6)
        report["cohorts"][cohort]={
            "accepted":len(valid),"expected":args.repeats,
            "p50_ms":med("latency_us_p50",1000),
            "p95_ms":med("latency_us_p95",1000),
            "p99_ms":med("latency_us_p99",1000),
            "sender_cpu_s":med("sender_cpu_s"),
            "receiver_cpu_s":med("receiver_cpu_s"),
            "sender_rss_kb":med("sender_max_rss_kb"),
            "receiver_rss_kb":med("receiver_max_rss_kb"),
        }
    x=report["cohorts"].get("citoq-quic-mtls",{}).get("p95_ms")
    y=report["cohorts"].get("zenoh-quic-mtls",{}).get("p95_ms")
    if x is not None and y is not None and y>0:
        report["citoq_over_zenoh_quic_mtls_p95_ratio"]=round(x/y,5)
        report["citoq_minus_zenoh_quic_mtls_p95_ms"]=round(x-y,6)
    report["errors"]=errors
    args.root.mkdir(parents=True,exist_ok=True)
    (args.root/"quic-mtls-parity-report.json").write_text(json.dumps(report,indent=2)+"\n")
    print("QUIC_MTLS_PARITY_REPORT="+json.dumps(report,sort_keys=True),flush=True)
    for e in errors: print("QUIC_MTLS_PARITY_ERROR="+e,file=sys.stderr)
    return int(bool(errors))

if __name__=="__main__":
    sys.exit(main())
