#!/usr/bin/env python3
"""Require mixed-version QUIC Pub/Sub fallback and native persistent activation."""
import argparse,json,pathlib,statistics,sys

SCENARIOS={
    "new-to-old": ("candidate","baseline",False),
    "old-to-new": ("baseline","candidate",False),
    "new-to-new": ("candidate","candidate",True),
}
def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--root",type=pathlib.Path,required=True)
    ap.add_argument("--repeats",type=int,default=3)
    args=ap.parse_args()
    errors=[]
    result={"scenarios":{},"limitations":[
        "QUIC version/capability exchange is only for opt-in data-frame experiment.",
        "A new sender must fall back to original CTD1 streams with an old receiver.",
        "Candidate→candidate must record negotiation and actual CQDP use; mere delivery is insufficient.",
        "No complete deployment, security review or churn/fuzz assurance is implied."
    ]}
    for scenario,(_,_,expect_persistent) in SCENARIOS.items():
        directory=args.root/scenario
        data=directory/"summary.json"
        if not data.exists():
            errors.append(scenario+": missing summary")
            continue
        rows=[r for r in json.loads(data.read_text()) if r.get("engine")=="citoq" and r.get("profile")=="baseline"]
        if len(rows)!=args.repeats or {r.get("repetition") for r in rows}!=set(range(1,args.repeats+1)):
            errors.append(scenario+": repetition coverage")
        for r in rows:
            if not (r.get("accepted") is True and r.get("received_unique")==200 and
                    r.get("missing")==0 and r.get("duplicates")==0 and r.get("invalid")==0 and
                    r.get("sender_exit")==0 and r.get("receiver_exit")==0 and
                    r.get("payload_bytes")==256 and r.get("citoq_rx_mode")=="event"):
                errors.append(f"{scenario}: invalid delivery: {r}")
            logfile=directory/f"baseline-citoq-r{r['repetition']}-sender.log"
            if not logfile.exists():
                errors.append(scenario+": missing sender log")
            else:
                raw=logfile.read_text(errors="replace")
                used="CITOQ_PERSISTENT_USED" in raw
                negotiated="CITOQ_PERSISTENT_NEGOTIATED" in raw
                if used != expect_persistent or negotiated != expect_persistent:
                    errors.append(f"{scenario} trial {r['repetition']}: used={used}, negotiated={negotiated}, expected={expect_persistent}")
        if len(rows)==args.repeats:
            result["scenarios"][scenario]={
                "accepted":sum(1 for r in rows if r.get("accepted") is True),
                "p95_ms":round(statistics.median(r["latency_us_p95"] for r in rows)/1000,6),
                "receiver_cpu_s":round(statistics.median(r["receiver_cpu_s"] for r in rows),6),
            }
    result["errors"]=errors
    (args.root/"mixed-compat-report.json").write_text(json.dumps(result,indent=2)+"\n")
    print("CTD1_MIXED_REPORT="+json.dumps(result,sort_keys=True),flush=True)
    for e in errors:print("CTD1_MIXED_ERROR="+e,file=sys.stderr)
    return int(bool(errors))
if __name__=="__main__":
    sys.exit(main())
