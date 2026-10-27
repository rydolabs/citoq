#!/usr/bin/env python3
"""Strict full-delivery before/after product build evaluation: no invented speedup."""
import argparse,csv,json,pathlib,statistics,sys
CASES=((100,256,200),(100,16384,200),(1000,256,400),(1000,16384,400))
ENGINES=("before","after")
def report(root):
 rows=[];bad=[]
 for rate,size,count in CASES:
  vals={}
  for variant in ENGINES:
   path=root/f"r{rate}-s{size}-{variant}"/"summary.json"
   if not path.is_file():
    bad.append(f"missing result {path}");continue
   cases=json.loads(path.read_text())
   if len(cases)!=2 or {r.get("repetition") for r in cases}!={1,2}:
    bad.append(f"{path}: expected exactly 2 repetitions")
   for trial in cases:
    if not (trial.get("engine")=="citoq" and trial.get("profile")=="baseline"
            and trial.get("accepted") is True and trial.get("received_unique")==count
            and trial.get("expected")==count and trial.get("payload_bytes")==size
            and trial.get("missing")==0 and trial.get("duplicates")==0 and trial.get("invalid")==0):
     bad.append(f"failed delivery {path}: {trial}")
   good=[z for z in cases if z.get("accepted") is True and
         isinstance(z.get("latency_us_p95"),(int,float))]
   if good:
    vals[variant]={
     "p95_ms":round(statistics.median(x["latency_us_p95"] for x in good)/1000,6),
     "p99_ms":round(statistics.median(x["latency_us_p99"] for x in good)/1000,6),
     "bridge_rx_bytes":statistics.median(x["bridge_rx_delta"] for x in good),
     "complete":len(good)}
  if len(vals)==2:
   b,a=vals["before"],vals["after"]
   rows.append(dict(rate_hz=rate,size_bytes=size,count=count,
     before_p95_ms=b["p95_ms"],after_p95_ms=a["p95_ms"],
     p95_improvement_pct=round((b["p95_ms"]-a["p95_ms"])/b["p95_ms"]*100,3),
     before_p99_ms=b["p99_ms"],after_p99_ms=a["p99_ms"],
     before_bridge_rx_bytes=b["bridge_rx_bytes"],after_bridge_rx_bytes=a["bridge_rx_bytes"]))
 root.mkdir(parents=True,exist_ok=True)
 (root/"public-copy-ab-summary.json").write_text(json.dumps({
    "baseline_source":"08188eba73284dd1069016174ad216f5e02d2cfa",
    "variants":"Same Python public Context + native MsQuic, before pinned source vs optimized current source",
    "rows":rows,"failures":bad,
    "cautions":["Improvements may be negative; no CPU speedup claimed without profiler",
      "Both are same-runner application-level measurements, not protocol-only latency",
      "100 and 1000Hz are paced offered rates, NOT saturation throughput"]
 },indent=2)+"\n")
 if rows:
  with (root/"public-copy-ab-summary.csv").open("w",newline="") as f:
   w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
 for item in rows:print("PUBLIC_COPY_AB "+json.dumps(item,sort_keys=True),flush=True)
 for err in bad:print("PUBLIC_COPY_AB_FAILURE "+err,file=sys.stderr,flush=True)
 print(f"PUBLIC_COPY_AB_ACCEPTED={len(rows)*4}/16",flush=True)
 return not bad and len(rows)==len(CASES)
if __name__=="__main__":
 p=argparse.ArgumentParser()
 p.add_argument("--root",type=pathlib.Path,required=True)
 a=p.parse_args()
 sys.exit(0 if report(a.root) else 1)
