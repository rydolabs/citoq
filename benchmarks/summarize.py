#!/usr/bin/env python3
"""Summarize raw benchmark result rows without hiding invalid runs."""
import collections, csv, json, pathlib, statistics, sys
root=pathlib.Path(sys.argv[1] if len(sys.argv)>1 else "bench-results")
rows=[]
for path in sorted(root.glob("*/results.json")):
    content=json.loads(path.read_text())
    if isinstance(content,list):
        rows.extend(content)
groups=collections.defaultdict(list)
for row in rows:
    key=(row['profile'],row['nodes'],row['topics'],row['percent'],row['engine'])
    groups[key].append(row)
def median(rows,key):
    vs=[float(v[key]) for v in rows if isinstance(v.get(key),(int,float))]
    return round(statistics.median(vs),2) if vs else "n/a"
header="| Network | Nodes / topics / sub % | Engine | Trials valid/full | veth RX median (bytes) | Max-reader p95 median (µs) | Median delivery rate |"
out=["# Recorded netns benchmark summary","",
     "All iterations from archived JSON are included, including invalid, incomplete, or failed-discovery trials.",
     "Medians are descriptive only, not formal confidence intervals. Different transport stacks / Python binding costs are not normalized.",
     "",header,"| --- | --- | --- | --- | ---: | ---: | ---: |"]
for key, group in sorted(groups.items()):
    profile,nodes,topics,percent,engine=key
    valid=sum(bool(v.get('valid')) for v in group)
    full=sum(bool(v.get('valid') and v.get('all_received')) for v in group)
    out.append(f"| {profile} | {nodes} / {topics} / {percent}% | {engine} | {valid}/{len(group)} valid, {full}/{len(group)} full | {median(group,'veth_rx_bytes')} | {median(group,'subscriber_p95_max_us')} | {median(group,'delivery_rate')} |")
out.extend(["","## Gate interpretation","",
            "- Valid = process exit, topic isolation, no corruption or duplicate, publisher sent all samples and benchmark-defined readiness achieved.",
            "- Full = valid plus exact expected message count received.",
            "- Do not exclude failed runs from statistical statements without explaining them.",
            "- The Python Zenoh and C/C++ DDS/CitoQ drivers have SDK overhead and nonidentical readiness semantics.",
            "- Raw per-node logs and receiver counts remain in the GitHub Actions artifact.",
            ""])
root.mkdir(parents=True,exist_ok=True)
(root/"SUMMARY.md").write_text("\n".join(out)+"\n")
fields=sorted(set().union(*(r.keys() for r in rows))) if rows else []
if fields:
    with (root/"ALL_TRIALS.csv").open('w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=fields);w.writeheader();w.writerows(rows)
print("\n".join(out),flush=True)
