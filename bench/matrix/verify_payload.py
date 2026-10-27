#!/usr/bin/env python3
"""Strict size-sweep acceptance: never omit failures or relabel paced rate as maximum throughput."""
import argparse
import csv
import json
import pathlib
import statistics
import sys

ENGINES=("citoq","cyclonedds","zenoh","fastdds")

def summarize(root, sizes, repeats):
    rows=[]
    failures=[]
    output=[]
    for size in sizes:
        path=root/f"size-{size}"/"summary.json"
        if not path.is_file():
            failures.append(f"missing summary: {path}")
            continue
        found=json.loads(path.read_text())
        for engine in ENGINES:
            subset=[r for r in found if r.get("engine")==engine and r.get("profile")=="baseline"]
            if len(subset)!=repeats or {r.get("repetition") for r in subset}!=set(range(1,repeats+1)):
                failures.append(f"{size}B {engine} expected {repeats} distinct baseline trials, got {len(subset)}")
            for r in subset:
                record=dict(r)
                record["nominal_application_bytes"]=size
                rows.append(record)
                if not (r.get("accepted") is True and r.get("payload_bytes")==size and
                        r.get("expected")==200 and r.get("received_unique")==200 and
                        r.get("missing")==0 and r.get("duplicates")==0 and r.get("invalid")==0):
                    failures.append(f"{size}B {engine} trial {r.get('repetition')} failed: {r}")
            good=[r for r in subset if r.get("accepted") is True and isinstance(r.get("latency_us_p95"),(int,float))]
            output.append({
                "size_bytes":size,"engine":engine,"trials":len(subset),"accepted_trials":len(good),
                "median_p95_ms":round(statistics.median(x["latency_us_p95"] for x in good)/1000,6) if good else None,
                "median_p99_ms":round(statistics.median(x["latency_us_p99"] for x in good)/1000,6) if good else None,
                "median_bridge_ingress_bytes":statistics.median(x["bridge_rx_delta"] for x in good) if good else None,
                "paced_payload_rate_mbps":round(size*100*8/1e6,5),
            })
    root.mkdir(parents=True,exist_ok=True)
    (root/"all-trials.json").write_text(json.dumps(rows,indent=2)+"\n")
    (root/"matrix-summary.json").write_text(json.dumps({"source_pin":"08188eba73284dd1069016174ad216f5e02d2cfa","rows":output,"failures":failures,"note":"100Hz paced rate is NOT maximum throughput; CitoQ public mTLS/QUIC, DDS UDP RELIABLE and Zenoh TCP are not equivalent QoS/security stacks."},indent=2)+"\n")
    with (root/"matrix-summary.csv").open("w",newline="") as f:
        wr=csv.DictWriter(f,fieldnames=list(output[0]))
        wr.writeheader();wr.writerows(output)
    print("PAYLOAD_MATRIX="+json.dumps(output,sort_keys=True),flush=True)
    print(f"PAYLOAD_ACCEPTED={sum(r['accepted_trials'] for r in output)}/{len(ENGINES)*len(sizes)*repeats}",flush=True)
    for error in failures: print("PAYLOAD_FAILURE "+error,file=sys.stderr,flush=True)
    return not failures

def main():
    p=argparse.ArgumentParser()
    p.add_argument("--root",type=pathlib.Path,required=True)
    p.add_argument("--sizes",type=int,nargs="+",required=True)
    p.add_argument("--repeats",type=int,default=2)
    a=p.parse_args()
    return 0 if summarize(a.root,a.sizes,a.repeats) else 1

if __name__=="__main__":sys.exit(main())
