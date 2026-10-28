#!/usr/bin/env python3
"""Strict public API receive-stage trace validator (non-causal aggregate)."""
import argparse,json,pathlib,statistics,sys
def main():
    p=argparse.ArgumentParser()
    p.add_argument("--root",type=pathlib.Path,required=True)
    p.add_argument("--repeats",type=int,default=3)
    args=p.parse_args()
    errors=[]
    report={"limitations":[
        "ctypes callback timing starts at Python bridge entry, after native C ABI dispatch.",
        "Per-callback trace adds Python perf_counter and append overhead.",
        "Separate p95s are not additive or a causal decomposition of end-to-end p95.",
        "Zenoh TCP is not TLS/mTLS-equivalent to CitoQ authenticated MsQuic QUIC.",
    ],"engines":{}}
    for engine in ("citoq","zenoh"):
        root=args.root/engine
        summary=root/"summary.json"
        if not summary.is_file():
            errors.append(f"{engine} missing summary")
            continue
        rows=[r for r in json.loads(summary.read_text()) if r.get("engine")==engine and r.get("profile")=="baseline"]
        if len(rows)!=args.repeats or {r.get("repetition") for r in rows}!=set(range(1,args.repeats+1)):
            errors.append(f"{engine}: invalid repetition coverage")
        for r in rows:
            if not(r.get("accepted") is True and r.get("received_unique")==200 and r.get("missing")==0
                   and r.get("duplicates")==0 and r.get("invalid")==0 and
                   r.get("sender_exit")==0 and r.get("receiver_exit")==0):
                errors.append(f"{engine}: failed trial {r}")
        result={"p95_ms":statistics.median(r["latency_us_p95"] for r in rows)/1000 if rows else None}
        if engine=="citoq":
            trace=[]
            for i in range(1,args.repeats+1):
                path=root/f"baseline-citoq-r{i}-receiver.log"
                if not path.is_file():
                    errors.append(f"missing trace {path}")
                    continue
                marks=[x.split("CITOQ_PY_CALLBACK_STAGE=",1)[1] for x in path.read_text(errors="replace").splitlines() if "CITOQ_PY_CALLBACK_STAGE=" in x]
                if len(marks)!=1:
                    errors.append(f"trace count {path}: {len(marks)}")
                    continue
                record=json.loads(marks[0])
                if any(record.get(k,{}).get("count")!=200 for k in ("copy_ns","handler_ns","bridge_ns")):
                    errors.append(f"invalid 200/200 callback trace {path}: {record}")
                trace.append(record)
            result["callback_stage"]={
                stage:{metric:round(statistics.median(r[stage][metric] for r in trace)/1000,3)
                       for metric in ("p50_ns","p95_ns","p99_ns")}
                for stage in ("copy_ns","handler_ns","bridge_ns") for _ in [0]
            } if trace else {}
            result["stage_units"]="microseconds"
        report["engines"][engine]=result
    report["errors"]=errors
    args.root.mkdir(parents=True,exist_ok=True)
    (args.root/"python-callback-stage-report.json").write_text(json.dumps(report,indent=2)+"\n")
    print("PY_CALLBACK_STAGE_REPORT="+json.dumps(report,sort_keys=True),flush=True)
    for e in errors:print("PY_CALLBACK_STAGE_ERROR="+e,file=sys.stderr)
    return int(bool(errors))
if __name__=="__main__":
    sys.exit(main())
