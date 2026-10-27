#!/usr/bin/env python3
"""Guard a same-runner before/after CitoQ mTLS/QUIC latency A/B."""
import argparse
import json
import pathlib
import statistics

def load(path):
    rows=json.loads(pathlib.Path(path).read_text())
    assert len(rows)==6, f"expected three repeats x two profiles: {path}"
    by_profile={}
    for row in rows:
        assert row["engine"]=="citoq" and row["accepted"], row
        assert row["received_unique"]==row["expected"]==200
        assert row["duplicates"]==row["invalid"]==row["missing"]==0
        by_profile.setdefault(row["profile"],[]).append(row["latency_us_p95"]/1000)
    assert {k:len(v) for k,v in by_profile.items()}=={
        "baseline":3,"netem_10ms_1pct":3}, by_profile
    return by_profile

def main():
    p=argparse.ArgumentParser()
    p.add_argument("--legacy",required=True)
    p.add_argument("--immediate",required=True)
    p.add_argument("--report",required=True)
    args=p.parse_args()
    old,new=load(args.legacy),load(args.immediate)
    report={"profiles":{}}
    for profile in old:
        before=statistics.median(old[profile])
        after=statistics.median(new[profile])
        report["profiles"][profile]={
            "legacy_p95_ms":old[profile], "immediate_p95_ms":new[profile],
            "legacy_median_p95_ms":before, "immediate_median_p95_ms":after,
            "median_reduction_ms":before-after,
            "median_reduction_percent":(before-after)/before*100,
        }
    pathlib.Path(args.report).write_text(json.dumps(report,indent=2)+"\n")
    print("POLL_FIX_REPORT="+json.dumps(report,sort_keys=True),flush=True)
    # The 10 ms artificial queueing delay must be removed, not just shifted
    # between cohorts; use median across 3 full successful trials.
    oldp=report["profiles"]["baseline"]["legacy_median_p95_ms"]
    newp=report["profiles"]["baseline"]["immediate_median_p95_ms"]
    assert newp < oldp-5, f"regression: legacy {oldp:.3f}ms, immediate {newp:.3f}ms"
    assert newp < 5.0, f"CitoQ baseline p95 still high: {newp:.3f}ms"
    print("POLL_FIX_VERIFIED baseline median p95 below 5ms, reduction above 5ms",flush=True)

if __name__=="__main__":
    main()
