#!/usr/bin/env python3
"""Force receiver/Coordinator restart during an active authenticated PubSub session."""
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

def launch(role, out, python, library, incarnation, signal_after=0,
           signal_file=None, pause_after=0, resume_file=None,
           recovery_first_seq=0):
    ns="citoq-bb" if role=="receiver" else "citoq-ba"
    cert=ROOT/"benchmark-certs"
    args=[python,str(ROOT/"benchmark/bench_driver.py"),"citoq",role,
          "--count",str(EXPECTED),"--payload","256","--rate","100",
          "--timeout","45","--cio","none"] if False else [
          python,str(ROOT/"benchmark/bench_driver.py"),"citoq",role,
          "--count",str(EXPECTED),"--payload","256","--rate","100",
          "--timeout","45",
          "--sender-incarnation","2001","--server-incarnation",str(incarnation),
          "--citoq-poll-mode","immediate","--citoq-rx-mode","event",
          "--ca",str(cert/"ca.pem"),"--cert",str(cert/"server.pem"),
          "--key",str(cert/"server.key"),
          "--client-cert",str(cert/"client.pem"),
          "--client-key",str(cert/"client.key"),
          "--result",str(out.with_suffix(".json"))]
    if role=="sender":
        args+=["--pause-after",str(pause_after),"--resume-file",str(resume_file)]
    else:
        args+=["--emit-sequences","--recovery-first-seq",str(recovery_first_seq)]
        if signal_after:
            args+=["--signal-after",str(signal_after),"--signal-file",str(signal_file)]
    env=["PYTHONPATH="+str(ROOT/"python"),
         "CITOQ_LIBRARY="+str(library/"libcitoq.so"),
         "CITOQ_NATIVE_LIBRARY="+str(library/"libcitoq_native.so")]
    command=["sudo","ip","netns","exec",ns,"env"]+env+args
    f=out.with_suffix(".log").open("w")
    try:
        p=subprocess.Popen(command,stdout=f,stderr=subprocess.STDOUT,start_new_session=True)
        return p,f
    except Exception:
        f.close()
        raise

def terminate(p):
    if p is None: return
    if p.poll() is None:
        try:os.killpg(p.pid,signal.SIGKILL)
        except ProcessLookupError:pass
        try:p.wait(timeout=4)
        except subprocess.TimeoutExpired:p.kill();p.wait()

def run(root,profile,index,python,library):
    out=root/("trial-"+str(index))
    out.mkdir(parents=True,exist_ok=True)
    net.OUT=out
    processes=[]
    streams=[]
    details={"profile":profile,"trial":index,"post_restart_required":AFTER}
    marker=out/"first_receiver_30.marker"
    resume=out/"continue_after_restart.marker"
    try:
        net.setup()
        net.set_profile(profile)
        rx1,f=launch("receiver",out/"receiver-before-kill",python,library,5001,
                     signal_after=FIRST,signal_file=marker)
        processes.append(rx1);streams.append(f)
        time.sleep(2.5)
        sender,f=launch("sender",out/"sender-continuous",python,library,5001,
                        pause_after=FIRST,resume_file=resume)
        processes.append(sender);streams.append(f)
        deadline=time.monotonic()+28
        while time.monotonic()<deadline:
            paused="SENDER_PAUSE_AT=30" in (out/"sender-continuous.log").read_text(errors="replace")
            if marker.is_file() and paused:
                break
            if sender.poll() is not None or rx1.poll() is not None:break
            time.sleep(.05)
        details["initial_30_received"]=marker.is_file()
        details["sender_paused"]=("SENDER_PAUSE_AT=30" in
            (out/"sender-continuous.log").read_text(errors="replace"))
        if not (details["initial_30_received"] and details["sender_paused"]):
            details["complete"]=False
            details["initial_failure"]="phase barrier not reached"
            return details
        terminate(rx1)
        details["receiver_was_killed"]=rx1.returncode!=0
        rx2,f=launch("receiver",out/"receiver-after-restart",python,library,5002,
                     recovery_first_seq=FIRST+1)
        processes.append(rx2);streams.append(f)
        # Let new Coordinator advertise its new incarnation and complete
        # subscriber registration before releasing the paused old publisher.
        time.sleep(2.5)
        resume.write_text("continue\n")
        try:sender.wait(timeout=43)
        except subprocess.TimeoutExpired:terminate(sender)
        try:rx2.wait(timeout=46)
        except subprocess.TimeoutExpired:terminate(rx2)
        details["sender_exit"]=sender.returncode
        details["receiver_restarted_exit"]=rx2.returncode
        file=out/"receiver-after-restart.json"
        data=json.loads(file.read_text()) if file.is_file() else {}
        new={x for x in data.get("received_sequences",[]) if FIRST+1<=x<=EXPECTED}
        details["post_restart_received"]=len(new)
        details["post_restart_missing"]=AFTER-len(new)
        details["duplicates"]=data.get("duplicates")
        details["invalid"]=data.get("invalid")
        details["p95_post_restart_us"]=data.get("recovery_p95_us")
        details["complete"]=(
            details["receiver_was_killed"] and
            details["sender_exit"]==0 and details["receiver_restarted_exit"]==0 and
            len(new)==AFTER and data.get("duplicates")==0 and data.get("invalid")==0
        )
        if not details["complete"]:
            for kind in ("sender-continuous","receiver-after-restart"):
                details[kind+"_tail"]=(out/(kind+".log")).read_text(errors="replace")[-1100:]
        return details
    finally:
        for p in processes:terminate(p)
        for f in streams:f.close()
        for ns in net.NS:net.cmd("sudo","ip","netns","del",ns,check=False)
        net.cmd("sudo","ip","link","del","citoq-br",check=False)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--profile",required=True,choices=["netem_10ms_1pct","netem_10ms_2pct"])
    ap.add_argument("--python",default=sys.executable)
    ap.add_argument("--library-dir",type=pathlib.Path,required=True)
    ap.add_argument("--repeats",type=int,default=3)
    ap.add_argument("--output-dir",type=pathlib.Path,required=True)
    args=ap.parse_args()
    args.output_dir.mkdir(parents=True,exist_ok=True)
    results=[]
    for index in range(1,args.repeats+1):
        try:res=run(args.output_dir,args.profile,index,args.python,args.library_dir.resolve())
        except Exception as e:
            res={"profile":args.profile,"trial":index,"complete":False,"exception":str(e)}
        results.append(res)
        print("RECEIVER_RESTART_TRIAL="+json.dumps(res,sort_keys=True),flush=True)
    accepted=sum(bool(r.get("complete")) for r in results)
    summary={"profile":args.profile,"trials":len(results),"accepted":accepted,
             "post_restart_delivered":sum(r.get("post_restart_received",0) for r in results),
             "results":results,
             "limits":[
                "The initially received 30 messages are a phase barrier, not a durability guarantee.",
                "The receiver/Coordinator process is SIGKILLed and restarted with new incarnation.",
                "The same publisher remains alive and resumes after new Coordinator startup.",
                "Only the 200 new sequence IDs post-restart must arrive without duplication/corruption.",
                "The observed rate includes a deliberate pause and is not throughput evidence."
            ]}
    (args.output_dir/"receiver-restart-report.json").write_text(
        json.dumps(summary,indent=2)+"\n")
    print("RECEIVER_RESTART_SUMMARY="+json.dumps(summary,sort_keys=True),flush=True)
    return int(accepted!=args.repeats)

if __name__=="__main__":
    sys.exit(main())
