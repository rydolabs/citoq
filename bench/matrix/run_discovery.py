#!/usr/bin/env python3
"""Node-count publisher-readiness experiment; CitoQ is *internal UDP prototype*, not public Context."""
import argparse
import csv
import json
import pathlib
import statistics
import subprocess
import sys

ENGINES=("cito","dds","zenoh","fastdds")

def main():
    p=argparse.ArgumentParser()
    p.add_argument("--nodes",type=int,choices=(3,10,30),required=True)
    p.add_argument("--repeats",type=int,default=2)
    p.add_argument("--output",type=pathlib.Path,required=True)
    a=p.parse_args()
    a.output.mkdir(parents=True,exist_ok=True)
    cwd=pathlib.Path(__file__).resolve().parents[2]
    binaries={
        "cito":cwd/"build/bench-m26/cito_multi_topic_node",
        "dds":cwd/"build/bench-m26/dds_multi_topic_node",
        "zenoh":cwd/"build/zenoh/zenoh_multi_topic_node",
        "fastdds":cwd/"build/fastdds/fastdds_multi_topic_node"
    }
    cmd=[sys.executable,str(cwd/"bench/m26/m26_efficiency.py"),
         "--engines",*ENGINES,
         "--nodes",str(a.nodes),"--topics","4","--percent","50",
         "--bytes","256","--samples","4","--hz","10",
         "--repeats",str(a.repeats),"--stagger-ms","4",
         "--lifetime-ms",str(15000 if a.nodes==3 else 25000 if a.nodes==10 else 45000),
         "--profile","baseline","--output",str(a.output)]
    for engine,path in binaries.items(): cmd+=["--"+engine,str(path)]
    with (a.output/"process.log").open("w") as log:
        result=subprocess.run(cmd,cwd=cwd,stdout=log,stderr=subprocess.STDOUT,check=False)
    source=a.output/"results.json"
    rows=json.loads(source.read_text()) if source.exists() else []
    expected=len(ENGINES)*a.repeats
    failures=[]
    if result.returncode:
        failures.append(f"m26 harness exited {result.returncode}")
    if len(rows)!=expected:
        failures.append(f"expected {expected} results; got {len(rows)}")
    out=[]
    for eng in ENGINES:
        selected=[r for r in rows if r.get("engine")==eng]
        if len(selected)!=a.repeats: failures.append(f"{eng}: {len(selected)} trials")
        for r in selected:
            if not (r.get("valid") is True and r.get("all_received") is True and
                    r.get("received")==r.get("expected") and r.get("negative_received")==0 and
                    isinstance(r.get("discovery_ready_ms"),int) and
                    isinstance(r.get("prepublish_rx_bytes"),int)):
                failures.append(f"{eng} trial {r.get('repeat')} incomplete/unmatched: {r}")
        good=[r for r in selected if r.get("valid") is True and r.get("all_received") is True and r.get("discovery_ready_ms") is not None]
        out.append({
            "engine":eng,"nodes":a.nodes,"expected_trials":a.repeats,
            "accepted_trials":len(good),
            "median_publisher_ready_ms":statistics.median(x["discovery_ready_ms"] for x in good) if good else None,
            "median_prepublish_veth_rx_bytes":statistics.median(x.get("prepublish_rx_bytes",0) for x in good) if good else None,
            "median_full_trial_rx_bytes":statistics.median(x.get("veth_rx_bytes",0) for x in good) if good else None,
            "total_delivered":sum(x.get("received",0) for x in selected),
            "total_expected":sum(x.get("expected",0) for x in selected)
        })
    summary={
      "source_pin":"08188eba73284dd1069016174ad216f5e02d2cfa",
      "nodes":a.nodes,"results":out,"failures":failures,
      "warnings":[
       "CitoQ is citoq::detail::UdpRuntimeAdapter prototype, NOT installed Context mTLS/QUIC.",
       "Publisher readiness measured from own process start; subscribers are started before publisher.",
       "Zenoh matching_status may not mean all interested peers matched; DDS and CitoQ readiness semantics differ.",
       "Prep-uplish bytes include control traffic and process warmup; sampling every 50ms is approximate.",
       "No measured discovery-only bandwidth or comparable cold-start convergence across engines.",
       "BEST_EFFORT UDP vs Zenoh peer TCP have different delivery semantics."
      ]
    }
    (a.output/"matrix-summary.json").write_text(json.dumps(summary,indent=2)+"\n")
    with (a.output/"matrix-summary.csv").open("w",newline="") as f:
        wr=csv.DictWriter(f,fieldnames=list(out[0]))
        wr.writeheader();wr.writerows(out)
    print("DISCOVERY_MATRIX="+json.dumps(summary,sort_keys=True),flush=True)
    print(f"DISCOVERY_ACCEPTED={sum(x['accepted_trials'] for x in out)}/{expected}",flush=True)
    for e in failures:print("DISCOVERY_FAILURE "+e,file=sys.stderr,flush=True)
    return 0 if not failures else 1

if __name__=="__main__":sys.exit(main())
