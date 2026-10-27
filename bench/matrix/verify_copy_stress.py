#!/usr/bin/env python3
"""Six interleaved before/after pairs at 1000Hz, 16KiB: check deliveries and report CPU & tails."""
import argparse,csv,json,pathlib,statistics,sys
def main():
 p=argparse.ArgumentParser()
 p.add_argument("--root",type=pathlib.Path,required=True)
 p.add_argument("--repeats",type=int,default=6)
 a=p.parse_args()
 rows=[];errors=[]
 for index in range(1,a.repeats+1):
  for variant in ("before","after"):
   file=a.root/f"r{index}-{variant}"/"summary.json"
   if not file.exists():
    errors.append(f"missing {file}");continue
   trials=json.loads(file.read_text())
   if len(trials)!=1:
    errors.append(f"{file} expected 1 trial");continue
   t=trials[0]
   if not(t.get("accepted") is True and t.get("expected")==500 and
          t.get("received_unique")==500 and t.get("payload_bytes")==16384 and
          t.get("duplicates")==0 and t.get("invalid")==0 and t.get("missing")==0 and
          "sender_cpu_s" in t and "receiver_cpu_s" in t):
    errors.append(f"{file}: invalid {t}")
   rows.append(dict(index=index,variant=variant,accepted=t.get("accepted"),
    p95_ms=(t.get("latency_us_p95") or 0)/1000,
    p99_ms=(t.get("latency_us_p99") or 0)/1000,
    cpu_total_s=(t.get("sender_cpu_s") or 0)+(t.get("receiver_cpu_s") or 0),
    sender_rss_kb=t.get("sender_max_rss_kb"),receiver_rss_kb=t.get("receiver_max_rss_kb")))
 stats={}
 for variant in ("before","after"):
  arows=[r for r in rows if r["variant"]==variant]
  if len(arows)!=a.repeats:errors.append(f"{variant}: missing iterations")
  if arows:
   stats[variant]={k:round(statistics.median(row[k] for row in arows),6)
      for k in ("p95_ms","p99_ms","cpu_total_s")}
 out={"rows":rows,"medians":stats,"errors":errors,
      "note":"Identical public CitoQ Context+mTLS QUIC path, pinned baseline vs candidate, interleaved on same GitHub runner. 1000Hz offered not maximum throughput."}
 if len(stats)==2:
  out["p95_change_pct"]=round((stats["after"]["p95_ms"]/stats["before"]["p95_ms"]-1)*100,3)
  out["cpu_change_pct"]=round((stats["after"]["cpu_total_s"]/stats["before"]["cpu_total_s"]-1)*100,3)
 a.root.mkdir(parents=True,exist_ok=True)
 (a.root/"copy-stress-summary.json").write_text(json.dumps(out,indent=2)+"\n")
 if rows:
  with (a.root/"copy-stress-summary.csv").open("w",newline="") as f:
   w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
 print("PUBLIC_COPY_STRESS="+json.dumps(out,sort_keys=True),flush=True)
 for err in errors:print("PUBLIC_COPY_STRESS_FAILURE "+err,file=sys.stderr,flush=True)
 # Fail on broken delivery or severe material median regression; a small p95
 # fluctuation alone is not a speedup claim.
 if len(stats)==2 and out["p95_change_pct"]>25:
  print("PUBLIC_COPY_STRESS_REGRESSION p95 increased >25%",file=sys.stderr,flush=True)
  errors.append("median p95 regressed by >25%")
 return 0 if not errors else 1
if __name__=="__main__":sys.exit(main())
