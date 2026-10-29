#!/usr/bin/env python3
"""Post-receiver-restart: strict security-normalized CitoQ vs Zenoh parity gate.

Only benchmark/public API results. No product runtime source modifications.
"""
import argparse
import json
import pathlib
import statistics
import sys

SCENARIOS = {
    "256B-100Hz": (100, 1000, "baseline", 3),
    "256B-1000Hz": (1000, 1000, "baseline", 3),
    "256B-loss1": (100, 200, "netem_10ms_1pct", 2),
    "256B-loss2": (100, 200, "netem_10ms_2pct", 2),
}
COHORTS = {"citoq": "citoq", "zenoh-quic-mtls": "zenoh"}

def median(rows, key, scale=1):
    vals=[r.get(key) for r in rows]
    if not vals or any(not isinstance(v,(int,float)) for v in vals):
        return None
    return round(statistics.median(vals)/scale,6)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--root",type=pathlib.Path,required=True)
    args=ap.parse_args()
    errors=[]
    report={
        "methods":{
            "transport":"CitoQ MsQuic mTLS vs Zenoh 1.10.1 QUIC/mTLS; no TCP fallback or SHM",
            "clock":"Linux shared CLOCK_MONOTONIC across two independent netns",
            "metric":"time embedded in sent 256B payload to receiving public Python callback",
            "sample_unit":"trial-level p50/p95/p99 medians; not pooled percentiles",
            "pacing":"first-accepted-send rebased; measured successful API publish Hz",
            "recovery":"comparison runs after separate receiver SIGKILL / new-incarnation gate passes"},
        "scenarios":{},
        "caveats":[
            "A successful publish is not the receiver's application delivery acknowledgement.",
            "The two implementations differ in framing, QoS and callback scheduler even when QUIC/mTLS security is comparable.",
            "Public CI runners vary substantially; CPU seconds include control and process lifetime.",
            "A passing 2% loss sample cannot establish zero failure probability.",
            "The 256B payload validates sequence/time/size but not an independent application CRC."],
    }
    for scenario,(rate,count,profile,repeats) in SCENARIOS.items():
        report["scenarios"][scenario]={}
        for cohort,engine in COHORTS.items():
            directory=args.root/scenario/cohort
            path=directory/"summary.json"
            if not path.exists():
                errors.append(f"{scenario}/{cohort}: missing summary")
                continue
            rows=[r for r in json.loads(path.read_text())
                  if r.get("profile")==profile and r.get("engine")==engine]
            if len(rows)!=repeats or {r.get("repetition") for r in rows}!=set(range(1,repeats+1)):
                errors.append(f"{scenario}/{cohort}: expected {repeats} exact repetitions; got {len(rows)}")
            traces=[]
            for row in rows:
                rep=row.get("repetition")
                if not (row.get("accepted") is True and row.get("expected")==count and
                        row.get("received_unique")==count and row.get("payload_bytes")==256 and
                        row.get("missing")==0 and row.get("duplicates")==0 and row.get("invalid")==0 and
                        row.get("sender_exit")==0 and row.get("receiver_exit")==0 and
                        row.get("bridge_rx_delta",0)>0 and row.get("bridge_tx_delta",0)>0 and
                        (engine!="citoq" or row.get("citoq_rx_mode")=="event") and
                        (engine!="zenoh" or (row.get("zenoh_transport")=="quic-mtls" and
                                             row.get("zenoh_auth_control")=="legitimate"))):
                    errors.append(f"{scenario}/{cohort} repetition={rep}: delivery/authenticity failed: {row}")
                log=directory/f"{profile}-{engine}-r{rep}-sender.log"
                if not log.exists():
                    errors.append(f"{scenario}/{cohort} repetition={rep}: missing sender log")
                    continue
                strings=[line.split("PACING_REPORT=",1)[1]
                         for line in log.read_text(errors="replace").splitlines()
                         if "PACING_REPORT=" in line]
                if len(strings)!=1:
                    errors.append(f"{scenario}/{cohort} repetition={rep}: {len(strings)} sender pacing reports")
                    continue
                try:
                    trace=json.loads(strings[0])
                except ValueError as exc:
                    errors.append(f"{scenario}/{cohort} invalid pacing JSON: {exc}")
                    continue
                observed=trace.get("observed_hz")
                if (trace.get("count")!=count or trace.get("mode")!="rebase" or
                    trace.get("offered_hz")!=rate or not isinstance(observed,(int,float)) or
                    not rate*.95 <= observed <= rate*1.05):
                    errors.append(f"{scenario}/{cohort} repetition={rep}: pacing mismatch {trace}")
                if trace.get("short_gap_count",999999)>max(3, count//100):
                    errors.append(f"{scenario}/{cohort} repetition={rep}: excessive catch-up gaps {trace}")
                traces.append(trace)
            cohort_entry={
                "accepted_trials":sum(r.get("accepted") is True for r in rows),
                "expected_trials":repeats,"messages_per_trial":count,
                "sender_actual_hz":median(traces,"observed_hz"),
                "sender_short_gap_count":median(traces,"short_gap_count"),
                "p50_ms":median(rows,"latency_us_p50",1000),
                "p95_ms":median(rows,"latency_us_p95",1000),
                "p99_ms":median(rows,"latency_us_p99",1000),
                "first_100_p95_ms":median(rows,"latency_us_first_100_p95",1000),
                "last_100_p95_ms":median(rows,"latency_us_last_100_p95",1000),
                "sender_cpu_s":median(rows,"sender_cpu_s"),
                "receiver_cpu_s":median(rows,"receiver_cpu_s"),
                "sender_rss_kb":median(rows,"sender_max_rss_kb"),
                "receiver_rss_kb":median(rows,"receiver_max_rss_kb")}
            for metric in ("p50_ms","p95_ms","p99_ms","sender_cpu_s",
                           "receiver_cpu_s","sender_rss_kb","receiver_rss_kb"):
                if cohort_entry[metric] is None:
                    errors.append(f"{scenario}/{cohort}: missing metric {metric}")
            report["scenarios"][scenario][cohort]=cohort_entry
        cq=report["scenarios"][scenario].get("citoq",{}).get("p95_ms")
        zn=report["scenarios"][scenario].get("zenoh-quic-mtls",{}).get("p95_ms")
        if cq is not None and zn is not None and zn>0:
            report["scenarios"][scenario]["citoq_to_zenoh_p95_ratio"]=round(cq/zn,5)
    report["errors"]=errors
    args.root.mkdir(parents=True,exist_ok=True)
    (args.root/"post-recovery-parity-report.json").write_text(json.dumps(report,indent=2)+"\n")
    print("POST_RECOVERY_PARITY_REPORT="+json.dumps(report,sort_keys=True),flush=True)
    for msg in errors:
        print("POST_RECOVERY_PARITY_ERROR="+msg,file=sys.stderr)
    return int(bool(errors))

if __name__=="__main__":
    sys.exit(main())
