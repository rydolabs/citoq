#!/usr/bin/env python3
"""Require rejection of a client signed by an untrusted CA in Zenoh QUIC mTLS.

This control follows a separately passing legitimate-client trial. A failure to
connect without that positive control does not, on its own, prove authentication.
"""
import argparse
import json
from pathlib import Path
import sys

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--root",type=Path,required=True)
    args=ap.parse_args()
    p=args.root/"summary.json"
    errors=[]
    if not p.is_file():
        errors.append("missing negative-control trial report")
        rows=[]
    else:
        rows=json.loads(p.read_text())
        if len(rows)!=1:
            errors.append(f"expected one negative-control trial, got {len(rows)}")
    for r in rows:
        if (r.get("engine")!="zenoh" or r.get("profile")!="baseline" or
            r.get("zenoh_transport")!="quic-mtls" or
            r.get("zenoh_auth_control")!="rogue" or
            r.get("accepted") is not False or
            r.get("received_unique",0)!=0 or
            r.get("missing")!=20 or
            r.get("receiver_exit")!=0):
            errors.append("unauthorized-client trial was accepted or not correctly labeled: "+str(r))
    logfile=args.root/"baseline-zenoh-r1-sender.log"
    log=logfile.read_text(errors="replace") if logfile.exists() else ""
    if "ZENOH_TRANSPORT=quic-mtls" not in log:
        errors.append("missing Zenoh QUIC transport mode in sender log")
    # Zenoh Publisher.put() may return successfully without a connected
    # subscriber. It is incorrect to require a nonzero sender exit here.
    # The positive counterpart delivers 200/200; this untrusted identity
    # must deliver 0/20, irrespective of sender process exit.
    report={"negative_control_rejected":not errors, "errors":errors,
            "limitation":"Legitimate-client success plus untrusted-client rejection proves this setup rejects the tested foreign CA, not comprehensive TLS policy conformance."}
    (args.root/"mtls-negative-control.json").write_text(json.dumps(report,indent=2)+"\n")
    print("ZENOH_MTLS_NEGATIVE_REPORT="+json.dumps(report,sort_keys=True),flush=True)
    for e in errors: print("ZENOH_MTLS_NEGATIVE_ERROR="+e,file=sys.stderr)
    return int(bool(errors))
if __name__=="__main__":
    sys.exit(main())
