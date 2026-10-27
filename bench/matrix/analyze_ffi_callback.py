#!/usr/bin/env python3
"""Validate trace integrity and measure timing overhead, never assert speedup.

All spans in nanoseconds measured on the receiver owner thread. C-ABI call
duration contains Python trampoline, bytes copy, and user handler. Thus nested
durations are NOT additive independent legs of the end-to-end latency.
"""
import argparse
import json
import pathlib
import statistics
import sys

COHORTS=("normal-a","traced-a","traced-b","normal-b")
def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--root",type=pathlib.Path,required=True)
    ap.add_argument("--repeats",type=int,default=3)
    args=ap.parse_args()
    report={"cohorts":{},"limitations":[
        "Callback durations are nested: native ABI invocation spans Python copy and user handler.",
        "Profiling adds two Python clock calls, list appends, native chrono calls; latency under trace is perturbed.",
        "FFI entry/exit and Python dispatch overhead is inferred from ABI duration minus measured Python inner spans, not independently timestamped.",
        "Neither event-queue dwell nor MsQuic worker scheduling is present in ABI callback duration.",
        "No single-runner timing comparison establishes a stable product speedup.",
    ]}
    errors=[]
    for cohort in COHORTS:
        fn=args.root/cohort/"summary.json"
        if not fn.exists():
            errors.append(f"{cohort}: missing summary")
            continue
        rows=[x for x in json.loads(fn.read_text()) if x.get("engine")=="citoq" and x.get("profile")=="baseline"]
        if len(rows)!=args.repeats or {r.get("repetition") for r in rows} != set(range(1,args.repeats+1)):
            errors.append(f"{cohort}: incomplete trial coverage")
        values=[]
        for r in rows:
            if (r.get("accepted") is not True or r.get("received_unique")!=200 or
                    r.get("duplicates")!=0 or r.get("invalid")!=0 or
                    r.get("missing")!=0 or r.get("sender_exit")!=0 or
                    r.get("receiver_exit")!=0 or r.get("citoq_rx_mode")!="event"):
                errors.append(f"{cohort}: invalid trial {r}")
            if cohort.startswith("traced"):
                n=r.get("repetition")
                path=args.root/cohort/f"baseline-citoq-r{n}-receiver.log"
                log=path.read_text(errors="replace") if path.exists() else ""
                abi=[json.loads(v.partition("=")[2]) for v in log.splitlines() if "CITOQ_ABI_TRACE_JSON=" in v]
                py=[json.loads(v.partition("=")[2]) for v in log.splitlines() if "CITOQ_PY_TRACE_JSON=" in v]
                if len(abi)!=1 or len(py)!=1:
                    errors.append(f"{cohort} {n}: no unique receiver ABI/Python traces")
                elif abi[0].get("samples") !=200 or py[0].get("samples")!=200:
                    errors.append(f"{cohort} {n}: ABI/Python samples not 200: {abi} {py}")
                else:
                    values.append({"abi":abi[0],"py":py[0]})
        def med(f):
            xs=[r.get(f) for r in rows]
            return round(statistics.median(xs),6) if len(xs)==args.repeats and all(isinstance(x,(int,float)) for x in xs) else None
        cohort_report={
            "accepted":sum(1 for r in rows if r.get("accepted") is True),
            "expected":args.repeats,
            "p95_ms":round(med("latency_us_p95")/1000,6) if med("latency_us_p95") is not None else None,
            "p99_ms":round(med("latency_us_p99")/1000,6) if med("latency_us_p99") is not None else None,
            "receiver_cpu_s":med("receiver_cpu_s"),
        }
        if len(values)==args.repeats:
            for prefix,item,keys in (
                ("abi","abi",("median_ns","p95_ns","p99_ns")),
                ("python","py",("payload_copy_median_ns","payload_copy_p95_ns",
                                "user_handler_median_ns","user_handler_p95_ns")),
            ):
                for key in keys:
                    cohort_report[prefix+"_"+key]=statistics.median(x[item][key] for x in values)
        report["cohorts"][cohort]=cohort_report
    report["errors"]=errors
    args.root.mkdir(parents=True,exist_ok=True)
    (args.root/"ffi-callback-profile.json").write_text(json.dumps(report,indent=2)+"\n")
    print("FFI_CALLBACK_PROFILE="+json.dumps(report,sort_keys=True),flush=True)
    for error in errors: print("FFI_CALLBACK_ERROR="+error,file=sys.stderr)
    return int(bool(errors))

if __name__=="__main__":
    sys.exit(main())
