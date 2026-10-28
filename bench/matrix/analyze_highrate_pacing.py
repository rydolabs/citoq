#!/usr/bin/env python3
"""Strict paired QUIC/mTLS high-rate pacing analysis, including send spacing."""
import argparse
import json
import pathlib
import statistics
import sys

COHORTS = {
    "citoq-legacy": ("citoq","legacy"),
    "citoq-rebase": ("citoq","rebase"),
    "zenoh-quic-legacy": ("zenoh","legacy"),
    "zenoh-quic-rebase": ("zenoh","rebase"),
}
def median(records, name, factor=1.0):
    values = [r.get(name) for r in records]
    if not values or any(not isinstance(v,(int,float)) for v in values):
        return None
    return round(statistics.median(values)/factor,6)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--root",type=pathlib.Path,required=True)
    ap.add_argument("--repeats",type=int,default=3)
    ap.add_argument("--messages",type=int,default=1000)
    ap.add_argument("--rate",type=int,default=1000)
    args=ap.parse_args()
    errors=[]
    report={"config":{"messages":args.messages,"payload_bytes":256,"rate_hz":args.rate,
        "transport":"CitoQ mTLS/MsQuic vs Zenoh QUIC mTLS, shared Ubuntu veth/netns",
        "measure":"monotonic payload timestamp -> Python callback",
        "pacing_compare":"legacy pre-match deadline versus first-success rebased no-catchup"},
        "cohorts":{},"warnings":[
            "The offered rate is not the sustained achieved rate; observed_hz is recorded.",
            "CPU/RSS from process /usr/bin/time; runner interference is uncontrolled.",
            "CitoQ/Zenoh QoS and Python callbacks are not identical.",
            "Reported p95s are medians of individual trial-level percentiles, not pooled.",
            "Warmup samples are included in full-trial p95; post-200 statistics are explicitly labeled.",
        ]}
    for cohort,(engine,mode) in COHORTS.items():
        directory=args.root/cohort
        path=directory/"summary.json"
        if not path.exists():
            errors.append(f"missing summary {path}")
            continue
        rows=[r for r in json.loads(path.read_text()) if r.get("engine")==engine and r.get("profile")=="baseline"]
        if len(rows)!=args.repeats or {r.get("repetition") for r in rows}!=set(range(1,args.repeats+1)):
            errors.append(f"{cohort}: incomplete repetitions")
        clocks=[]
        for row in rows:
            rep=row.get("repetition")
            if not (row.get("accepted") is True and row.get("expected")==args.messages and
                    row.get("received_unique")==args.messages and row.get("missing")==0 and
                    row.get("duplicates")==0 and row.get("invalid")==0 and
                    row.get("sender_exit")==0 and row.get("receiver_exit")==0 and
                    row.get("payload_bytes")==256 and
                    (engine!="citoq" or row.get("citoq_rx_mode")=="event")):
                errors.append(f"{cohort} rep {rep} invalid delivery: {row}")
            sender=directory/f"baseline-{engine}-r{rep}-sender.log"
            if not sender.exists():
                errors.append(f"{cohort} rep {rep}: missing sender log")
                continue
            traces=[line.split("PACING_REPORT=",1)[1] for line in sender.read_text(errors="replace").splitlines()
                    if "PACING_REPORT=" in line]
            if len(traces)!=1:
                errors.append(f"{cohort} rep {rep}: pacing records {len(traces)}")
                continue
            try:
                trace=json.loads(traces[0])
                if (trace.get("mode")!=mode or trace.get("count")!=args.messages or
                    trace.get("offered_hz")!=args.rate or not isinstance(trace.get("observed_hz"),(int,float))):
                    errors.append(f"{cohort} rep {rep}: invalid pacing trace {trace}")
                if mode=="rebase":
                    measured = trace.get("observed_hz",0)
                    if not 0.97*args.rate <= measured <= 1.03*args.rate:
                        errors.append(f"{cohort} rep {rep}: achieved {measured}Hz outside 3% of {args.rate}Hz")
                    if trace.get("short_gap_count",1000000)>3:
                        errors.append(f"{cohort} rep {rep}: catch-up microbursts detected ({trace.get('short_gap_count')})")
                clocks.append(trace)
            except (ValueError,KeyError) as exc:
                errors.append(f"{cohort} rep {rep} invalid trace JSON: {exc}")
        entry={"accepted_trials":sum(r.get("accepted") is True for r in rows),
               "attempted_trials":len(rows),
               "e2e_p50_ms":median(rows,"latency_us_p50",1000),
               "e2e_p95_ms":median(rows,"latency_us_p95",1000),
               "e2e_p99_ms":median(rows,"latency_us_p99",1000),
               "first_100_p95_ms":median(rows,"latency_us_first_100_p95",1000),
               "after_200_p95_ms":median(rows,"latency_us_after_first_200_p95",1000),
               "last_100_p95_ms":median(rows,"latency_us_last_100_p95",1000),
               "receiver_cpu_s":median(rows,"receiver_cpu_s"),
               "sender_cpu_s":median(rows,"sender_cpu_s"),
               "receiver_rss_kb":median(rows,"receiver_max_rss_kb"),
               "sender_observed_hz":median(clocks,"observed_hz"),
               "sender_short_gap_count":median(clocks,"short_gap_count"),
               "sender_first_100_span_ms":median(clocks,"first_100_span_ms"),
               "sender_first_accept_wait_ms":median(clocks,"first_accept_wait_ms")}
        report["cohorts"][cohort]=entry
    for engine,legacy,rebase in (("citoq","citoq-legacy","citoq-rebase"),
                                 ("zenoh","zenoh-quic-legacy","zenoh-quic-rebase")):
        old=report["cohorts"].get(legacy,{})
        new=report["cohorts"].get(rebase,{})
        oldp,newp=old.get("e2e_p95_ms"),new.get("e2e_p95_ms")
        if oldp and newp:
            report[engine+"_rebased_full_p95_change_pct"]=round((newp/oldp-1)*100,3)
    report["errors"]=errors
    args.root.mkdir(parents=True,exist_ok=True)
    (args.root/"highrate-pacing-report.json").write_text(json.dumps(report,indent=2)+"\n")
    print("HIGHRATE_PACING_REPORT="+json.dumps(report,sort_keys=True),flush=True)
    for error in errors:
        print("HIGHRATE_PACING_ERROR="+error,file=sys.stderr)
    return int(bool(errors))

if __name__=="__main__":
    sys.exit(main())
