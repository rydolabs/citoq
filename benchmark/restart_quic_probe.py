#!/usr/bin/env python3
"""Kill a live QUIC publisher, restart its Coordinator incarnation, verify post-crash data."""
import argparse
import json
import os
import pathlib
import signal
import subprocess
import sys
import time

import run_comparison as net

ROOT=pathlib.Path(__file__).resolve().parents[1]
FIRST=30
AFTER=200
EXPECTED=FIRST+AFTER

def spawn(role, output, incarnation, count, first_seq, timeout, library_dir, python):
    ns="citoq-bb" if role=="receiver" else "citoq-ba"
    base=ROOT/"benchmark-certs"
    args=[python,str(ROOT/"benchmark/bench_driver.py"),
          "citoq",role,"--count",str(count),"--payload","256",
          "--rate","100","--timeout",str(timeout),
          "--first-seq",str(first_seq),"--sender-incarnation",str(incarnation),
          "--citoq-poll-mode","immediate","--citoq-rx-mode","event",
          "--ca",str(base/"ca.pem"),"--cert",str(base/"server.pem"),
          "--key",str(base/"server.key"),
          "--client-cert",str(base/"client.pem"),
          "--client-key",str(base/"client.key"),
          "--result",str(output.with_suffix(".json"))]
    if role=="receiver":
        args+=["--recovery-first-seq",str(FIRST+1),"--emit-sequences"]
    env=["PYTHONPATH="+str(ROOT/"python"),
         "CITOQ_LIBRARY="+str(library_dir/"libcitoq.so"),
         "CITOQ_NATIVE_LIBRARY="+str(library_dir/"libcitoq_native.so")]
    cmd=["sudo","ip","netns","exec",ns,"env"]+env+args
    log=output.with_suffix(".log")
    stream=log.open("w")
    try:
        return subprocess.Popen(cmd,stdout=stream,stderr=subprocess.STDOUT,
                                start_new_session=True),stream,log
    except Exception:
        stream.close()
        raise

def terminate(proc):
    if proc is None: return
    if proc.poll() is None:
        try:os.killpg(proc.pid,signal.SIGKILL)
        except ProcessLookupError:pass
        try:proc.wait(timeout=4)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()

def run_trial(root, profile, repetition, library_dir, python):
    out=root/("trial-"+str(repetition))
    out.mkdir(parents=True,exist_ok=True)
    net.OUT=out
    receiver=first=second=None
    files=[]
    details={"profile":profile,"repetition":repetition,"expected_post_restart":AFTER}
    try:
        net.setup()
        net.set_profile(profile)
        receiver,fd,rxlog=spawn("receiver",out/"receiver",5001,EXPECTED,1,42,library_dir,python)
        files.append(fd)
        time.sleep(2.5)
        first,fd,firstlog=spawn("sender",out/"publisher-crashed",2001,FIRST,1,22,library_dir,python)
        files.append(fd)
        ready=False
        deadline=time.monotonic()+24
        while time.monotonic()<deadline:
            if firstlog.exists() and "SENDER_OK engine=citoq sent=30" in firstlog.read_text(errors="replace"):
                ready=True
                break
            if first.poll() is not None:break
            time.sleep(.05)
        details["first_sender_published_30"]=ready
        if not ready:
            details["first_sender_log_tail"]=firstlog.read_text(errors="replace")[-1000:]
            return details
        terminate(first)
        details["first_sender_forced_termination"]=first.returncode!=0
        details["first_sender_exit"]=first.returncode
        time.sleep(.75)
        second,fd,secondlog=spawn("sender",out/"publisher-restarted",2002,AFTER,FIRST+1,25,library_dir,python)
        files.append(fd)
        try:second.wait(timeout=34)
        except subprocess.TimeoutExpired:terminate(second)
        details["restarted_sender_exit"]=second.returncode
        try:receiver.wait(timeout=43)
        except subprocess.TimeoutExpired:terminate(receiver)
        details["receiver_exit"]=receiver.returncode
        report=out/"receiver.json"
        data=json.loads(report.read_text()) if report.exists() else {}
        sequences=data.get("received_sequences",[])
        recovered={x for x in sequences if FIRST+1<=x<=EXPECTED}
        details["first_phase_received"]=sum(x<=FIRST for x in sequences)
        details["recovered_unique"]=len(recovered)
        details["post_restart_missing"]=AFTER-len(recovered)
        details["recovery_p95_us"]=data.get("recovery_p95_us")
        details["invalid"]=data.get("invalid")
        details["duplicates"]=data.get("duplicates")
        details["complete"]=(
            details["first_sender_forced_termination"] and
            details["restarted_sender_exit"]==0 and
            details["receiver_exit"]==0 and
            len(recovered)==AFTER and len(sequences)==len(set(sequences)) and
            data.get("invalid")==0 and data.get("duplicates")==0)
        if not details["complete"]:
            details["restarted_sender_log_tail"]=secondlog.read_text(errors="replace")[-900:]
            details["receiver_log_tail"]=rxlog.read_text(errors="replace")[-900:]
        return details
    finally:
        for proc in (first,second,receiver):terminate(proc)
        for fd in files:fd.close()
        for ns in net.NS:net.cmd("sudo","ip","netns","del",ns,check=False)
        net.cmd("sudo","ip","link","del","citoq-br",check=False)

def main():
    p=argparse.ArgumentParser()
    p.add_argument("--profile",required=True,choices=("netem_10ms_1pct","netem_10ms_2pct"))
    p.add_argument("--repeats",type=int,default=3)
    p.add_argument("--python",default=sys.executable)
    p.add_argument("--library-dir",type=pathlib.Path,default=ROOT/"build-benchmark")
    p.add_argument("--output-dir",type=pathlib.Path,required=True)
    args=p.parse_args()
    args.output_dir.mkdir(parents=True,exist_ok=True)
    results=[]
    for i in range(1,args.repeats+1):
        try:
            row=run_trial(args.output_dir,args.profile,i,args.library_dir.resolve(),args.python)
        except Exception as exc:
            row={"profile":args.profile,"repetition":i,"complete":False,
                 "exception":str(exc)}
        results.append(row)
        print("RESTART_TRIAL="+json.dumps(row,sort_keys=True),flush=True)
    summary={"profile":args.profile,"trials":len(results),
             "passed":sum(bool(x.get("complete")) for x in results),
             "results":results,
             "limits":[
                 "Forced SIGKILL of sender1 after 30 accepted publish calls.",
                 "The initial 30 in-flight messages may be lost on deliberate crash.",
                 "Only the 200 new sequence IDs after restart are strictly required.",
                 "Same persistent subscriber, new client Coordinator incarnation, authenticated QUIC.",
                 "A publish call is not a network-delivery acknowledgement."
             ]}
    (args.output_dir/"reconnect-report.json").write_text(json.dumps(summary,indent=2)+"\n")
    print("RESTART_SUMMARY="+json.dumps(summary,sort_keys=True),flush=True)
    return int(summary["passed"]!=args.repeats)

if __name__=="__main__":
    sys.exit(main())
